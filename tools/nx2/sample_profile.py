"""Sampling profiler for a running lichii.exe: which functions the busiest threads spend time in.

    python tools/nx2/sample_profile.py --delay 120 --duration 30 --out profile_report.txt

Waits --delay seconds (to get into the scene to measure), picks the busiest threads, then suspends
each briefly many times a second and records where it was. Needs lichii.pdb next to lichii.exe
(build/bin) for function names. The code addresses of the emulated game (the JIT) have no names;
they show as "jit/unknown" and mean the time is in the emulated CPU.

A thread blocked in a wait shows the wait function (ntdll, KernelBase), so each sample also records
the nearest lichii return address on its stack: that is the function that called the wait. Names
are from the PDB; identical functions are merged by the linker, so a name can stand for a sibling
with the same code.

Windows only. Slows the game down a little while sampling.
"""
import argparse
import ctypes
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from ctypes import wintypes as w

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp.dll")
psapi = ctypes.WinDLL("psapi", use_last_error=True)

THREAD_SUSPEND_RESUME = 0x2
THREAD_GET_CONTEXT = 0x8
THREAD_QUERY_INFORMATION = 0x40
PROCESS_QUERY_INFORMATION = 0x400
PROCESS_VM_READ = 0x10
CONTEXT_CONTROL = 0x100001


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG),
                ("dwFlags", w.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_uint64), ("High", ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64), ("P3Home", ctypes.c_uint64),
                ("P4Home", ctypes.c_uint64), ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", w.DWORD), ("MxCsr", w.DWORD),
                ("SegCs", w.WORD), ("SegDs", w.WORD), ("SegEs", w.WORD), ("SegFs", w.WORD),
                ("SegGs", w.WORD), ("SegSs", w.WORD), ("EFlags", w.DWORD),
                ("Dr0", ctypes.c_uint64), ("Dr1", ctypes.c_uint64), ("Dr2", ctypes.c_uint64),
                ("Dr3", ctypes.c_uint64), ("Dr6", ctypes.c_uint64), ("Dr7", ctypes.c_uint64),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64),
                ("Rbx", ctypes.c_uint64), ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64),
                ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64), ("R8", ctypes.c_uint64),
                ("R9", ctypes.c_uint64), ("R10", ctypes.c_uint64), ("R11", ctypes.c_uint64),
                ("R12", ctypes.c_uint64), ("R13", ctypes.c_uint64), ("R14", ctypes.c_uint64),
                ("R15", ctypes.c_uint64), ("Rip", ctypes.c_uint64),
                ("Rest", ctypes.c_ubyte * 1024)]  # floating point and vector state, unused


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", w.ULONG), ("Size", w.ULONG), ("ModBase", ctypes.c_uint64),
                ("Flags", w.ULONG), ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64),
                ("Register", w.ULONG), ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG),
                ("MaxNameLen", w.ULONG), ("Name", ctypes.c_char * 512)]


k32.OpenThread.restype = w.HANDLE
k32.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.SuspendThread.argtypes = [w.HANDLE]
k32.ResumeThread.argtypes = [w.HANDLE]
k32.GetThreadContext.argtypes = [w.HANDLE, ctypes.POINTER(CONTEXT)]
k32.ReadProcessMemory.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.GetThreadTimes.argtypes = [w.HANDLE, ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME),
                               ctypes.POINTER(w.FILETIME), ctypes.POINTER(w.FILETIME)]
k32.CreateToolhelp32Snapshot.restype = w.HANDLE
k32.GetCurrentProcess.restype = w.HANDLE
k32.CloseHandle.argtypes = [w.HANDLE]
k32.GetThreadDescription.argtypes = [w.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_wchar_p, w.DWORD]
psapi.GetModuleInformation.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, w.DWORD]
dbg.SymInitialize.argtypes = [w.HANDLE, ctypes.c_char_p, w.BOOL]
dbg.SymSetOptions.argtypes = [w.DWORD]
dbg.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                            ctypes.POINTER(SYMBOL_INFO)]
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymLoadModuleEx.argtypes = [w.HANDLE, w.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                ctypes.c_uint64, w.DWORD, ctypes.c_void_p, w.DWORD]


