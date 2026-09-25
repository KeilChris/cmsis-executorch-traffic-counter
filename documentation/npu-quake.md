# Quake on the NPU render pipeline

id Software's Quake ([GPL source release](https://github.com/id-Software/Quake),
the software renderer in `WinQuake/`) on the pipeline of
[NPU render](npu-render.md): the Cortex-M55 runs Quake's front end (game
logic, BSP and edge list, span rasterizer, texture mapping), the Ethos-U85 is
the per-pixel back end (lighting in RGB, bloom, Quake's screen blend, 2x
bilinear upscale to the 480x800 panel) through the same `create_ai_layer.py`
and ExecuTorch runtime, with graphs made of the operators the NPU render
`shade` graph already uses.

**Status: work in progress.** What runs today, and how it was checked:

| Step | State | Checked |
|------|-------|---------|
| ExecuTorch 1.5.0 (pack + Python, together) | done | NPU render demo: 21.2 fps, max error 2.6/255 on the DevKit-E8 and on the AppKit-E8 (22 fps with 1.4.1) |
| Quake graphs through the unchanged export flow | done | On the AppKit-E8: `qpresent` 5.2 ms (NPU 2.5 ms + 2.6 ms IO copies), `qshade` with bloom 11.8 ms (NPU 8.9 ms + 2.8 ms) |
| Quake engine, headless, on the FVP | done | `timedemo demo1`: 969 frames, clean exit |
| Quake engine, headless, on the AppKit-E8 | done | `timedemo demo1`: **969 frames, 13.9 s, 69.8 fps** at 400x240, stock C renderer, hunk in the PSRAM; frame checksums equal the FVP's (frame 100: `808d62ab`); stack peak 233 kB of 320 kB |
| External RAM | AppKit-E8: works. DevKit-E8: does not | AppKit PSRAM 64 MB: 0 errors, 147 MB/s write, 116 MB/s read, 236 ns random line read. The DevKit's HyperRAM hangs the bus on the first access |
| Quake on the LCD, CPU present path | done | palette, rotation and nearest 2x on the CPU: 5.3 ms per frame; `timedemo demo1` 49.9 fps (frame read back from the frame buffer and checked) |
| NPU present path (`qpresent`), pipelined | done | the Ethos-U85 rotates (transpose), scales 2x bilinear and interleaves; a present thread drives NPU and display while the Quake thread renders the next frame (RTX semaphores under the Ethos-U driver): **53.4 fps** (sequential 46.7); lighter scenes sit at the panel's 60 Hz |
| NPU lighting (`qshade` in the renderer) | not started | the graph is measured (11.8 ms), the renderer patches are not written |

At 14.3 ms of CPU per frame and 11.8 ms for `qshade`, a pipelined frame is
bounded by the CPU plus the hand-off pass: the panel's 60 Hz is in reach.

## Where the CPU time goes (profiled on the target)

`tools/pc_profile.py` samples the DWT program counter sample register through
the J-Link while the target runs (3700 samples/s, no firmware change, no
halt) and maps the samples to functions. Pipelined NPU present, before the
hand-off pass was optimized:

| Share | Function | What |
|-------|----------|------|
| 23.5 % | `D_DrawSpans8` | Quake's textured span drawer |
| 11.5 % | hand-off pass | palette + rotation on the CPU, column-wise reads |
| 10.2 % | `arm_ethos_io_memcpy` | the 1.15 MB frame out of the NPU scratch (+ 288 kB in) |
| 8.0 % | `R_DrawSurfaceBlock8_mip0/1` | lighting the surface cache through the colormap |
| 4.0 % | `D_DrawZSpans` | z-buffer fill |
| 3.2 % | `video_end` | data-cache clean of the frame buffer |
| ... | | edge list, BSP walk, alias models: 2-3 % each |

The Ethos-U driver's whole-cache clean/invalidate per job does not show up.
What followed from it: the rotation moved into the graphs (a `permute`, fused
by Vela: `qpresent` stays at 2 NPU operators), so the hand-off pass reads rows
and resolves the palette with Helium gathers, 16 pixels at a time: 2276 us ->
690 us per frame. The output copy stays: the NPU delivers int8, the panel wants
uint8 (one bit flipped), and nothing linear in the display path can undo that.

