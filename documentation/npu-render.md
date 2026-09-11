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
