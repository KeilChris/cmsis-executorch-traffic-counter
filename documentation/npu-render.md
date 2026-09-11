# NPU render: the Ethos-U85 as a tensor coprocessor for a 3D pipeline

A prototype of the idea that the Ethos-U85 is not a 3D accelerator but is
usable for the parts of a 3D pipeline that are plain tensor math. It runs on
the Alif Ensemble E8 DevKit (Cortex-M55 HP core at 400 MHz with Helium +
Ethos-U85 with 256 MACs) through the same csolution, `create_ai_layer.py` and
ExecuTorch runtime as the original TinyCNN example, and shows the result on
the DevKit's 480x800 MIPI DSI panel.

## What the NPU cannot do, and what it does here

The Ethos-U85 executes a Vela-compiled command stream over static shapes:
no data-dependent control flow, no scattered writes, int8/int16 only. That
rules out rasterization (edge walking, z-test, per-triangle branching). It
leaves the stages that are tensor math, and the prototype implements them as
two ExecuTorch methods in [`model/model.py`](../model/model.py):

| Method | Precision | Graph | Input | Output |
|--------|-----------|-------|-------|--------|
| `vertex` | int16 activations | `clip = bmm(pos, mvp)`, `nview = bmm(nrm, mv)`; the batch dimension is the object, each with its own matrices | 8 objects x 512 x 4 positions and normals, 8 x 4 x 4 MVP and model-view | clip-space positions, view-space normals |
| `shade` | int8 | three directional lights: 1x1 conv (n·L per light) → relu → 1x1 conv (light colours, bias = ambient); Blinn-Phong highlight: 1x1 conv (n·h) → relu → `pow(·, 16)` (a table lookup) → 1x1 conv (highlight colour); ambient occlusion: 7x7 depthwise box blur of the depth plane, `1 - 6·relu(depth - blur)`, clamped; `albedo · light · ao + highlight`; depth fog; bloom: `relu(colour - 0.55)` → 4x4 mean pool (stride-4 depthwise conv) → two 5x5 depthwise Gaussians → 4x bilinear up → `colour + 0.7 · glow`; clamp; 5x5 depthwise Gaussian → 2x bilinear upscale → NCHW-to-NHWC transpose | G-buffer 3 + 3 + 1 planes, 240 x 400 | 800 x 480 x 3 interleaved RGB, the display's RGB888 layout |

Vela compiles the shade graph to 425 NPU operators, 16.9 M MACs and 62 MB of
SRAM traffic per frame.

The Cortex-M55 does everything in between ([`src/app_main.cpp`](../src/app_main.cpp)),
written with Helium (MVE) intrinsics:

- perspective divide and viewport, one vertex per 4-lane vector;
- an edge-function rasterizer that walks the bounding box four pixels per
  iteration: barycentric weights as affine planes stepped per row, coverage,
  the 16-bit z-test (widening loads, narrowing stores), normal interpolation
  with a vector reciprocal square root (MVE has no vector sqrt or divide, so
  it is the integer seed plus one Newton step), albedo and fog interpolation,
  and int8 quantization with `vcvtn` and narrowing predicated stores into the
  planar G-buffer;
- vector fills for the G-buffer and z-buffer clears;
- the frame copy: the ExecuTorch Ethos-U backend copies the method output
  out of the NPU scratch through a weak `arm_ethos_io_memcpy` hook; the
  runner overrides it so the 1.15 MB frame goes straight into the back frame
  buffer with 16-byte vector loads and stores, XORing 0x80 on the way (int8
  `q + 128` is the uint8 the panel wants). The output tensor is never
  written.

The frame is double-buffered: the back buffer is presented with the CDC200's
vsync-synchronised frame buffer update, and the copy into the other buffer
waits for the controller's next start-of-frame event before overwriting it
([`board/DevKit-E8/board_display.c`](../board/DevKit-E8/board_display.c)).

The scene is eight objects, one per vertex batch: two interlocked checkered
tori (chain links, 512 vertices each) spinning as one, a striped sphere (480
vertices) weaving through them, four small moons and a thin ring on tilted
orbits around the whole, 2208 vertices and 4224 triangles, animated for as
long as the board runs. Without a display (the Corstone-320 FVP target) it
renders 8 frames and prints an ASCII preview.

## Measured on the DevKit-E8

Average over 120-frame windows, Cortex-M55 HP at 400 MHz, AC6 with the app
group built `optimize: speed`, display on:

| Stage | Where | Time |
|-------|-------|------|
| Vertex transform, 8 x 512 vertices, 2 batched matmuls | NPU | 0.34 ms (0.17 ms of it copying the int16 tensors) |
| Perspective divide + viewport, 4096 vertices | CPU, Helium | 0.68 ms |
| Clear + rasterize (155 k to 180 k box pixels tested, 30 k to 34 k covered, ~1900 front-facing triangles) | CPU, Helium | 17 to 18.6 ms (clear 0.3 ms) |
| Lights + highlight + AO + fog + bloom + 5x5 filter + 2x bilinear upscale, 480x800 | NPU | 24.7 ms, of which 2.5 ms is the Helium frame copy into the back buffer; vsync wait 0 |
| Reference check (every 120th frame) | CPU | 64 ms |
| Frame | | 44 to 45 ms, 22 to 23 fps |

The NPU output matches a float reference of the same shading (lights,
highlight, AO, fog, bloom, filter and resize, from the quantized G-buffer)
within 2.6/255 on a 12 x 20 grid of frame pixels, checked on every reported
frame.

For comparison, the first version of this demo (two objects, one Lambert
light, a 3x3 filter: 4.4 M MACs) ran at 53 fps with 8.1 ms of NPU shading;
the shade graph is now four times the MACs and five times the SRAM traffic,
and the NPU is the largest item in the frame.

Reading the numbers:

- **The NPU shading is memory-bound, not MAC-bound.** 16.9 M MACs would take
  the 256-MAC array 66 k cycles, 0.17 ms at 400 MHz; the 22 ms of NPU time
  are Vela's 62 MB of SRAM traffic at about 2.8 GB/s. Every 240x400
  feature map between the ~400 elementwise and convolution operators is
  read and written in the NPU's NHCWB16 layout, which pads the 3- and
  1-channel planes to 16 channels: 1.5 MB per plane instead of 288 kB or
  96 kB. Fewer, wider tensors (or fusing the elementwise chain) would cut
  the time far more than fewer MACs.
- **Vertex transform stays essentially free** at 8 x 512 vertices; half of
  its 0.34 ms is the input/output copy.
- **The Helium rasterizer at 17 to 18.6 ms is the second item**, for 30 k to
  34 k covered pixels out of up to 180 k bounding-box pixels: the moons' and
  the tori's boxes overlap a lot.
- The 64 ms reference check on every 120th frame is a visible hitch; it
  builds the bloom and AO references over the whole G-buffer.

## Lessons from making the shade graph heavier

- **`spec * spec` is not the way to raise to a power.** Four squarings of a
  1-channel plane lowered to int32 multiplies whose rescales Vela did not
  fuse, and each 240x400 int32 intermediate, 16-channel padded, is 6 MB:
  Vela wanted 14.25 MB of scratch. `torch.pow(x, 16.0)` on a quantized
  tensor becomes one int8 TABLE lookup (`DecomposeIntPowPass` leaves it to
  `InsertTableOpsPass`) and the peak fell to 3.94 MB.
- **A clamp shares its quantization with its input.** With the final
  `clamp(0, 1)` after the post filter, the frame was quantized on the
  filter's unclamped range (about [0, 2.8] in the calibration, scale
  0.011): 90 levels for the visible range. Clamping *before* the filter
  gives the filter's own observer a [0, 1] input and the frame the full
  8-bit range (scale 1/255).
- **Both-sides broadcasts are avoided** by tinting the 1-channel highlight
  with a 1x1 conv (1 → 3 channels) instead of multiplying it by a
  (1, 3, 1, 1) colour.
- **Memory on the DevKit-E8 is now the 8 MB SRAM, nearly full**: the temp
  pool is 4 MB for the 3.94 MB scratch, the method pool 1.25 MB for the
  planned buffers (1.15 MB frame output, 64 kB vertex outputs), plus two
  1.15 MB frame buffers, the z-buffer, the scene's vertex arrays (the
  DTCM holds the G-buffer and no more) and the 72 kB bloom reference;
  103 kB are left. The reference check borrows the z-buffer, free after
  rasterization, for its separable 7x7 depth box sum.

## How the export differs from the TinyCNN example

- `model/model.py` returns a list of `MethodSpec` (module, calibration
  samples, activation bits) and `create_ai_layer.py` exports them into one
  `.pte` with two methods, using `to_edge_transform_and_lower` with a dict of
  programs and partitioners.
- Both methods take and return **quantized tensors**. The Arm backend leaves
  the float quantize/dequantize on the CPU by default; the exporter removes
  them with ExecuTorch's `quantize_input` / `quantize_output` passes
  (`executorch.exir.passes.quantize_io_pass`) and writes the resulting
  scales, zero points, shapes and C types to `ai_layer/model_io.h`, which the
  rasterizer uses to encode the G-buffer directly. No float tensor and no
  CPU operator is left in the program (`operators: []` in the layer).
- Inputs are **not memory-planned** (`MemoryPlanningPass(alloc_graph_input=False)`):
  otherwise the runtime reserves planned memory for every input and memcpys
  the caller's tensors into it on each call, 672 kB per frame for the
  G-buffer, and the shade method did not fit its pool.
