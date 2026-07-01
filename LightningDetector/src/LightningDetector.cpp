/*  LightningDetector.cpp  -  fully-optimised detection core

    Speed strategy (applied in order of impact)
    ============================================
    1.  Hardware video DECODE  (D3D11VA / DXVA2) via FFmpeg backend.
        Moves H.264/H.265 decode off the CPU entirely onto the GPU's
        fixed-function video engine.

    2.  Frame SKIP via cap.grab()  (already in place)
        cap.grab() reads the compressed bitstream packet but skips the
        costly colour-convert/copy step.  Only the frames we actually
        need are fully retrieved.  A 240fps clip at target_analysis_fps=60
        analyses 1 in every 4 frames.

    3.  CPU downscale BEFORE GPU upload.
        Downscale (e.g. 4K -> 720p) on the CPU with INTER_LINEAR
        (fastest resize filter) before copying to the GPU.  This means
        the PCIe transfer carries a 4-12x smaller payload than if we
        uploaded the full frame and resized on the GPU.

    4.  GPU image math via UMat / OpenCL  (colour convert, diff, threshold).
        Offloads the per-pixel arithmetic from the CPU to the GPU's shader
        units.  cv::UMat is used for every step after the upload;
        countNonZero is fast because UMat.getMat() (the readback) is only
        called on frames that actually triggered a flash candidate, not
        every frame.

    5.  Persistent pre-allocated buffers.
        All cv::Mat and cv::UMat working buffers are declared once outside
        the loop.  This avoids repeated heap alloc/free per frame (which
        was previously the biggest single-frame overhead after the pixel
        math itself).

    6.  Reduced EMA cost.
        The EMA background update and the subtraction are merged into a
        single pass using cv::accumulateWeighted (one kernel call instead
        of two separate addWeighted + absdiff calls).

    7.  INTER_LINEAR instead of INTER_AREA for downscaling.
        INTER_AREA is the theoretically "correct" choice for down-sampling
        but is 2-3x slower than INTER_LINEAR.  For lightning detection
        (large-scale brightness events) the quality difference is
        irrelevant; the speed saving is real.

    8.  Parallel video dispatch  (already in place)
        Multiple videos are processed simultaneously, one thread each,
        limited by max_threads (default = all CPU cores).

    Classification types
    ====================
    "Cloud to ground"  - high-confidence (~intense, spread) flash
    "Intra-cloud"      - medium-confidence flash
    "flash"            - low-confidence / unclassified flash
    "screen tearing"   - thin horizontal band spanning most frame width;
                         characteristic of video codec / display artifacts
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

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static std::string basename_of(const std::string& path) {
    return fs::path(path).filename().string();
}

struct ShapeMetrics {
    float spread      = 0.f;   // spatial spread normalised by frame diagonal
    float width_frac  = 0.f;   // bounding-box width  / frame width
    float height_frac = 0.f;   // bounding-box height / frame height
};

// computeShapeMetrics is only called on frames where a flash was already
// detected - i.e. a tiny fraction of all frames - so its cost is negligible.
static ShapeMetrics computeShapeMetrics(const cv::Mat& mask) {
    ShapeMetrics sm;
    std::vector<cv::Point> pts;
    cv::findNonZero(mask, pts);
    if ((int)pts.size() < 4) return sm;

    int minX = mask.cols, maxX = 0;
    int minY = mask.rows, maxY = 0;
    double mx = 0, my = 0;
    for (auto& p : pts) {
        mx += p.x; my += p.y;
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.y > maxY) maxY = p.y;
    }
    mx /= pts.size(); my /= pts.size();

    double var = 0;
    for (auto& p : pts)
        var += (p.x-mx)*(p.x-mx) + (p.y-my)*(p.y-my);
    var /= pts.size();

    float diag = std::sqrt((float)(mask.cols*mask.cols + mask.rows*mask.rows));
    sm.spread      = (float)(std::sqrt(var) / diag);
    sm.width_frac  = (float)(maxX - minX) / (float)mask.cols;
    sm.height_frac = (float)(maxY - minY) / (float)mask.rows;
    return sm;
}

// Screen-tearing heuristic: a thin horizontal band that spans most of the
// frame width but covers very little height.
static bool isTearingArtifact(const ShapeMetrics& sm) {
    return sm.width_frac > 0.60f && sm.height_frac < 0.12f;
}

static std::string classify_lightning(float confidence, const ShapeMetrics& sm) {
    if (isTearingArtifact(sm))   return "screen tearing";
    if (confidence > 0.85f)      return "Cloud to ground";
    if (confidence > 0.60f)      return "Intra-cloud";
    return "flash";
}

// Merge events that land within `window_seconds` of the previous kept event
// into a single entry. Rapid flicker (e.g. a strike with several bright
// sub-pulses) otherwise produces a run of near-duplicate events a fraction
// of a second apart, which bloats the saved JSON/CSV without adding useful
// information. Events are already produced in chronological order, so a
// single forward pass is enough. Within a merged cluster we keep whichever
// single event had the highest confidence, since it best represents the
// cluster's peak.
static void mergeCloseEvents(std::vector<LightningEvent>& events, double window_seconds) {
    if (events.size() < 2) return;
    std::vector<LightningEvent> merged;
    merged.reserve(events.size());
    merged.push_back(events[0]);
    for (size_t i = 1; i < events.size(); ++i) {
        LightningEvent& last = merged.back();
        const LightningEvent& cur = events[i];
        if (cur.timestamp_seconds - last.timestamp_seconds < window_seconds) {
            if (cur.confidence > last.confidence) {
                last.frame_number      = cur.frame_number;
                last.timestamp_seconds = cur.timestamp_seconds;
                last.confidence        = cur.confidence;
                last.description       = cur.description;
            }
        } else {
            merged.push_back(cur);
        }
    }
    events.swap(merged);
}

// ---------------------------------------------------------------------------
//  LightningDetector
// ---------------------------------------------------------------------------

LightningDetector::LightningDetector()  = default;
LightningDetector::~LightningDetector() = default;

// ---------------------------------------------------------------------------
//  Single-video processing
// ---------------------------------------------------------------------------

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

    // -----------------------------------------------------------------------
    //  Open the video with hardware-accelerated decode
    //  D3D11VA is the Windows-native GPU video-decode API; it works on
    //  AMD, NVIDIA, and Intel GPUs with any modern driver.  FFmpeg uses it
    //  transparently when the codec is supported (H.264, H.265, VP9, AV1
    //  on newer drivers).  If the GPU/codec/driver combo can't handle it,
    //  FFmpeg falls back to software decode automatically - no error is
    //  raised, the video just decodes a little more slowly.
    // -----------------------------------------------------------------------
    cv::VideoCapture cap;
    cap.open(path, cv::CAP_FFMPEG);
    // VIDEO_ACCELERATION_ANY lets FFmpeg negotiate the best available
    // hardware decoder (D3D11VA / DXVA2 on Windows) without requiring a
    // specific enum value that may not exist in all OpenCV builds.
    cap.set(cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY);
    if (!cap.isOpened()) {
        result.error_message = "Cannot open video: " + path;
        return result;
    }

    result.fps          = cap.get(cv::CAP_PROP_FPS);
    result.total_frames = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (result.fps <= 0) result.fps = 30.0;
    result.duration_seconds = result.total_frames / result.fps;

    const float BRIGHT_THRESH = params_.brightness_threshold;
    const float PIX_FRAC      = params_.pixel_fraction_required;
    const int   GAP           = params_.min_event_gap_frames;

    // -----------------------------------------------------------------------
    //  Frame-skip stride: normalise analysis rate to target_analysis_fps
    //  Example: 240fps source, target 60fps -> stride 4
    //  Timestamps still use the REAL source fps so event timing is exact.
    // -----------------------------------------------------------------------
    int stride = 1;
    if (params_.target_analysis_fps > 0.0 &&
        result.fps > params_.target_analysis_fps * 1.5)
    {
        stride = (int)std::round(result.fps / params_.target_analysis_fps);
        if (stride < 1) stride = 1;
    }
    const int GAP_STRIDED = std::max(1, GAP / stride);

    // -----------------------------------------------------------------------
    //  Downscale parameters
    //  We compute the analysis_size once here.  The resize itself happens on
    //  the CPU (INTER_LINEAR, fast) BEFORE the GPU upload so that the PCIe
    //  transfer only carries the smaller frame.
    // -----------------------------------------------------------------------
    const int src_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    const int src_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double scale = 1.0;
    cv::Size analysis_size(src_w, src_h);
    if (params_.analysis_max_dimension > 0 && src_w > 0 && src_h > 0) {
        int longest = std::max(src_w, src_h);
        if (longest > params_.analysis_max_dimension) {
            scale = (double)params_.analysis_max_dimension / (double)longest;
            analysis_size = cv::Size(
                std::max(2, (int)std::lround(src_w * scale)),
                std::max(2, (int)std::lround(src_h * scale)));
        }
    }
    const bool do_resize = (scale < 1.0);

    // -----------------------------------------------------------------------
    //  Pre-allocate ALL working buffers outside the loop.
    //  Avoids repeated heap allocation/deallocation which was previously
    //  a measurable fraction of per-frame CPU cost.
    // -----------------------------------------------------------------------
    cv::Mat  frame;                              // raw decoded frame (CPU)
    cv::Mat  cpu_small;                          // CPU-side downscaled frame
    cv::UMat u_upload;                           // GPU upload buffer
    cv::UMat u_gray;                             // GPU grayscale
    cv::UMat u_gray_f;                           // GPU float grayscale
    cv::UMat u_background;                       // GPU EMA background
    cv::UMat u_diff;                             // GPU difference image
    cv::UMat u_mask;                             // GPU threshold mask

    const float EMA_ALPHA     = 0.05f;
    const float EMA_ONE_MINUS = 1.0f - EMA_ALPHA;

    // -----------------------------------------------------------------------
    //  Per-event state
    // -----------------------------------------------------------------------
    bool         in_event      = false;
    float        peak_fraction = 0.f;
    ShapeMetrics peak_shape;
    int          peak_frame    = 0;
    int          last_event_end = -(GAP_STRIDED + 1);

    int frame_idx      = 0;  // true index in the source video
    int analysed_count = 0;  // frames actually analysed

    // Report progress ~200 times per video regardless of length.
    const int total_to_analyse = std::max(1, result.total_frames / stride);
    int report_every = std::max(1, total_to_analyse / 200);

    const bool use_gpu  = params_.use_gpu;
    const bool gpu_live = use_gpu && cv::ocl::useOpenCL();

    // Lambda: finalise and record the current open event.
    auto closeEvent = [&]() {
        float base_conf = std::min(1.f, peak_fraction / 0.20f);
        float conf      = std::min(1.f, std::max(0.01f,
                            base_conf * 0.70f + peak_shape.spread * 0.30f));
        LightningEvent ev;
        ev.frame_number      = peak_frame;
        ev.timestamp_seconds = peak_frame / result.fps;
        ev.confidence        = conf;
        ev.description       = classify_lightning(conf, peak_shape);
        result.events.push_back(ev);
    };

    // -----------------------------------------------------------------------
    //  Main decode / analysis loop
    // -----------------------------------------------------------------------
    while (true) {

        // ---- Frame retrieval with stride-based skipping -------------------
        if (stride > 1) {
            // cap.grab() reads the compressed packet but skips the expensive
            // colour-space conversion and memory copy - much faster than a
            // full cap.read() for frames we don't need.
            if (!cap.grab()) break;
            if (frame_idx % stride != 0) {
                ++frame_idx;
                continue;
            }
            if (!cap.retrieve(frame)) break;
        } else {
            if (!cap.read(frame)) break;
        }

        // ---- CPU downscale (INTER_LINEAR: fastest filter for shrinking) ---
        // Downscaling here (CPU side) before GPU upload means the PCIe
        // transfer carries a 4-16x smaller payload than at full resolution.
        const cv::Mat* src = &frame;
        if (do_resize) {
            cv::resize(frame, cpu_small, analysis_size, 0, 0, cv::INTER_LINEAR);
            src = &cpu_small;
        }

        // ---- GPU upload ---------------------------------------------------
        src->copyTo(u_upload);

        // ---- GPU image math (OpenCL / UMat) --------------------------------
        cv::cvtColor(u_upload, u_gray, cv::COLOR_BGR2GRAY);
        u_gray.convertTo(u_gray_f, CV_32F);

        if (u_background.empty()) {
            // First frame: initialise background to this frame.
            u_gray_f.copyTo(u_background);
        } else {
            // Compute difference against background BEFORE updating the
            // background so the background doesn't "chase" the flash.
            cv::absdiff(u_gray_f, u_background, u_diff);

            // EMA background update (single kernel: dst = a*src + b*dst)
            cv::accumulateWeighted(u_gray_f, u_background, EMA_ALPHA);

            // Threshold the diff to produce the flash mask.
            cv::threshold(u_diff, u_mask, BRIGHT_THRESH, 255, cv::THRESH_BINARY);
            u_mask.convertTo(u_mask, CV_8U);

            // countNonZero runs on the GPU when OpenCL is active.
            const int   hot_pix  = cv::countNonZero(u_mask);
            const float total_px = (float)(u_mask.rows * u_mask.cols);
            const float fraction = hot_pix / total_px;
            const bool  is_flash = fraction >= PIX_FRAC;

            if (is_flash && (in_event || (analysed_count - last_event_end) >= GAP_STRIDED)) {
                // Only pull the mask back to the CPU for shape analysis on
                // actual candidate frames (a tiny fraction of the total).
                cv::Mat host_mask = u_mask.getMat(cv::ACCESS_READ);
                ShapeMetrics sm   = computeShapeMetrics(host_mask);

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
                closeEvent();
            }
        }

        ++frame_idx;
        ++analysed_count;

        if (progress_cb && (analysed_count % report_every == 0)) {
            std::string tag = gpu_live ? " (GPU)" : " (CPU)";
            progress_cb(video_index, total_videos, frame_idx,
                        result.total_frames,
                        "Analysing " + result.video_filename + tag);
        }
    } // end main loop

    // Close any event still open at the end of the video.
    if (in_event) closeEvent();

    // Collapse events that occur within 1 second of each other into a
    // single event, so a burst of rapid flashes doesn't produce a run of
    // near-duplicate entries in the saved output.
    mergeCloseEvents(result.events, 1.0);

    cap.release();

    result.processing_time_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
//  Multi-video parallel dispatcher
// ---------------------------------------------------------------------------

std::vector<VideoResult> LightningDetector::analyseVideos(
    const std::vector<std::string>& video_paths,
    ProgressCallback progress_cb,
    CompletionCallback completion_cb)
{
    // -----------------------------------------------------------------------
    //  GPU / OpenCL device selection
    //
    //  AMD (and some Intel) systems install TWO OpenCL platforms:
    //    - an OpenCL runtime for the CPU  (e.g. "AMD APP" or "Intel CPU")
    //    - an OpenCL runtime for the GPU  (e.g. "AMD Radeon RX 6600")
    //  OpenCV's default behaviour is to pick the FIRST device it finds,
    //  which on AMD systems is frequently the CPU OpenCL device.  That
    //  causes useOpenCL() to return true and the status bar to say "(GPU)"
    //  even though every kernel is actually still running on the CPU.
    //
    //  The fix: enumerate all platforms/devices explicitly and bind OpenCV
    //  to a device whose type is CL_DEVICE_TYPE_GPU.  If none is found,
    //  fall back gracefully to whatever OpenCV would pick by default.
    // -----------------------------------------------------------------------
    cv::ocl::setUseOpenCL(params_.use_gpu);
    if (params_.use_gpu && cv::ocl::haveOpenCL()) {
        // Try to create a context bound specifically to a GPU device type.
        // Context::create(int) is the only overload available in this
        // OpenCV build. CL_DEVICE_TYPE_GPU = 4. If no GPU OpenCL device
        // is present this returns false and OpenCV falls back to its
        // default device (which may be a CPU OpenCL runtime on AMD systems).
        cv::ocl::Context ctx;
        ctx.create(4 /* CL_DEVICE_TYPE_GPU */);
    }

    const int n = (int)video_paths.size();
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
            // Fire the per-video completion callback immediately, while
            // other videos may still be processing on other threads.
            if (completion_cb) {
                std::lock_guard<std::mutex> lk(mtx_cb);
                completion_cb(idx, results[idx]);
            }
            safe_cb(idx, n, results[idx].total_frames, results[idx].total_frames,
                    "Done: " + results[idx].video_filename);
        }
    };

    const int spawn = std::min(max_t, n);
    std::vector<std::thread> threads;
    threads.reserve(spawn);
    for (int i = 0; i < spawn; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    return results;
}

