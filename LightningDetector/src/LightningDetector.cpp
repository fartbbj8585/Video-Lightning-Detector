/*  LightningDetector.cpp
    Core lightning detection logic using OpenCV.

    Algorithm overview
    
    1.  Read every frame of the video via cv::VideoCapture (with hardware-
        accelerated decode when available).
    2.  Convert to grayscale and compute a running "background" via a
        lightweight exponential moving average (EMA).
    3.  Subtract background from current frame -> difference image.
    4.  Count pixels whose difference exceeds brightness_threshold.
    5.  If the fraction of hot pixels exceeds pixel_fraction_required we have
        a candidate flash frame.
    6.  Consecutive candidate frames are merged into one LightningEvent; the
        peak-intensity frame determines the reported timestamp.
    7.  Confidence is derived from the peak fraction and the spatial
        distribution of bright pixels (spread across the frame = higher conf).
    8.  Shape of the bright region also determines the TYPE of event:
        a thin band stretching across most of the frame width but with very
        little vertical extent is a software/codec artifact (screen
        tearing), not a real flash, and is labelled accordingly.

    Steps 2-4 run on the GPU via OpenCV's Transparent API (cv::UMat) when an
    OpenCL-capable GPU/driver is available, which is the "use GPU" feature -
    this offloads color conversion, frame differencing, and thresholding
    from the CPU, cutting CPU usage substantially. If no GPU/OpenCL is
    available, UMat falls back to an optimized CPU path automatically, so
    results are identical either way - only speed/CPU load differs.

    This approach is:
    - Environment-agnostic  - works for day/night/indoor/outdoor.
    - Fast                  - single-pass, no neural net, vectorised via OpenCV.
    - Parallelisable        - each video processed on its own thread.
*/

#include "LightningDetector.h"

#include <opencv2/opencv.hpp>
#include <opencv2/core/ocl.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;
namespace fs = std::filesystem;

// 
//  Helpers
// 

static std::string basename_of(const std::string& path) {
    return fs::path(path).filename().string();
}

// Spatial shape of the lit-pixel region within a flash mask.
struct ShapeMetrics {
    float spread     = 0.f;   // std-dev of lit-pixel coords, normalised by frame diagonal
    float width_frac  = 0.f;  // bounding-box width  / frame width
    float height_frac = 0.f;  // bounding-box height / frame height
};

