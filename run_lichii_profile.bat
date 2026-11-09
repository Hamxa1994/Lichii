@echo off
rem Starts Lichii with the GPU and frame-time profilers on and the machine monitor running, so a
rem slow stretch can be read afterwards:
rem   - eden_log.txt: "GPU thread time per second by zone" and the per-present timings;
rem   - monitor_last.csv (next to this file): memory, CPU and GPU use once a second for 9 minutes.
rem Nothing else changes. Close Lichii normally (not by killing it) so the log is complete.
set EDEN_NX2_GPU_PROFILE=1
set EDEN_DKB_FRAME_TRACE=1
start "" /min pwsh -NoProfile -File "%~dp0tools\nx2\commit_monitor.ps1" -out "%~dp0monitor_last.csv"
start "" "%~dp0build\bin\lichii.exe"
