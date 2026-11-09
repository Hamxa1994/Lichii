# Reading a host crash

A log that just stops means the process died. The minidump is in
`%LOCALAPPDATA%\CrashDumps\lichii.exe.<pid>.dmp` and the Windows Application log has the faulting
module (`Get-WinEvent -FilterHashtable @{LogName='Application';ProviderName='Application Error'}`).

1. **Memory exhaustion or a fault?** Run `commit_monitor.ps1` before the game. A "low virtual
   memory" event (System log, id 2004) at the moment the log ends, or commit reaching its limit,
   is exhaustion. A fault in a driver DLL (`amdvlk64.dll`, `nvoglv64.dll`) is a different problem.
2. **Which thread?** `python tools/nx2/crash_dump_threads.py <dump>` prints the crashing thread and
   the lichii and driver frames on its stack. `Vulkan::Scheduler::WorkerThread` there means the
   driver crashed while replaying recorded GPU commands, so look for anything the recording thread
   changes while the worker may still be executing earlier commands (descriptor writes, buffers
   or images destroyed early).
3. **Names.** `python tools/nx2/crash_dump_symbolize.py build/bin/lichii.exe <offsets...>` uses
   dbghelp and the PDB next to the exe.

Install `minidump` with `pip install minidump`; the scripts import it from the user site.