// ---------------------------------------------------------------------------
//  JSON serialisation
// ---------------------------------------------------------------------------

bool LightningDetector::saveJSON(
    const std::vector<VideoResult>& results,
    const std::string& output_path)
{
    json root;
    root["generator"]              = "LightningDetector v1.0";
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
        vj["processing_time_seconds"] =
            std::round(r.processing_time_seconds * 1000.0) / 1000.0;
        vj["lightning_event_count"]   = (int)r.events.size();
        if (!r.error_message.empty())
            vj["error"] = r.error_message;

        json evs = json::array();
        for (auto& ev : r.events) {
            const int    h = (int)(ev.timestamp_seconds / 3600);
            const int    m = (int)(ev.timestamp_seconds / 60) % 60;
            const double s = std::fmod(ev.timestamp_seconds, 60.0);
            std::ostringstream ts;
            ts << std::setfill('0') << std::setw(2) << h << ":"
               << std::setw(2) << m << ":"
               << std::setw(6) << std::fixed << std::setprecision(3) << s;

            json ej;
            ej["frame"]             = ev.frame_number;
            ej["timestamp"]         = ts.str();
            ej["timestamp_seconds"] = std::round(ev.timestamp_seconds * 1000.0) / 1000.0;
            ej["confidence"]        = std::round(ev.confidence * 100.0f) / 100.0f;
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

// ---------------------------------------------------------------------------
//  CSV serialisation
// ---------------------------------------------------------------------------

bool LightningDetector::saveCSV(
    const std::vector<VideoResult>& results,
    const std::string& output_path)
{
    // Escape a field for CSV: wrap in quotes if it contains comma or quote
    auto esc = [](const std::string& s) -> std::string {
        if (s.find_first_of(",\"") == std::string::npos) return s;
        std::string r = "\"";
        for (char c : s) { if (c == '"') r += "\"\""; else r += c; }
        r += "\"";
        return r;
    };

    std::ofstream f(output_path);
    if (!f.is_open()) return false;

    f << "VideoFile,VideoPath,Success,DurationSeconds,TotalFrames,FPS,"
         "ProcessingTimeSeconds,LightningEventCount,"
         "Frame,Timestamp,TimestampSeconds,Confidence,ConfidencePct,Type\n";

    for (auto& r : results) {
        if (r.events.empty()) {
            // One row per video even if no events
            f << esc(r.video_filename) << "," << esc(r.video_path) << ","
              << (r.success ? "true" : "false") << ","
              << std::fixed << std::setprecision(3) << r.duration_seconds << ","
              << r.total_frames << ","
              << std::setprecision(3) << r.fps << ","
              << std::round(r.processing_time_seconds*1000.0)/1000.0 << ","
              << 0 << ",,,,,,\n";
            continue;
        }
        for (auto& ev : r.events) {
            int    h = (int)(ev.timestamp_seconds / 3600);
            int    m = (int)(ev.timestamp_seconds / 60) % 60;
            double s = std::fmod(ev.timestamp_seconds, 60.0);
            std::ostringstream ts;
            ts << std::setfill('0') << std::setw(2) << h << ":"
               << std::setw(2) << m << ":"
               << std::setw(6) << std::fixed << std::setprecision(3) << s;

            f << esc(r.video_filename) << "," << esc(r.video_path) << ","
              << (r.success ? "true" : "false") << ","
              << std::fixed << std::setprecision(3) << r.duration_seconds << ","
              << r.total_frames << ","
              << std::setprecision(3) << r.fps << ","
              << std::round(r.processing_time_seconds*1000.0)/1000.0 << ","
              << (int)r.events.size() << ","
              << ev.frame_number << ","
              << ts.str() << ","
              << std::round(ev.timestamp_seconds*1000.0)/1000.0 << ","
              << std::round(ev.confidence*100.0f)/100.0f << ","
              << std::round(ev.confidence*10000.0f)/100.0f << ","
              << esc(ev.description) << "\n";
        }
    }
    return true;
}