def find_pid(name):
    out = subprocess.check_output(["tasklist", "/fi", f"imagename eq {name}", "/fo", "csv", "/nh"],
                                  text=True)
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    ids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            ids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return ids


def thread_cpu_seconds(handle):
    c, e, kt, ut = (w.FILETIME() for _ in range(4))
    if not k32.GetThreadTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt),
                              ctypes.byref(ut)):
        return 0.0
    return (((kt.dwHighDateTime << 32) | kt.dwLowDateTime) +
            ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)) / 1e7


def thread_name(handle):
    try:
        ptr = ctypes.c_wchar_p()
        if k32.GetThreadDescription(handle, ctypes.byref(ptr)) == 0 and ptr.value:
            name = ptr.value
            k32.LocalFree(ptr)
            return name
    except OSError:
        pass
    return ""


def modules(hproc):
    arr = (ctypes.c_void_p * 1024)()
    needed = w.DWORD()
    psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(needed), 3)
    result = []
    for i in range(min(needed.value // ctypes.sizeof(ctypes.c_void_p), 1024)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleFileNameExW(hproc, ctypes.c_void_p(arr[i]), name, 260)
        info = (ctypes.c_uint64 * 3)()  # lpBaseOfDll, SizeOfImage (u32 + pad), EntryPoint
        psapi.GetModuleInformation(hproc, ctypes.c_void_p(arr[i]), ctypes.byref(info),
                                   ctypes.sizeof(info))
        result.append((arr[i], info[1] & 0xFFFFFFFF, name.value))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", default="lichii.exe")
    ap.add_argument("--pid", type=int, help="sample this process id instead of looking one up")
    ap.add_argument("--delay", type=float, default=0.0, help="seconds to wait before sampling")
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--threads", type=int, default=8, help="how many of the busiest threads")
    ap.add_argument("--interval-ms", type=float, default=5.0)
    ap.add_argument("--out", default="profile_report.txt")
    args = ap.parse_args()

    pid = args.pid
    if pid is None:
        print(f"waiting for {args.process}...")
    while pid is None:
        pid = find_pid(args.process)
        time.sleep(0.5)
    print(f"pid {pid}; sampling starts in {args.delay:.0f} s")
    time.sleep(args.delay)

    hproc = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not hproc:
        sys.exit("cannot open the process (run from the same user, not elevated differently)")
    mods = modules(hproc)
    main_mod = next((m for m in mods if m[2].lower().endswith(args.process.lower())), mods[0])
    args.process = main_mod[2].split("\\")[-1]
    base, size, path = main_mod

    dbg.SymSetOptions(0x2 | 0x4 | 0x10000)
    sym_proc = k32.GetCurrentProcess()
    dbg.SymInitialize(sym_proc, None, False)
    dbg.SymLoadModuleEx(sym_proc, None, path.encode(), None, base, size, None, 0)

    def module_of(addr):
        for b, s, p in mods:
            if b <= addr < b + s:
                return p.split("\\")[-1], addr - b
        return None, 0

    sym_cache = {}

    def symbol(addr):
        if addr in sym_cache:
            return sym_cache[addr]
        mod, off = module_of(addr)
        if mod is None:
            result = "jit/unknown"
        elif mod.lower() != args.process.lower():
            result = mod
        else:
            sym = SYMBOL_INFO()
            sym.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 512
            sym.MaxNameLen = 511
            disp = ctypes.c_uint64()
            if dbg.SymFromAddr(sym_proc, addr, ctypes.byref(disp), ctypes.byref(sym)):
                result = sym.Name.decode(errors="replace")[:140]
            else:
                result = f"{mod}+{off:#x}"
        sym_cache[addr] = result
        return result

    def is_call_return(addr):
        """True when the bytes before addr end a call instruction (so addr is a return address)."""
        buf = ctypes.create_string_buffer(9)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr - 9), buf, 9, ctypes.byref(got)):
            return False
        b = buf.raw
        if b[8 - 5] == 0xE8:             # call rel32
            return True
        for length in range(2, 8):       # FF /2 forms: call r/m64, with or without a REX prefix
            opcode = 9 - length
            if b[opcode] != 0xFF:
                continue
            modrm = b[opcode + 1]
            if (modrm >> 3) & 7 != 2:
                continue
            mod, rm = modrm >> 6, modrm & 7
            size = {3: 2, 0: 6 if rm == 5 else (3 if rm == 4 else 2),
                    1: 4 if rm == 4 else 3, 2: 7 if rm == 4 else 6}[mod]
            if mod == 3:
                size = 2
            if size == length:
                return True
        return False

    # Pick the busiest threads over a short window.
    handles = {}
    for tid in thread_ids(pid):
        h = k32.OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           False, tid)
        if h:
            handles[tid] = h
    before = {t: thread_cpu_seconds(h) for t, h in handles.items()}
    time.sleep(2.0)
    busy = sorted(((thread_cpu_seconds(h) - before[t]) / 2.0, t) for t, h in handles.items())
    chosen = [t for load, t in reversed(busy[-args.threads:]) if load > 0.02]
    loads = {t: load for load, t in busy}
    print("busiest threads (CPU fraction of one core):",
          ", ".join(f"{t}:{loads[t]:.2f}" for t in chosen))

    samples = defaultdict(Counter)   # thread -> function -> count
    waits = defaultdict(Counter)     # thread -> wait-site function -> count
    totals = Counter()
    # GetThreadContext needs a 16-byte aligned CONTEXT; ctypes structures are not guaranteed that.
    raw_ctx = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT) + 16)
    aligned = (ctypes.addressof(raw_ctx) + 15) & ~15
    ctx = CONTEXT.from_address(aligned)
    ctx.ContextFlags = CONTEXT_CONTROL
    end = time.time() + args.duration
    interval = args.interval_ms / 1000.0
    while time.time() < end:
        for tid in chosen:
            h = handles[tid]
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ok = k32.GetThreadContext(h, ctypes.byref(ctx))
            rip, rsp = ctx.Rip, ctx.Rsp
            stack = b""
            if ok:
                buf = ctypes.create_string_buffer(512)
                got = ctypes.c_size_t()
                if k32.ReadProcessMemory(hproc, ctypes.c_void_p(rsp), buf, 512, ctypes.byref(got)):
                    stack = buf.raw[:got.value]
            k32.ResumeThread(h)
            if not ok:
                continue
            totals[tid] += 1
            mod, _ = module_of(rip)
            if mod is not None and mod.lower() == args.process.lower():
                samples[tid][symbol(rip)] += 1
            else:
                # Outside lichii (a wait, the driver, the JIT): attribute to the caller in lichii.
                caller = None
                for i in range(0, len(stack) - 7, 8):
                    value = struct.unpack_from("<Q", stack, i)[0]
                    m2, _ = module_of(value)
                    if m2 is not None and m2.lower() == args.process.lower() and is_call_return(value):
                        caller = value
                        break
                where = symbol(rip)
                if caller is not None:
                    samples[tid][f"[{where}] <- {symbol(caller)}"] += 1
                else:
                    samples[tid][f"[{where}]"] += 1
        time.sleep(interval)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"{args.process} pid {pid}, {args.duration:.0f} s, {len(chosen)} busiest threads\n\n")
        for tid in chosen:
            n = totals[tid]
            name = thread_name(handles[tid])
            f.write(f"=== thread {tid} {name!r}: {loads[tid] * 100:.0f}% of a core, {n} samples\n")
            for fn, c in samples[tid].most_common(18):
                f.write(f"  {100.0 * c / n:5.1f}%  {fn}\n")
            f.write("\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