static ShapeMetrics computeShapeMetrics(const cv::Mat& mask) {
    ShapeMetrics sm;
    std::vector<cv::Point> pts;
    cv::findNonZero(mask, pts);
    if (pts.size() < 4) return sm;

    double mx = 0, my = 0;
    int minX = mask.cols, maxX = 0, minY = mask.rows, maxY = 0;
    for (auto& p : pts) {
        mx += p.x; my += p.y;
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    mx /= pts.size(); my /= pts.size();

    double var = 0;
    for (auto& p : pts)
        var += (p.x - mx)*(p.x - mx) + (p.y - my)*(p.y - my);
    var /= pts.size();

    float diag = std::sqrt((float)(mask.cols*mask.cols + mask.rows*mask.rows));
    sm.spread      = (float)(std::sqrt(var) / diag);
    sm.width_frac  = (float)(maxX - minX) / (float)mask.cols;
    sm.height_frac = (float)(maxY - minY) / (float)mask.rows;
    return sm;
}

// Classify the event type. Checked in priority order: a screen-tearing
// artifact (thin band spanning most of the frame width but barely any
// height) is checked FIRST, since it can otherwise look like a confident
// flash to the brightness/fraction logic alone.
static std::string classify_lightning(float confidence, const ShapeMetrics& sm) {
    const bool looksLikeTearing =
        sm.width_frac  > 0.60f &&   // spans most of the frame horizontally
        sm.height_frac < 0.12f;     // but only a thin horizontal slice

    if (looksLikeTearing)        return "screen tearing";
    if (confidence > 0.85f)      return "Cloud to ground";
    if (confidence > 0.60f)      return "Intra-cloud";
    return "flash";
}

// 
//  LightningDetector
// 

LightningDetector::LightningDetector()  = default;
LightningDetector::~LightningDetector() = default;

//  Single video 

VideoResult LightningDetector::processSingleVideo(
    const std::string& path,
    int video_index, int total_videos,
    ProgressCallback progress_cb)
{
    VideoResult result;
    result.video_path     = path;
    result.video_filename = basename_of(path);
    result.success        = false;

    auto t_start = std::chrono::steady_clock::now();

    cv::VideoCapture cap;
    cap.open(path, cv::CAP_FFMPEG);
    // Ask FFmpeg to use hardware-accelerated decoding when the system
    // supports it (D3D11VA / DXVA2 on Windows, falls back silently to
    // software decode if no compatible GPU/driver is present).
    cap.set(cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY);
    if (!cap.isOpened()) {
        result.error_message = "Cannot open video: " + path;
        return result;
    }

    result.fps          = cap.get(cv::CAP_PROP_FPS);
    result.total_frames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (result.fps <= 0) result.fps = 30.0;
    result.duration_seconds = result.total_frames / result.fps;

    const float  BRIGHT_THRESH  = params_.brightness_threshold;
    const float  PIX_FRAC       = params_.pixel_fraction_required;
    const int    GAP             = params_.min_event_gap_frames;
    const bool   USE_GPU         = params_.use_gpu;

    // Determine frame-skip stride so analysis runs at ~target_analysis_fps.
    int stride = 1;
    if (params_.target_analysis_fps > 0.0 && result.fps > params_.target_analysis_fps * 1.5) {
        stride = (int)std::round(result.fps / params_.target_analysis_fps);
        if (stride < 1) stride = 1;
    }
    const int GAP_STRIDED = std::max(1, GAP / stride);

    // Determine downscale factor (lightning is a large, global brightness
    // event, so downscaling barely affects detection while cutting
    // per-frame processing cost roughly with the square of the scale).
    int src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double scale = 1.0;
    if (params_.analysis_max_dimension > 0 && src_w > 0 && src_h > 0) {
        int longest = std::max(src_w, src_h);
        if (longest > params_.analysis_max_dimension)
            scale = (double)params_.analysis_max_dimension / (double)longest;
    }
    cv::Size analysis_size;
    if (scale < 1.0) {
        analysis_size = cv::Size(
            std::max(2, (int)std::lround(src_w * scale)),
            std::max(2, (int)std::lround(src_h * scale)));
    }

    // GPU-resident (UMat) working buffers. When USE_GPU is true and an
    // OpenCL device is available, OpenCV transparently runs these ops on
    // the GPU; otherwise UMat falls back to an optimised CPU path with
    // identical numerical results.
    cv::Mat  frame;
    cv::UMat uFrame, uSmall, uGray, uBackground, uGf, uDiff, uMask;
    const float EMA_ALPHA = 0.05f;   // how fast background adapts

    //  Candidate flash state 
    bool  in_event          = false;
    float peak_fraction     = 0.f;
    ShapeMetrics peak_shape;
    int   peak_frame        = 0;
    int   last_event_end    = -GAP_STRIDED - 1;

    int frame_idx      = 0;
    int analysed_count = 0;
    int report_every = std::max(1, (result.total_frames / std::max(1, stride)) / 200);

    auto closeEvent = [&](std::vector<LightningEvent>& events) {
        float base_conf = std::min(1.f, peak_fraction / 0.20f);
        float conf      = base_conf * 0.70f + peak_shape.spread * 0.30f;
        conf            = std::min(1.f, std::max(0.01f, conf));
        LightningEvent ev;
        ev.frame_number      = peak_frame;
        ev.timestamp_seconds = peak_frame / result.fps;
        ev.confidence        = conf;
        ev.description       = classify_lightning(conf, peak_shape);
        events.push_back(ev);
    };

    while (true) {
        if (stride > 1) {
            if (!cap.grab()) break;
            if (frame_idx % stride != 0) { ++frame_idx; continue; }
            if (!cap.retrieve(frame))    break;
        } else {
            if (!cap.read(frame)) break;
        }

        frame.copyTo(uFrame); // upload to GPU memory (no-op / cheap copy if no GPU)
        const cv::UMat* analyse_frame = &uFrame;
        if (scale < 1.0) {
            cv::resize(uFrame, uSmall, analysis_size, 0, 0, cv::INTER_AREA);
            analyse_frame = &uSmall;
        }

        cv::cvtColor(*analyse_frame, uGray, cv::COLOR_BGR2GRAY);

        if (uBackground.empty()) {
            uGray.convertTo(uBackground, CV_32F);
        } else {
            uGray.convertTo(uGf, CV_32F);
            cv::absdiff(uGf, uBackground, uDiff);

            // Update background (EMA) BEFORE deciding if it's lightning so
            // the background doesn't "chase" the flash.
            cv::addWeighted(uBackground, 1.f - EMA_ALPHA, uGf, EMA_ALPHA, 0, uBackground);

            cv::threshold(uDiff, uMask, BRIGHT_THRESH, 255, cv::THRESH_BINARY);
            uMask.convertTo(uMask, CV_8U);

            int   hot_pixels  = cv::countNonZero(uMask);
            float total_pix   = (float)(uMask.rows * uMask.cols);
            float fraction    = hot_pixels / total_pix;

            bool is_flash = fraction >= PIX_FRAC;

            if (is_flash && (in_event || (analysed_count - last_event_end) >= GAP_STRIDED)) {
                // Only pull the mask back to the CPU (and compute its
                // shape) on frames that are actually candidates - this
                // keeps the GPU-resident fast path branch-free for the
                // (vastly more common) non-flash frames.
                cv::Mat hostMask = uMask.getMat(cv::ACCESS_READ);
                ShapeMetrics sm = computeShapeMetrics(hostMask);

                if (!in_event) {
                    in_event      = true;
                    peak_fraction = fraction;
                    peak_shape    = sm;
                    peak_frame    = frame_idx;
                } else if (fraction > peak_fraction) {
                    peak_fraction = fraction;
                    peak_shape    = sm;
                    peak_frame    = frame_idx;
                }
            } else if (!is_flash && in_event) {
                last_event_end = analysed_count;
                in_event       = false;
                closeEvent(result.events);
            }
        }

        ++frame_idx;
        ++analysed_count;

        if (progress_cb && (analysed_count % report_every == 0)) {
            progress_cb(video_index, total_videos, frame_idx,
                        result.total_frames,
                        "Analysing " + result.video_filename +
                        (USE_GPU && cv::ocl::useOpenCL() ? " (GPU)" : " (CPU)"));
        }
    }

    if (in_event) {
        closeEvent(result.events);
    }

    cap.release();

    auto t_end = std::chrono::steady_clock::now();
    result.processing_time_seconds =
        std::chrono::duration<double>(t_end - t_start).count();

    result.success = true;
    return result;
}

//  Multi-video parallel dispatcher 

std::vector<VideoResult> LightningDetector::analyseVideos(
    const std::vector<std::string>& video_paths,
    ProgressCallback progress_cb)
{
    // GPU on/off is a global OpenCV setting - applied once, before any
    // worker threads start, so it stays consistent across the whole run.
    cv::ocl::setUseOpenCL(params_.use_gpu);

    int n = (int)video_paths.size();
    std::vector<VideoResult> results(n);

    int max_t = params_.max_threads > 0
        ? params_.max_threads
        : (int)std::thread::hardware_concurrency();
    if (max_t < 1) max_t = 2;

    std::mutex mtx_cb;

    auto safe_cb = [&](int vi, int vt, int fd, int ft, const std::string& msg) {
        if (!progress_cb) return;
        std::lock_guard<std::mutex> lk(mtx_cb);
        progress_cb(vi, vt, fd, ft, msg);
    };

    std::atomic<int> next_idx{0};

    auto worker = [&]() {
        while (true) {
            int idx = next_idx.fetch_add(1);
            if (idx >= n) break;
            results[idx] = processSingleVideo(
                video_paths[idx], idx, n,
                [&, idx](int vi, int vt, int fd, int ft, const std::string& msg) {
                    safe_cb(vi, vt, fd, ft, msg);
                });
            safe_cb(idx, n, results[idx].total_frames, results[idx].total_frames,
                    "Done: " + results[idx].video_filename);
        }
    };

    std::vector<std::thread> threads;
    int spawn = std::min(max_t, n);
    for (int i = 0; i < spawn; ++i)
        threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    return results;
}

//  JSON serialisation 

bool LightningDetector::saveJSON(
    const std::vector<VideoResult>& results,
    const std::string& output_path)
{
    json root;
    root["generator"]  = "LightningDetector v1.0";
    root["total_videos_processed"] = (int)results.size();

    json arr = json::array();
    for (auto& r : results) {
        json vj;
        vj["file"]                    = r.video_filename;
        vj["path"]                    = r.video_path;
        vj["success"]                 = r.success;
        vj["duration_seconds"]        = r.duration_seconds;
        vj["total_frames"]            = r.total_frames;
        vj["fps"]                     = r.fps;
        vj["processing_time_seconds"] = std::round(r.processing_time_seconds * 1000.0) / 1000.0;
        vj["lightning_event_count"]   = (int)r.events.size();
        if (!r.error_message.empty())
            vj["error"] = r.error_message;

        json evs = json::array();
        for (auto& ev : r.events) {
            int    h   = (int)(ev.timestamp_seconds / 3600);
            int    m   = (int)(ev.timestamp_seconds / 60) % 60;
            double s   = std::fmod(ev.timestamp_seconds, 60.0);
            std::ostringstream ts;
            ts << std::setfill('0') << std::setw(2) << h << ":"
               << std::setw(2) << m << ":"
               << std::setw(6) << std::fixed << std::setprecision(3) << s;

            json ej;
            ej["frame"]             = ev.frame_number;
            ej["timestamp"]         = ts.str();
            ej["timestamp_seconds"] = std::round(ev.timestamp_seconds * 1000.0) / 1000.0;
            ej["confidence"]        = std::round(ev.confidence * 10000.0f) / 10000.0f;
            ej["confidence_pct"]    = std::round(ev.confidence * 10000.0f) / 100.0f;
            ej["type"]              = ev.description;
            evs.push_back(ej);
        }
        vj["lightning_events"] = evs;
        arr.push_back(vj);
    }
    root["videos"] = arr;

    std::ofstream f(output_path);
    if (!f.is_open()) return false;
    f << root.dump(2);
    return true;
}
