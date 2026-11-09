# Performance plan

What the logs of 9 November 2026 (Mario Tennis Fever, RX 580 2048SP, frame generation off) show,
what that means for "make the math faster", and the tasks that follow from it.

## What the logs show

| Observation | Evidence | Reading |
|---|---|---|
| Gameplay is GPU-bound | In the heavy stretches a present takes 38 to 51 ms and 24 to 31 ms of it is spent blocked on the GPU fence; GPU use is 98% | The cost is rendering, not the emulated CPU |
| Light stretches are fine | 17 ms per present (59 fps), fence wait 4 to 6 ms | The same scene without the heavy passes runs at full speed |
| The multi-second "frames" are loading | Present maxima of 8.7 s and 4.2 s with no frames in between; the log shows stage loading (`Commit`, lazy commit passing 3 GiB, sparse maps) | The game itself stops presenting while it loads; the time is guest CPU and file work |
| Short hitches (100 to 800 ms) at scene changes | Bursts of 60 to 490 pipeline builds in 1 to 3 s | First-time pipeline translation; total 650 ms over 12 minutes, so a hitch, not the average |
| Loaded textures are evicted | The texture cache's critical limit falls from 6343 to 5593 MiB as the allocator's unused block space grows from 35 to 772 MiB | Deliberate protection against running the 8 GB card out of memory; the cache then evicts and reloads |
| Draws missing | 59 geometry pipelines skipped (the GCN4 driver crashes building a geometry stage); 13 distinct draws reported skipped to 2048x2048 and 512x512 targets | Layered or multi-viewport passes, probably shadows |
| Videos decode on the CPU | `Could not find supported GPU pixel format, falling back to CPU decoder`, 1440p VP9 | Polaris has no VP9 hardware decoder |

## What "move the math to the GPU" can and cannot mean

- The game's own code (physics, game logic, decompression) is ARM64 and runs on the emulated CPU
  (the JIT in `src/dynarmic`). It cannot be run on a GPU: it is serial, branchy and works on guest
  memory the GPU does not see.
- Everything the GPU is good at already runs there: rendering, the shader recompiler's output,
  ASTC decoding (`accelerate_astc: Gpu`), and texture swizzling when `gpu_unswizzle_enabled` is on.
- Moving more of it to the GPU would make this particular problem worse: the GPU is already at
  98%, the CPU (a Ryzen 9 5900X) has room. For a GPU-bound game the useful direction is the
  opposite, taking work off the GPU.


## Measured on 9 November 2026 (profile launcher, Mario Tennis Fever, RX 580 2048SP)

| Measurement | Value |
|---|---|
| Frame time, steady heavy scene | 26 to 28 ms (35 to 38 fps) |
| GPU 3D engine use (lichii) | 87 to 88% for the whole session; 3 to 4% of the machine's CPU |
| lichii CPU | about 2.5 cores of 24 threads |
| GPU work actually submitted per frame | 11 ms (15.8 submissions) inside a 23.5 ms span: the GPU is idle about half the time |
| Guest frame | 31 ms, of which 19 ms in the present and 18.2 ms of that waiting on the GPU fence |
| Present path (acquire, record, present call) | about 2.4 ms in total |
| Draws | about 48,000 a second in the heavy scene; the four most expensive programs are 70% of the timed draw time (the worst one, `s1:38ca81ade22ca604 + s5:6bb51b7a7a6d0ffc`, 0.78 ms a draw, 2 a frame) |

Reading: the CPU and the GPU take turns. The game prepares a frame (about 12 ms), then waits for
the GPU to finish it before starting the next. Overlapping them (`EDEN_NX2_FRAMES_AHEAD=1`, see
`run_lichii_frames_ahead.bat`) is the largest available gain; the wait is deliberate (the game
must not overwrite data the GPU thread still reads), so it is an experiment first.


## Frames ahead (`EDEN_NX2_FRAMES_AHEAD=1`), same scene

| Measurement | Before | With frames ahead |
|---|---|---|
| Frame time, steady heavy scene | 26 to 28 ms (35 to 38 fps) | 23 to 25 ms (about 41 fps) |
| Game thread wait on the GPU fence | 16 to 18 ms | 0 |
| Present call | 19 ms | 1 ms |
| GPU work per frame / span | 11 ms / 23.5 ms | 12.8 ms / 26.2 ms (still about half idle) |
| Emulated GPU front thread busy | about 65% (idle 354 ms/s) | 96.5% (idle 36 ms/s), 600 ms/s of it waiting on the back thread |
| Emulated GPU back thread busy | not measured | 85% (about 61,000 draws/s, 14 us of CPU a draw) |

