# ! Lightning Detector

A fast, native Windows desktop application that analyses video files to detect lightning events across any environment -- day, night, indoor, outdoor, wide-angle, telephoto -- and saves timestamped results with confidence scores to a readable JSON file.

---

## Features

| Feature | Detail |
|---|---|
| **Drag & Drop** | Drop multiple video files straight onto the window |
| **Multi-video** | Process many videos at once, in parallel |
| **Fast** | Pure OpenCV pixel-level analysis -- no AI model overhead; 240 000-frame videos handled in minutes |
| **Environment-agnostic** | Works for storms, studio flash, night sky, daytime |
| **Typed events** | Classifies each flash as "Cloud-to-ground", "Intra-cloud", or "Sheet lightning" |
| **Confidence score** | 0-100% per event, based on flash intensity and spatial spread |
| **JSON output** | Human-readable, pretty-printed; auto-saved and re-savable via "Save JSON As..." |
| **Adjustable settings** | Threshold, sensitivity, gap between events, thread count |

---

## Prerequisites (Windows 10 / 11, x64)

You need:
- **Windows 10** (version 1809 or later) -- for winget
- **~8 GB free disk space** (for Visual Studio build tools + OpenCV)
- An internet connection for the first-time install

---

## Step 1 -- Install dependencies (one time only)

Open **PowerShell as Administrator** and run:

```powershell
# Navigate to the project folder first
cd C:\path\to\LightningDetector

# Run the dependency installer
.\INSTALL_DEPS.ps1
```

This installs:
- Git for Windows
- CMake
- Visual Studio 2022 Build Tools (C++ Desktop workload)
- vcpkg package manager
- OpenCV 4 (with FFmpeg video support)
- nlohmann/json

> [timer] First-time vcpkg install of OpenCV takes ~15-30 minutes (it compiles from source).

---

## Step 2 -- Build the application

Open a **new** PowerShell window (so PATH is refreshed), then:

```powershell
cd C:\path\to\LightningDetector
.\build.ps1
```

The executable will be at:
```
build\Release\LightningDetector.exe
```

All required DLLs (OpenCV, FFmpeg) are automatically copied next to it.

---

## Step 3 -- Run

Double-click `LightningDetector.exe` or run from PowerShell:

```powershell
.\build\Release\LightningDetector.exe
```

### Using the application

1. **Add videos** -- Drag & drop video files onto the window, or click **Add Videos...** to browse. Supports: `.mp4 .avi .mkv .mov .wmv .m4v .mpg .mpeg .ts .flv`
2. **Configure (optional)** -- Click **Settings...** to tune detection parameters
3. **Analyse** -- Click **> Analyse**; progress bar and log update in real time
4. **Results** -- Results appear in the log and are auto-saved to `lightning_results.json` next to the `.exe`; click **Save JSON As...** to save elsewhere

---

## JSON Output Format

```json
{
  "generator": "LightningDetector v1.0",
  "total_videos_processed": 2,
  "videos": [
    {
      "file": "storm_footage.mp4",
      "path": "C:\\Videos\\storm_footage.mp4",
      "success": true,
      "duration_seconds": 312.4,
      "total_frames": 9372,
      "fps": 29.97,
      "processing_time_seconds": 18.4,
      "lightning_event_count": 7,
      "lightning_events": [
        {
          "frame": 1042,
          "timestamp": "00:00:34.749",
          "timestamp_seconds": 34.749,
          "confidence": 0.8812,
          "confidence_pct": 88.12,
          "type": "Cloud-to-ground"
        }
      ]
    }
  ]
}
```

---

## Detection Settings

| Parameter | Default | Description |
|---|---|---|
| Brightness threshold | `30` | Per-pixel difference (0-255) vs background required to count as "lit" |
| Min pixel fraction | `0.05` | Minimum proportion of the frame that must be bright (5%) |
| Min gap between events | `15` | Frames of "quiet" needed between two separate events |
| Max threads | `0` (auto) | Number of videos processed simultaneously; 0 = CPU core count |

**Tuning tips:**
- **Too many false positives** (e.g. car headlights, camera cuts): raise the brightness threshold or pixel fraction
- **Missing faint lightning**: lower the brightness threshold to ~15-20
- **Processing very high-fps cameras (120/240fps)**: increase min gap to 30-60

---

## Project Structure

```
LightningDetector/
 include/
    LightningDetector.h       # Public API + data structures
 src/
    LightningDetector.cpp     # Detection algorithm
    main_gui.cpp              # Win32 GUI + drag-and-drop
 CMakeLists.txt                # CMake build definition
 build.ps1                     # One-shot build script
 INSTALL_DEPS.ps1              # One-shot dependency installer
 README.md                     # This file
```

---

## Algorithm

1. **Background modelling** -- An exponential moving average (EMA,  = 0.05) tracks the "normal" brightness of each pixel
2. **Frame differencing** -- Each frame is subtracted from the background; pixels above `brightness_threshold` are flagged
3. **Flash detection** -- If the fraction of flagged pixels exceeds `pixel_fraction_required`, the frame is a candidate flash
4. **Event merging** -- Consecutive flash frames are merged into one event; the peak-intensity frame determines the timestamp
5. **Classification** -- Flash intensity and spatial spread (std-dev of lit pixels) determine whether the event is labelled cloud-to-ground, intra-cloud, or sheet lightning
6. **Confidence** -- Blended score of flash intensity (70%) and spread (30%), capped to [0, 1]

The algorithm is single-pass per video, O(WxH) per frame, and runs entirely on CPU -- no GPU required.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| **`cmake` not found** | Restart PowerShell after install; or add CMake to PATH manually |
| **`cl.exe` not found** | Run `build.ps1` from the "Developer PowerShell for VS 2022" |
| **OpenCV DLL missing at runtime** | Re-run `build.ps1`; it copies DLLs automatically |
| **Video won't open** | Ensure FFmpeg DLLs are present next to the .exe; try re-encoding with HandBrake |
| **No events detected** | Lower brightness threshold in Settings (try 15-20) |
| **Too many false positives** | Raise pixel fraction to 0.10-0.15, or increase brightness threshold |

---

## License

MIT -- free to use, modify, and distribute.