## Optimization steps, each measured on the AppKit-E8

Every step is built, loaded and run through the CMSIS Developer Assistant
(`cmsis_action build` / `load_and_debug`, vector table checked with
`read_memory`, session ended so the target runs free, results over the
console). `timedemo demo1`, 969 frames, fps for CPU present / NPU sequential /
NPU pipelined:

| Step | CPU | NPU seq. | NPU pipelined | Note |
|------|-----|----------|---------------|------|
| 0. Baseline, Debug build (packs at -O1) | 49.9 | 46.6 | 53.4 | the PSRAM set-up is skipped on a warm start (a debugger load resets the core only) |
| 1. Release build-type (everything -O3) | 51.6 | 48.2 | 54.4 | the NPU execute time does not change (5.61 ms): the runtime was never the cost, the copies and the NPU are |
| 2. Renderer code in the ITCM | - | - | - | does not link with AC6 while C++ is in the image: see the comment in `board/DevKit-E8/linker_ac6_quake.sct.src` |
| 3. Helium span drawer (`quake/patches/0001`) | 52.5 | 49.4 | 55.3 | four texels per gather, same fixed-point math; small gain: the loop is bound by the texture fetches, not by instructions. Bit-exact: the frame checksums at frames 100, 300 and 400 (`808d62ab`, `e0788b7d`, `b407a061`) equal the FVP run of the stock C renderer (other frames differ between any two runs: particles and dynamic lights decay in real time) |

## Get the Quake source and the game data

Neither is in this repository.

```bash
python quake/fetch_quake.py      # clones id-Software/Quake at a pinned commit into quake/upstream/ (git-ignored), applies quake/patches/
```

Quake's source is GPL-2.0-or-later; the files of this port that derive from
it (`quake/platform/sys_alif.c`, `vid_alif.c`) carry that license, the rest of
the repository stays Apache-2.0. The game data is yours to supply: the
shareware `pak0.pak` (version 1.06, 18,689,235 bytes) goes to
`quake/data/id1/pak0.pak` (git-ignored). It may be redistributed only as the
unmodified shareware archive, so it is not fetched for you.

## How the work is split

```
Cortex-M55 HP, Quake thread                       Ethos-U85, present thread
 Host_Frame: server, QuakeC VM, client             qshade(albedo, light, blend_k, blend_c):
 R_RenderView: BSP/PVS, edges -> spans               color = albedo * light
 span drawers -> 400x240 planes in the DTCM          bloom at quarter resolution
 VID_Update = hand-off pass (Helium):                color = color * blend_k + blend_c   (v_blend)
   rotate to portrait, merge the HUD overlay,        2x bilinear, NCHW -> NHWC
   palette LUT index -> RGB                 ---->   -> 480x800 RGB888 -> frame buffer
```

Quake's lighting is one statement, baked into the surface cache:
`colormap[(light & 0xFF00) + pix]` (`R_DrawSurfaceBlock8_mip*` in `r_surf.c`,
`D_PolysetDrawSpans8` in `d_polyse.c`). The plan is to keep the texel and an
8-bit light value apart there, `(light << 8) | pix`, let the span drawers
write an index plane and a light plane, and do the multiply on the NPU in RGB:
smooth lighting instead of 64 colormap rows, at about the CPU cost of the
stock renderer.

The graphs are in [`model/quake.py`](../model/quake.py). `light` is a linear
multiplier, code / 128: code 128 is exactly 1.0 and marks unlit pixels (sky,
water, particles, fullbright texels, the HUD); codes above it are Quake's
overbright range, and a product above 1.0 saturates. Vela 5.1 for the
Ethos-U85-256, `Ethos_U85_SRAM_MRAM` / `Shared_Sram`:

| Method | NPU operators | Vela SRAM (scratch) | SRAM traffic per frame |
|--------|---------------|---------------------|------------------------|
| `qpresent(albedo)`: 2x bilinear, NHWC | 2 | 2625 KiB | 9.3 MB |
| `qshade`, no bloom | 4 | 2625 KiB | 10.5 MB |
| `qshade` with bloom | 411 | 2625 KiB | 40.5 MB |
| NPU render `shade`, for comparison | 426 | 4032 KiB | 61.9 MB (24.7 ms measured) |