- Int16 activations (`get_symmetric_a16w8_quantization_config`) work for the
  batched matmul on the U85 with Vela 5.1; the calibration samples pin the
  ranges (|pos| ≤ 1, |matrix| ≤ 4, |clip| ≤ 8) so the fixed-point scale is
  2⁻¹² everywhere.
- Vela wants 3.94 MB of scratch for the 480x800 shade graph, so the
  DevKit-E8 board layer sets the ExecuTorch temp pool to 4 MB and the
  method pool to 1.25 MB (bulk SRAM0+SRAM1, AC6 scatter file), next to the
  two 1.15 MB frame buffers, the z-buffer and the vertex arrays. The 672 kB
  int8 G-buffer lives in the DTCM.

## Display

The board layer adds the pack's CDC200, MIPI DSI, DPHY, GPIO and ILI9806E
panel components and powers the DPHY in `main.c` as the pack's own
DevKit-e8 layer does. `board_display.c` wraps the CDC200 driver in the same
sequence the pack's VideoOut driver uses: Initialize, PowerControl, enable
the scanline-0 event, configure the display, set a frame buffer, Start.
The layer's `RTE_Device.h` (the pack's DevKit-E8 configuration) sets
RGB888, 480x800 at 60 fps, and the panel variant. The app defines
`APP_HAS_DISPLAY`, `APP_DISPLAY_WIDTH/HEIGHT` and `APP_FRAMEBUFFER_SECTION`
come from the layer, so the same `app_main.cpp` builds headless for the
FVP.

## Recording a video of the target with SDS

The DevKit-E8 build records what the panel shows through the
[SDS-Framework](https://github.com/ARM-software/SDS-Framework): the runner
opens two SDS streams over the J-Link OB's RTT channel 1, `NpuRender` with
one 240x400 RGB888 frame per record (the 480x800 frame buffer box-downscaled
2x2, since the NPU output is a 2x bilinear upscale of a 240x400 image anyway)
and `Timing` with one record of per-stage microsecond timings per frame.
SDSIO-Server writes them as `.sds` files, `sds-convert` turns the frame
stream into an MP4 and `sds-view` plots the timings. The animation advances
by 1/50 s per frame, so a 50 fps video plays in real time whatever the
recording rate was.

What the board layer adds (`board/DevKit-E8/Board-U85.clayer.yml`):
`SDS:Stream&CMSIS-RTOS2`, `SDS:IO:RTT`, `SEGGER:RTT`, and, because the SDS
Stream component needs CMSIS-RTOS2, `CMSIS:RTOS2:Keil RTX5&Source` with the
SysTick OS tick; `main.c` runs `app_main` in an RTX thread with a 32 kB
stack in the bulk SRAM, the handler stack shrinks to 16 kB. The config
files are in `board/DevKit-E8/RTE/{CMSIS,SDS,SEGGER}`: a 16 kB RTT up
buffer, 2 kB down, 16 kB RTX dynamic memory, 4 streams.

How the runner records (`src/app_main.cpp`, `APP_HAS_SDS`): at start it
calls `sdsInit`, then watches the RTT down buffer for 1.5 s: a connected
SDSIO-Server sends a flags message every 100 ms, so bytes arrive if one is
there. Without a server it prints "not recording" and runs the demo as
before (the probe is needed because the SDS 3.1.0 client's `sdsioOpen`
never returns when nobody answers: its receive loop does not exit on a
timeout). With a server it records the first `APP_SDS_RECORD_FRAMES`
frames (500, 10 s of video). No memory is free for a frame-sized stream
buffer, so while recording the display is single-buffered on frame buffer
0 and frame buffer 1 (1.15 MB) is the SDS stream buffer: up to three
frames queue there and the SDS thread drains them over RTT while the next
frame renders; when it is full the runner waits (`SDS_NO_SPACE`,
`osDelay`). Every recorded frame also calls `sdsExchange`, which consumes
the server's flags messages (the 2 kB down buffer fills in seconds
otherwise and the bridge stalls) and reports the runner's status. The 2x2
downscale is staged in the NPU temp pool, idle between NPU calls. After
the last frame both streams are closed (flushed) and the display goes back
to double buffering; after a link error they are left open, since
`sdsClose` would wait forever on a dead link. The RTT throughput sets the
recording rate, not the renderer.

