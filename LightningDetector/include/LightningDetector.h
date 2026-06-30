#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>

struct LightningEvent {
    double timestamp_seconds;
    int    frame_number;
    float  confidence;        // 0.0 - 1.0
    std::string description;  // "Cloud to ground", "Intra-cloud", "flash", "screen tearing"
};

struct VideoResult {
    std::string video_path;
    std::string video_filename;
    double      duration_seconds;
    int         total_frames;
    double      fps;
    std::vector<LightningEvent> events;
    bool        success;
    std::string error_message;
    double      processing_time_seconds;
};

// Progress callback: (videoIndex, totalVideos, framesDone, totalFrames, statusMsg)
using ProgressCallback = std::function<void(int, int, int, int, const std::string&)>;

class LightningDetector {
public:
    LightningDetector();
    ~LightningDetector();

    // Analyse a list of video files. Calls progress_cb periodically.
    std::vector<VideoResult> analyseVideos(
        const std::vector<std::string>& video_paths,
        ProgressCallback progress_cb = nullptr);

    // Save results to a JSON file
    static bool saveJSON(const std::vector<VideoResult>& results,
                         const std::string& output_path);

    // Tuneable parameters
    struct Params {
        // Frame difference threshold (0-255 per channel) to flag a bright flash
        float  brightness_threshold    = 30.0f;
        // Fraction of pixels that must exceed threshold to count as lightning
        float  pixel_fraction_required = 0.05f;
        // Minimum frames between two independent lightning events
        int    min_event_gap_frames    = 15;
        // Max threads for parallel video processing
        int    max_threads             = 0;   // 0 = use hardware_concurrency

        //  Speed options 
        // If the source video's fps exceeds this, frames are skipped so
        // that analysis effectively runs at ~target_analysis_fps. Real
        // timestamps are still computed from the source fps, so reported
        // event times remain accurate to the original video length.
        // Set to 0 to disable (analyse every frame regardless of fps).
        double target_analysis_fps     = 60.0;

        // Downscale frames to at most this many pixels on the longest side
        // before analysis. Lightning flashes are large, global brightness
        // events, so downscaling has negligible effect on detection while
        // cutting per-frame processing cost roughly with the square of the
        // scale factor. Set to 0 to disable (analyse at full resolution).
        int    analysis_max_dimension  = 720;

        // Offload per-frame image math (color convert, diff, threshold) to
        // the GPU via OpenCV's Transparent API (OpenCL) when a compatible
        // GPU/driver is available. Automatically falls back to CPU with
        // identical results if no GPU/OpenCL is present. Cuts CPU usage
        // substantially on supported hardware.
        bool   use_gpu                 = true;
    };

    void setParams(const Params& p) { params_ = p; }
    Params getParams() const        { return params_; }

private:
    Params params_;

    VideoResult processSingleVideo(
        const std::string& path,
        int video_index, int total_videos,
        ProgressCallback progress_cb);
};
