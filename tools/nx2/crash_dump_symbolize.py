"""Turns lichii.exe offsets (from crash_dump_threads.py) into function names and source lines with
Windows' dbghelp, for builds where llvm-symbolizer cannot read lichii.pdb.

    python tools/nx2/crash_dump_symbolize.py build/bin/lichii.exe 3795510 25e1665

lichii.pdb must sit next to the exe. Optimised builds fold identical functions, so a name can be
another function with the same code: check the neighbours before trusting a single frame.
"""
import ctypes, sys
from ctypes import wintypes as w
dbg=ctypes.WinDLL("dbghelp.dll")
class SYMBOL_INFO(ctypes.Structure):
    _fields_=[("SizeOfStruct",w.ULONG),("TypeIndex",w.ULONG),("Reserved",ctypes.c_uint64*2),("Index",w.ULONG),("Size",w.ULONG),
              ("ModBase",ctypes.c_uint64),("Flags",w.ULONG),("Value",ctypes.c_uint64),("Address",ctypes.c_uint64),("Register",w.ULONG),
              ("Scope",w.ULONG),("Tag",w.ULONG),("NameLen",w.ULONG),("MaxNameLen",w.ULONG),("Name",ctypes.c_char*1024)]
class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_=[("SizeOfStruct",w.DWORD),("Key",ctypes.c_void_p),("LineNumber",w.DWORD),("FileName",ctypes.c_char_p),("Address",ctypes.c_uint64)]
dbg.SymInitialize.argtypes=[w.HANDLE,ctypes.c_char_p,w.BOOL]
dbg.SymLoadModuleEx.argtypes=[w.HANDLE,w.HANDLE,ctypes.c_char_p,ctypes.c_char_p,ctypes.c_uint64,w.DWORD,ctypes.c_void_p,w.DWORD]
dbg.SymLoadModuleEx.restype=ctypes.c_uint64
dbg.SymFromAddr.argtypes=[w.HANDLE,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64),ctypes.POINTER(SYMBOL_INFO)]
dbg.SymGetLineFromAddr64.argtypes=[w.HANDLE,ctypes.c_uint64,ctypes.POINTER(w.DWORD),ctypes.POINTER(IMAGEHLP_LINE64)]
h=ctypes.windll.kernel32.GetCurrentProcess()
exe=sys.argv[1].encode(); base=0x140000000
print("init",dbg.SymInitialize(h,None,False))
dbg.SymSetOptions(0x2|0x4|0x10000)  # UNDNAME | DEFERRED_LOADS... 
b=dbg.SymLoadModuleEx(h,None,exe,None,base,0x10000000,None,0)
print("loaded base",hex(b), "err", ctypes.GetLastError())
for off in sys.argv[2:]:
    a=base+int(off,16)
    sym=SYMBOL_INFO(); sym.SizeOfStruct=ctypes.sizeof(SYMBOL_INFO)-1024; sym.MaxNameLen=1023
    d=ctypes.c_uint64(0)
    ok=dbg.SymFromAddr(h,a,ctypes.byref(d),ctypes.byref(sym))
    line=IMAGEHLP_LINE64(); line.SizeOfStruct=ctypes.sizeof(IMAGEHLP_LINE64); disp=w.DWORD(0)
    ok2=dbg.SymGetLineFromAddr64(h,a,ctypes.byref(disp),ctypes.byref(line))
    print(off, sym.Name.decode(errors='replace')[:200] if ok else "??", ("%s:%d"%(line.FileName.decode(errors='replace'),line.LineNumber)) if ok2 else "")