Host side, in this order (the runner's `sdsOpen` gives the server 5 s):

```bash
# 1. Halt the board at its reset vector (J-Link attaches reliably only then)
pyocd commander --cbuild-run out/cmsis-executorch+DevKit-E8.cbuild-run.yml -c "reset halt"
# 2. RTT bridge: J-Link DLL via pylink, TCP on 5050; resumes the CPU when the server connects
.venv/bin/python tools/sdsio_rtt_bridge.py --device AE822FA0E5597LS0_M55_HP \
    --rtt-addr $(grep -o "0x[0-9a-f]* *0x000000a8 *Zero *RW.*_SEGGER_RTT" out/cmsis-executorch/DevKit-E8/Debug/cmsis-executorch.axf.map | cut -d' ' -f1)
# 3. SDSIO-Server in connect mode, files land in recordings/
.venv/bin/python ~/.cache/arm/packs/ARM/SDS/3.1.0/utilities/sdsio-server.py socket --port 5050 --connect --workdir recordings
# 4. When the console says "SDS: recording closed", convert
.venv/bin/python ~/.cache/arm/packs/ARM/SDS/3.1.0/utilities/sds-convert.py video \
    -i recordings/NpuRender.0.sds -o recordings/npu-render -y recordings/NpuRender.sds.yml
```

The `_SEGGER_RTT` address comes from the linker map because the J-Link's
RAM search does not cover the M55_HP DTCM. The utilities need
`pyserial opencv-python pyyaml pandas ifaddr libusb1 pylink-square` in the
venv (`uv pip install --python .venv/bin/python ...`).

Measured: the bridge moves 145 kB/s through the J-Link OB, so a 288 kB
frame takes 2 s and the 500-frame recording 17 minutes; the renderer's
own stages are unchanged meanwhile (vertex 0.36 ms, raster 18 to 40 ms,
shade 24.8 ms per frame, from the Timing stream). `sds-convert video`
writes an OpenCV `mp4v` file; `ffmpeg -c:v libx264 -pix_fmt yuv420p`
re-encodes it for players that need H.264.

Why the bridge instead of J-Link Commander's RTT telnet server: with
JLinkExe attached and SDSIO-Server on `--connect '$$SEGGER_TELNET_ConfigStr=RTTCh;1$$'`
against port 19021, the stream ran at 28 kB/s and lost bytes after four
frames every time ("Data integrity error - protocol mismatch"), while a
raw J-Link read of 256 kB takes about a second once the core is found on
AP[3]. The bridge polls the RTT up buffer directly through the DLL. pylink
must load the DLL of the installed JLinkExe (V9.24 here): its default pick
was an older V8.24 install that does not know the Alif device.

Pitfalls seen on the way:

- J-Link's own reset (`r`) on this board falls back to VECTRESET or the
  reset pin and did not restart the application reliably; pyOCD's
  `reset halt` followed by a J-Link `g` did.
- Attaching J-Link or pyOCD to a running application failed intermittently
  ("Could not find core in Coresight setup"). Halt with pyOCD first.
- After several aborted attaches the SoC stopped powering up its debug
  domain altogether (J-Link: SW-DP found, "Failed to power up DAP"; pyOCD:
  "Not supported by current CPU + target interface combination") and the
  console went silent: only a power cycle of the board recovers that.

Open issue (2026-09-11): after several minutes the DTCM shows sparse
corruption, a 16-bit word holding the low half of its own address every
1664 bytes, marching through the vertex input tensors among others. In the
10 s recording it appears after about 4 s as streaks (vertices flung
across the frame) and once ended a run with `vertex method failed
(err=18)`. Single-word reads through pyOCD and J-Link confirm the memory
content; MRAM reads back bit-exact, so it is not a read artefact. An 8 min
run of the same build with no debugger attached and no server showed the
pattern only in the heap and the unused DTCM gap, at the same addresses as
before the power cycle, so those may be fossils; the vertex tensors stayed
clean in that run. The writer is still unidentified; a data watchpoint at
the next predicted address (`pyocd commander -c "watch ADDR w 2"`) is the
next step.

## Running it

Same three steps as the README, on the `DevKit-E8` target-type:

```bash
cbuild setup cmsis-executorch.csolution.yml --active DevKit-E8
python3 create_ai_layer.py cmsis-executorch.cbuild-mlops.yml
cbuild cmsis-executorch.csolution.yml --active DevKit-E8
```

Then **Debug** or **Run** in the CMSIS view (or ask the CMSIS Developer
Assistant to load and run it). The panel shows the animation; the console
on UART4 prints the per-stage timings every 120 frames.

Standalone (no debugger attached) the loop reports 22 to 23 fps, well
under the panel's 60 Hz, so the vsync wait is zero. If the board
hard-faults straight after a flash, see the README's troubleshooting note
on the MRAM loader: the pyOCD loader has also been seen to leave the first
16-byte MRAM line wrong (reset vector 0xfffffffd, IACCVIOL HardFault);
verify the first words after programming and reprogram with the core
halted.

The `SSE-320-U85` FVP target still builds; its timings are not those of
hardware.
