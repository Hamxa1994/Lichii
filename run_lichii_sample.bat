@echo off
rem Starts Lichii (frames ahead, profilers on), the machine monitor, and a sampling profiler that
rem waits 150 seconds, then samples the busiest threads for 30 seconds and writes
rem profile_report.txt next to this file: which functions the emulated GPU threads, and the other
rem busy threads, spend their time in.
rem
rem Get into the heavy scene within 150 seconds and stay in it until about 190 seconds after
rem starting. Close Lichii normally afterwards. Needs Python (the same one used for the crash tools).
set EDEN_NX2_FRAMES_AHEAD=1
set EDEN_NX2_GPU_PROFILE=1
set EDEN_DKB_FRAME_TRACE=1
start "" /min pwsh -NoProfile -File "%~dp0tools\nx2\commit_monitor.ps1" -out "%~dp0monitor_sample.csv"
start "" /min python "%~dp0tools\nx2\sample_profile.py" --delay 150 --duration 30 --out "%~dp0profile_report.txt"
start "" "%~dp0build\bin\lichii.exe"