At the 2.8 MB/ms the NPU render demo measured, that is about 3 ms, 4 ms and
15 ms of NPU time; the measurement on the board is open.

## Memory

| Memory | Holds |
|--------|-------|
| DTCM, 1 MB | the 400x240 frame (96 kB), the z-buffer (192 kB), the Quake thread's stack (320 kB: the render path keeps about 240 kB of edge, surface and span lists on it) |
| Bulk SRAM, 8 MB | Quake's globals (574 kB), the surface cache (694 kB), C library and RTX data; later the ExecuTorch pools and the frame buffers. The first 4 kB stay free: the boot configuration loads the Cortex-A32 stub there |
| HyperRAM, 64 MB, XIP at 0xA0000000 | Quake's hunk, 16 MB (`quake/platform/port_mem.c` brings it up) |
| OSPI NOR flash, XIP at 0xC0000000 | the pak image, served as `id1/pak0.pak` by `quake/platform/fs_pak_mem.c` through the C library's file interface |
| MRAM | code and constants: 383 kB of the 2 MB HP region |

The DevKit-E8 numbers are from the link map (CMSIS Developer Assistant,
`get_memory_usage`): `vid_buffer` 0x20000000, `zbuffer` 0x20017700,
`quake_stack` 0x20046500, `surfcache` 0x02012e60; RW_RAM 615,680 of 901,120 B,
RW_SRAM 1,214,008 of 8,384,512 B.

## Build and run

The Quake project is `quake/quake.cproject.yml`, in the same csolution, as
target-set `quake`:

```bash
cbuild cmsis-executorch.csolution.yml --active SSE-320-U85@quake --packs    # FVP, headless
cbuild cmsis-executorch.csolution.yml --active DevKit-E8@quake --packs      # board (AC6)
```

On the FVP the model preloads the pak image into DDR, where
`quake/platform/port_fvp.c` expects it:

```bash
.vscode/fvp.sh -f board/Corstone-320/fvp_config.txt --simlimit 600 \
    --data mps4_board.subsystem.cpu0=quake/data/id1/pak0.pak@0x90000000 \
    -a out/quake/SSE-320-U85/Debug/quake.hex
```

The run is a `timedemo demo1`; every 100th frame prints a checksum, the
result line ends the model.

## Notes from the bring-up

- The Quake group builds as `gnu90` with `-fsigned-char -fno-strict-aliasing
  -fwrapv -fcommon`. `-fcommon` is required, not optional: Quake's headers
  define globals without `extern` (`d_local.h`: `sadjust`, `bbextents`, ...).
  `console.c` wants `<unistd.h>` and `<fcntl.h>` for its debug log;
  `quake/platform/compat/` answers them. No change to the Quake source was
  needed to build it: `quake/patches/` is empty so far.
- Quake reads paks and demos with stdio `FILE*`, not only through its
  `Sys_File*` layer, so the pak is served below the C library
  (CMSIS-Compiler File Interface), not in `sys_alif.c`.
- The Corstone-320 target had a 4 kB main stack; `app_main` of the NPU render
  demo alone has a 5.3 kB frame, which ended in a HardFault at its entry
  (stack limit) before the first `printf`. The FVP target now has 512 kB.
- A `build` through the CMSIS Developer Assistant reported success for a
  context whose link had failed; check that the `.axf` is new.

## Running it on the AppKit-E8

The board needs no hands: programming, reset and the pak go through the
on-board J-Link, the console is UART4.

```bash
python quake/fetch_quake.py
cbuild cmsis-executorch.csolution.yml --active AppKit-E8@quake --packs
python tools/mram_load.py out/quake/AppKit-E8/Debug/quake.hex --no-run   # verified MRAM programming
# reset the SoC through the J-Link's reset pin: JLinkExe ... -autoconnect 0, commands r0, Sleep 300, r1
# console: "press a key to set the external RAM up" -> send a key
# console: "load id1/pak0.pak to 0xa1000000 now"     -> .venv/bin/python tools/load_pak.py, then a key
```

The pak stays in the PSRAM through resets of the SoC; the image finds it and
skips the RAM test and the load. Things that cost time during the bring-up:

