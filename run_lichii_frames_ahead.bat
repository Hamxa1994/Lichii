@echo off
rem Experiment: let the game prepare the next frame while the GPU renders this one
rem (EDEN_NX2_FRAMES_AHEAD=1). By default the game waits for the GPU to finish every frame before
rem it starts the next, so the CPU and the GPU take turns. On the 9 November 2026 profile of Mario
rem Tennis Fever (RX 580) the GPU was busy 11 ms of a 23.5 ms frame while the game waited 18 ms on
rem the GPU fence, so overlapping them could roughly halve the frame time.
rem
rem Risk: the emulator waits on purpose so the game does not overwrite data the GPU thread is still
rem reading, so a wrong or missing texture, or a flicker, is possible. If it shows up, close and
rem use the normal launcher. A built-in safeguard waits for the current frame again whenever the
rem GPU is behind.
rem
rem Same profilers and monitor as run_lichii_profile.bat; the monitor writes monitor_frames_ahead.csv.
set EDEN_NX2_FRAMES_AHEAD=1
set EDEN_NX2_GPU_PROFILE=1
set EDEN_DKB_FRAME_TRACE=1
start "" /min pwsh -NoProfile -File "%~dp0tools\nx2\commit_monitor.ps1" -out "%~dp0monitor_frames_ahead.csv"
start "" "%~dp0build\bin\lichii.exe"
