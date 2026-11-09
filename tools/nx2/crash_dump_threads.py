r"""Lists every thread of a lichii minidump and, for the crashing thread, the return addresses on
its stack that fall inside lichii.exe or the Vulkan driver.

    python tools/nx2/crash_dump_threads.py %LOCALAPPDATA%\CrashDumps\lichii.exe.<pid>.dmp

Needs the `minidump` package (pip install minidump). Feed the lichii.exe+0x... offsets to
crash_dump_symbolize.py. The stack is a scan, so stale values show up; frames that name the
function being called by the driver (amdvlk64 / nvoglv64 / igvk64) are the ones to trust.
"""
import sys, struct
from minidump.minidumpfile import MinidumpFile
m=MinidumpFile.parse(sys.argv[1])
mods=sorted(m.modules.modules, key=lambda x:x.baseaddress)
def where(a):
    for mod in mods:
        if mod.baseaddress<=a<mod.baseaddress+mod.size:
            return "%s+0x%x" % (mod.name.split('\\')[-1], a-mod.baseaddress)
    return None
exc=m.exception.exception_records[0]
print("exception thread", exc.ThreadId, "addr", where(exc.ExceptionRecord.ExceptionAddress))
info=getattr(exc.ExceptionRecord,'ExceptionInformation',None)
print("info", info)
mr=m.get_reader()
names={}
try:
    for t in m.thread_names.names: names[t.ThreadId]=t.ThreadName
except Exception as e: pass
print("threads:", len(m.threads.threads))
for t in m.threads.threads:
    ctx=t.ContextObject
    rip=ctx.Rip; sp=ctx.Rsp
    frames=[]
    try:
        buf=mr.read(sp, 0x1800)
        for i in range(0,len(buf),8):
            v=struct.unpack_from("<Q",buf,i)[0]
            w=where(v)
            if w and ('lichii' in w or 'amdvlk' in w):
                frames.append(w)
    except Exception as e:
        frames.append("stack unreadable")
    mark="  <== CRASH" if t.ThreadId==exc.ThreadId else ""
    print("T%d %s rip=%s%s" % (t.ThreadId, names.get(t.ThreadId,''), where(rip), mark))
    if t.ThreadId==exc.ThreadId or any('amdvlk' in f for f in frames[:3]):
        for f in frames[:14]: print("     ", f)