- J-Link's `loadfile` (and the extension's load task) cannot be trusted with
  the MRAM: its loader fails when the core comes out of reset in a bad state
  and its read-back is cached. `tools/mram_load.py` copies with 64-bit stores
  on the core and verifies in a fresh session.
- The pack's OSPI0 defaults are the DevKit's HyperRAM values. The AppKit's
  APS512XXN PSRAM needs `RTE_OSPI0_WAIT_CYCLES 4` (else reads are one word
  late) and `RTE_OSPI0_SPI_FRAME_FORMAT 4`, dual octal (else addresses alias
  at 1 kB).
- A debugger reset restarts the core only; the PSRAM set-up then never
  finishes. Reset the SoC with the reset pin instead.
- J-Link takes 0xA0000000 for a flash bank: disable flash download before
  touching it. Its burst writes into the PSRAM drop a 32-byte block now and
  then; `tools/load_pak.py` verifies and repairs.

## The M55-HE with its Ethos-U55

Target-type `AppKit-E8-HE`: the same Quake project on the other Cortex-M55 of
the device, the high-efficiency core (160 MHz, 256 kB TCMs), with the
Ethos-U55 (128 MACs) that is local to it.

What is different from the HP / Ethos-U85 target:

- **The graphs.** The Ethos-U55 takes less. Through the same export flow
  (`ai_layer_u55/`, `MODEL_FLAVOR=quake-u55`, see the header of
  `ai_layer_u55/cmsis-executorch.cbuild-mlops.yml`):

  | On the Ethos-U55-128 | Result |
  |---|---|
  | transpose + nearest-neighbour 2x + interleave (`qpresent`) | on the NPU, 2 operators, 1406 KiB scratch |
  | bilinear 2x, any variant | stays on the CPU as a float operator: not used |
  | `albedo x light`, clamp, screen blend (`qshade`) | on the NPU, 11 operators |
  | bloom | compile error (depthwise convolution rejected): left out |

- **Memory.** The 256 kB DTCM cannot hold the frame, the z-buffer and the
  320 kB Quake stack: they live in the bulk SRAM
  (`board/AppKit-E8-HE/linker_ac6_quake_he.sct.src`); the NPU scratch pool is
  1.4 MB instead of 2.8 MB. Code: the HE's MRAM region at 0x80000000.
- **Boot.** The Secure Enclave has to start the HE core from its MRAM region
  and park the HP core. Once, with the board's switch on SEUART:

  ```bash
  cp .alif/M55_HE_mram_cfg.json  "$SETOOLS/build/config/"
  cp .alif/M55_HE_mram_stub.bin  "$SETOOLS/build/images/"
  cd "$SETOOLS" && ./tools-config -p 'E8 (AE822FA0E5597BS0) - 5.5 MRAM / 9.75 SRAM' \
    && ./app-gen-toc -f build/config/M55_HE_mram_cfg.json && ./app-write-mram -p
  ```

  (the VS Code task "Alif: Install M55_HE debug stubs (AppKit-E8-HE, ...)"
  does the same), then the switch back to UART4. After that the debugger loads
  the image like on the HP (`AppKit-E8-HE` in the CMSIS view). Going back to
  the HP target needs the same step with `.alif/M55_HP_mram_cfg.json`.

  The part number matters: the AppKit's device is `AE822FA0E5597BS0`, the
  DevKit's `...LS0`. A boot table built for the other one is skipped as a whole
  (`[SES] ATOC Part# mismatch ... ATOC SKIPPED` on the SEUART, 57600 baud);
  no core is started then and the J-Link reports `AP[n]: Could not read AHB
  ROM register` / `Could not find core in Coresight setup` for both cores.