Reading: overlapping the CPU and the GPU gained about 15%, and moved the limit to the CPU side of
GPU emulation: the front and back threads that turn the game's commands into Vulkan calls. The
GPU still idles half the frame, waiting for them. Cost per draw in the back thread, per second:
method handling 190 to 200 ms (2M/s), "other" 160 to 170 ms (not attributed to any stage), update
buffers 130 to 140 ms, fence 100 to 165 ms, bind stage buffers 55 to 60 ms. `run_lichii_sample.bat`
samples those threads to say which functions those stages are.


Frames ahead is now the default for Mario Tennis Fever (`nx2_gsv.h`, per title like Ocarina of Time;
Donkey Kong Bananza still waits). The first present logs `nx2:vi2 frames ahead: on|off|auto`, and
`EDEN_NX2_FRAMES_AHEAD=0|1` still overrides it for any game. It is not the default for every game
because the code documents titles that corrupt memory when run ahead (Bananza, Splatoon Raiders,
Pokopia's glyph atlas).

## Sampling profile (frames ahead on, `run_lichii_sample.bat`)

| Thread | Share of a core | Where the samples are |
|---|---|---|
| GPU front thread | 91% | 56% in waits (spinning on the back thread), then `Maxwell3D::FrontWrite` 8%, macro run 8.5%, `DmaPusher` 7%, `BindRasterizer` 4% |
| GPU back thread | 74% | a flat profile: `GpuSplit::BackLoop`/`Execute` 13%, memcpy 4.6%, the profiler's own timer 14% (profiler runs only), no function over 7% |
| Vulkan worker thread | 20% | 83% waiting for work: it is starved, which is why the GPU is half idle |
| Emulated game cores (5 threads) | 16 to 25% each | 76 to 83% waiting, 12 to 20% in the JIT: the game's own code is not the limit |

Reading: the front and back GPU emulation threads (one each) are the bottleneck, and the back thread
has no single hot function to fix. What it can do is less work: 27% of the game's draws (about 380
of 1,400 a frame) end at a pipeline that was never built, after the full per-draw setup. That
setup is now skipped (`IsUnbuildable`). The profiler's own timer is 14% of the back thread, so the
profiled numbers are pessimistic; a run without it is faster.

## Tasks

Measure first, change second. Each item names what would justify it.

1. **Run one slow stretch with `run_lichii_profile.bat`.** Gives the GPU time per zone, the per
   present timings and `monitor_last.csv` (CPU, GPU, memory). Decides every item below.
2. **If the GPU is the limit** (monitor: `lichii_gpu3d_pct` near 100, CPU low): try, in this order,
   a lower resolution scale, `accelerate_astc` on the CPU (moves decode work off the GPU), and the
   window adapting filter off. These are settings, not code.
3. **If one lichii core is pegged and the GPU is idle**: profile the JIT and the kernel with the
   existing `EDEN_NX2_*` tools before changing anything.
4. **Texture eviction**: the margins are intentional; changing them risks out-of-memory crashes
   on an 8 GB card. A smaller texture footprint is the real fix (see 2: lower resolution scale
   shrinks render targets; BC1 instead of BC3 halves ASTC textures but needs a 20 GiB budget in
   this build, see the `UseBc3ForSwitch2OnSmallCards` warning).
5. **Missing layered passes**: `EDEN_NX2_GCN4_GEOMETRY=1` tests whether the installed driver
   survives building a geometry stage. If it does, the skip can be lifted for that driver version.
6. **Audio hang at the start**: no audio errors are logged. The start has a 490-pipeline burst and
   the first CPU-decoded 1440p video at the same time; the next profile run shows whether the CPU
   saturates then. Without that, there is nothing to fix.
7. **Frame generation cost**: GPU timestamps around the pyramid, flow, median and interpolation
   passes, once the measurement in 1 shows frame generation is worth tuning.
8. **Already done**: lazy commit of guest memory, the missing-pipeline draw drop (port of
   Eglantine #720), the scroll areas in Graphics, the flow median prefilter.
