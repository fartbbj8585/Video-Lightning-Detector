# Lightning Detector (C++ / Real-Time Detection System)

Lightning Detector is a C++ application designed to analyse visual input (such as video feeds or frame sequences) and detect lightning events in real time. It is built for high-performance execution and uses optimised image-processing techniques to identify rapid brightness changes consistent with lightning strikes.

---

## Features

- Real-time lightning detection from video or frame input
- Support for multiple video sources (files or live feeds depending on build configuration)
- Adjustable detection sensitivity thresholds
- Optimised C++ performance for low-latency processing
- Frame comparison-based detection algorithm (brightness spike analysis)
- Simple executable GUI / console interface depending on build
- Packaged installer available for easy setup (Windows)

---

## How It Works

The detection system operates by analyzing consecutive frames and identifying sudden changes in brightness that exceed a defined threshold.

### Core Detection Logic

1. The program reads frames from a video source
2. Each frame is read of its brightness data and can be changed how sensetive you want it to be.
3. The current frame is compared with the previous frame for more accurate results
4. Each detection will be given a confidence score, which will tell you how confident it was that there was lightning
5. If the difference exceeds a configured threshold, the lightning will be silently detected and output into a .json file, which holds all data, including the frame number, the confidence score, and the timestamp of the video.

### Key Parameters

- **Brightness Threshold**  
  Defines how large a change must be to count as lightning.

- **Minimum Flash Pixel Fraction**  
  Defines what percentage of the frame must be lit up at once for it to count as lightning

- **Event Cooldown Time**  
  Prevents multiple detections of the same lightning strike.
---

## File Format Support

-Supports MP4, AVI, MKV, MOV, WMV, M4V, MPG, TS, FLV, (WEBM Coming Soon)

## Optimisation Features

-Allows GPU acceleration
-Ability to analyse at any FPS (The less FPS analysed, the lower the number of frames analysed)
-Ability to choose how many CPU threads it uses
-Ability to switch between the application using your GPU and CPU
-Ability to change the dimensions of the videos being analysed

## Downloading

The downloader file in releases, when downloaded, will automatically install all files and dependencies needed to run this application on your pc.