- **Pak load through the debug session.** Halted at the "load id1/pak0.pak"
  prompt, in the debug console of the CMSIS debugger:

  ```
  > monitor flash download = 0
  > restore quake/data/id1/pak0.pak binary 0xa1000000
  > dump binary memory /tmp/pak_rb.bin 0xa1000000 0xa21d2cd3    (then cmp with the pak)
  ```

  Without the first line the J-Link takes the PSRAM window for a flash bank
  and runs its RAM loader at 0x20000000 (the HE's DTCM): the data arrives, but
  the core comes back with its PC in the loader (UsageFault in the heap) and
  the OSPI controller is left in a state in which the next PSRAM set-up hangs
  in `ospi_busy` until a reset through the pin (`> monitor reset 2`). With it
  the write takes about four minutes and the core state is untouched.

### Measured (AppKit-E8, M55-HE at 160 MHz + Ethos-U55-128, 2026-09-21)

| | M55-HE + Ethos-U55 | M55-HP + Ethos-U85 (for comparison) |
|---|---|---|
| `qpresent` execute | 23.6 ms (NPU busy 20.9 ms, IO copies 2.7 ms) | 5.6 ms |
| `qshade` execute | 39.6 ms (NPU busy 36.7 ms, IO copies 2.9 ms) | 11.9 ms (with bloom) |
| hand-off (palette LUT, rotate) | 1.8-2.1 ms | 0.69 ms |
| PSRAM, 64 MB test | 0 errors; write 90.8 MB/s, read 47.7 MB/s | same |
| attract mode (demo1, e1m3), NPU pipelined | 23-31 fps | 53-55 fps (`timedemo`) |

The fps is from the 100-frame report lines of the demo loop (vsync-paced),
not from `timedemo`. Open: after the debugger disconnects the HE core stays
halted (the HP keeps running), and the HE launch resets more than the core, so
the pak in the PSRAM does not survive a relaunch.

### CPU load as the acceptance criterion (Balletto-class budget: M55-HE + Ethos-U55)

The target class is a Cortex-M55 at 160 MHz with an Ethos-U55-128, as in the
Balletto B1: no GATHER, SELECT, comparisons, bilinear resize or depthwise
convolution on the NPU (the Ethos-U85 graphs `qfetch` and `qvert` of
`model/quake.py` do not apply; they are built into the U85 layer only and were
never run on hardware). On this class the CPU is the bottleneck and the NPU has
slack, so a change is kept only if it lowers the **CPU-busy time per frame** of
`timedemo demo1` (969 frames, the same in every run). RTX's idle thread is
replaced by one that counts its cycles (`port_alif.c`); everything else is CPU
work. `PORT_TIMEDEMO` in the board layer runs the benchmark, once per present path.

| Step (M55-HE + U55, `timedemo demo1`) | CPU busy / frame | CPU load | fps | Verdict |
|---|---|---|---|---|
| CPU present (no NPU) | 51.2 ms | 99 % | 19.7 | reference |
| NPU present, sequential | 37.5 ms | 64 % | 17.2 | |
| NPU present, pipelined (baseline) | 37.3 ms | 95 % | 25.8 | the NPU takes 13.9 ms of CPU work per frame |
| `qpresent_nhwc`: rotation on the CPU, graph = the resize alone | hand-off 1.9 -> 5.4 ms | | 19.8 -> 18.6 (demo loop) | **rejected**: NPU busy 20.7 -> 9.6 ms, but it loads the CPU (`PORT_PRESENT_NHWC` keeps it selectable) |
| `qshade_nhwc` | | | | not pursued: NPU busy 47.0 ms against 36.7 ms for `qshade` |
| Zero-copy scan-out (`PORT_ZERO_COPY`), pipelined | **33.5 ms** | 90 % | **27.2** | **accepted**: -3.9 ms CPU per frame |
| Zero-copy scan-out, sequential | 33.6 ms | 61 % | 18.5 | |
| 200 x 120 view, NPU scales 4x (`QuakePresentLowRes`), copying present | 23.6 ms | 72 % | 30.6 (capped: every second vblank) | **rejected by the user: the lower resolution is not acceptable.** The largest CPU saving measured (-13.7 ms); the 4x graph needs 2625 KiB of scratch (Vela keeps the 2x intermediate 16-channel padded), two such arenas do not fit |
| Hand-off pass writes into the NPU's input in the arena (no input copy) | 32.8 ms | 89 % | 27.4 | **reverted: distorted picture.** Vela places the output frame over the input planes (same address: their lifetimes do not overlap), and that output is on the panel while the next frame's planes are written |

Zero-copy scan-out: the present graph only moves bytes (transpose, nearest
resize), so the hand-off pass writes raw colour bytes (no int8 offset) and the
frame comes out of the NPU ready for the panel. The panel's two buffers are the
output tensors where Vela put them in the scratch of two present modules that
take turns (learned from the IO copy hook at start-up); the hook skips the
1.15 MB output copy, and no data-cache clean is needed for memory the CPU never
touches. The two 1.15 MB frame buffers are gone, a second pair of pools
(1.2 + 1.4 MB) came. IO copies per frame: 2.87 -> 0.54 ms.

Two things about the buffer hand-over, found with the panel in front of a person
and not in any log. The NPU module of a frame is the one whose scratch is the
display's *back* buffer, and it is chosen when the job starts, after
`video_begin()` has seen the last flip take effect: chosen by the renderer at
hand-off time, a frame queued behind a running job got the module that was
about to go on screen, and the NPU wrote into the visible frame (its input
planes first: artefacts on the left of the picture, and a frame rate above the
hand-over's limit as the tell). And "the last flip took effect" is read from
the CDC200 itself: its frame buffer address register is shadowed and reads back
the working value, so it shows the new buffer only after the reload at the
vertical blanking (`display_wait_shown()` in `board/DevKit-E8/board_display.c`;
measured: 27 % of the requests read back at once, the rest after the blanking).

### Where the M55-HE's CPU time goes, and the Helium steps that followed

`PORT_PROFILE` (with `PORT_TIMEDEMO`) adds a sampling profiler that needs no
probe: a high-priority RTX thread wakes every millisecond, reads the PC at which
the Quake thread was preempted from the exception frame on that thread's stack
and counts it in 64-byte buckets of the code; the buckets are printed after the
pipelined run and resolved against the linker map. 32 163 CPU samples:

| Share | Function | |
|---|---|---|
| 28.1 % | `D_DrawSpans8` | world texture mapping (4-lane Helium gather) |
| 11.4 % | `R_DrawSurfaceBlock8_mip0/1` | lighting the surface cache |
| 10.9 % | `R_RecursiveWorldNode`, `R_RenderFace`, `R_EmitEdge`, `R_ClipEdge` | visibility |
| 8.3 % | `D_PolysetDrawSpans8`, `D_PolysetScanLeftEdge`, `D_PolysetDrawFinalVerts` | alias models |
| 8.1 % | `R_GenerateSpans`, `R_LeadingEdge`, `R_StepActiveU` | edge list -> spans |
| 6.7 % | `D_DrawZSpans` | z-buffer fill |
| 5.6 % | hand-off pass | palette, feeding the NPU |
| 1.8 % / 1.1 % | `Turbulent8` / `uart_send_blocking` | water / the report lines |

None of the big ones fits an Ethos-U55 (gather, scatter); two were scalar C and
fit Helium. `timedemo demo1`, NPU pipelined, CPU busy per frame:

| Step | CPU busy / frame | fps | Verdict |
|---|---|---|---|
| Zero-copy scan-out (start of this table) | 33.5 ms | 27.2 | |
| No D-cache invalidate after the NPU jobs (`PORT_DCACHE_KEEP`) | 33.6 ms | 27.1 | no effect (the working set does not fit the cache anyway): off |
| 8-bit frame in the DTCM, z-buffer in the (otherwise empty) ITCM as data | 33.1 ms | 27.3 | kept; the CPU-only present path gains much more (52.6 -> 44.4 ms: column-wise reads) |
| Helium `D_DrawZSpans`: the 32-bit ramp four pixels at a time | 32.8 ms | 27.4 | kept, bit-exact |
| Helium `R_DrawSurfaceBlock8_mip0/1`: 8 x 16-bit light lanes, colormap gather | **30.4 ms** | **28.9** | kept, bit-exact (includes ~0.3 ms from a quieter benchmark console) |
| `D_DrawSpans8` perspective-correct every 16 pixels instead of 8 (`PORT_SPANSTEP: 16`; id's assembly used 16) | **28.7 ms** | **29.4** | kept: **not bit-exact by design**, judged acceptable on the panel in an A/B (`PORT_SPANSTEP_AB` alternates 8 / 16 every 4 s, labelled). The checksums of this configuration: `b3436e89`, `6b0c4d40`, `c8806486` |
| `D_DrawSpans8` with 8-lane gathers (16-bit texel offsets, `D_SPANS_8LANE`) | 29.2 ms | 29.1 | **rejected**: bit-exact but slower; the loop is bound by the gather's byte loads, the narrowing only adds work |
| Helium `D_PolysetDrawSpans8` (alias models): z test, skin fetch, colormap and both stores under one predicate, spans of 8 pixels and more | 28.4 ms | 29.2 | kept, bit-exact |
| Helium `D_DrawTurbulent8Span` (water: two sine-table gathers, texel gather) and a Helium copy for the 288 kB input planes | **28.2 ms** | **29.3** | kept, bit-exact |

Bit-exact means: the frame checksums of the CPU pass at frames 300, 400 and 900
(`e0788b7d`, `ae1453ef`, `d7e4615a`) are those of the stock C renderer. From
51.2 ms with the CPU doing everything to 28.2 ms: -45 % of CPU per frame. From the
16-pixel step on, "bit-exact" is against that configuration's checksums
(`b3436e89`, `6b0c4d40`, `c8806486`). With the console in other hands the benchmark's
results are read with the debugger: `port_results` in `port_alif.c`.

## The pak in the OSPI flash (AppKit-E8)

The PSRAM is volatile: after a power loss the 18 MB pak is gone, and with it the
game. `PORT_FLASH_PAK` (HE layer) keeps a copy in the board's OSPI NOR flash:

- **The part.** The AppKit-E8 carries a Macronix **MX66UW1G** (1 Gbit octal) on
  OSPI1, not the DevKit-E8's ISSI IS25WX256 (`BOARD_OSPI_FLASH_INSTANCE 2` in the
  pack's board file is the Macronix driver's CMSIS flash driver number). The
  pack's XIP set-up (`BSP:OSPI FLASH XIP:core`) only knows the ISSI part: its
  probe reads the ID 0xFF here and gives up. So the flash is not memory mapped;
  it is read and written through the pack's CMSIS flash driver
  (`BSP:External peripherals:OSPI Flash MX66UW1G`), `quake/platform/port_mem.c`.
- **Layout.** Sector 0: a header (`QPAK`, size, adler32). From 4 kB on: the pak.
- **Boot.** `pak_valid()` checks the whole PSRAM image against the shareware
  pak's adler32 (0x785c230a over 18 689 235 bytes), as the CPU reads it: four
  magic bytes survive a short power-off and a half-done load. Invalid and the
  flash has the image: copy it into the PSRAM, check again, go. Valid and the
  flash has none: store it, once (erase 4563 sectors: 110 s, program: 15.6 s,
  read back and compare, then the header). Neither: the firmware waits and
  looks at the PSRAM every two seconds; whoever loads the pak there (a
  debugger: `monitor flash download = 0`, `restore <pak> binary 0xa1000000`)
  needs no key press afterwards, and the next boot stores it in the flash.
- **For a debugger** (the console may be in other hands): `port_pak_status`
  {adler, checks, valid, waiting}, `port_flash_result` (10 image present, 11
  none, 1 restored, 3 stored, 2/4 failed), `port_flash_status` {stage, error,
  first_word, restore_ms, erase_ms, program_ms, done_bytes}, `port_flash_gate`
  (clear it at `main` to keep the firmware off the flash).
- **Open.** A debugger launch that resets the core while the previous run is
  busy on an OSPI controller can leave the PSRAM set-up hanging in the pack's
  `ospi_busy()` (the APS512XXN has no reset line, the controller is reset, the
  wait has no timeout); a reset through the pin (`monitor reset 2`) clears it.

## Boot screen

`quake/platform/port_splash.c`: the panel shows what the firmware is doing
before Quake can draw (a 5 x 7 font of its own, Quake's is in the pak that is
being fetched), in the game's orientation: title, one line per stage (NPU
check, external RAM, pak in RAM, pak in flash, copying / storing, starting
Quake) and a progress bar (10 / 20 / 30 % after the set-up stages, 30 -> 90 %
while the flash is read or written). Each redraw goes into the current back
buffer only: with zero-copy scan-out the two panel buffers are the NPU
modules' arenas, and the boot-time NPU measurement runs in those very pools,
so it must come before the display (it did not, at first: a scribbled boot
screen and a present pipeline that hung after two frames). That measurement
(about 3 s) is off by default now, `PORT_NPU_MEASURE` turns it on; the panel
comes up right after the Secure Enclave has released the core and the drivers
are up.

