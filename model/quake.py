# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Quake on the NPU: the per-pixel stages behind Quake's software renderer.

Quake's renderer stays on the Cortex-M55 (BSP, edge list, spans, texture
mapping). What it hands over each frame is what the NPU is good at: planes of
the whole view. The hand-off pass on the CPU only resolves the palette, row by
row; the planes stay in Quake's 400 x 240 landscape layout, and the rotation to
the portrait panel is the NPU's: a transpose of H and W at the start of each
graph (the row order the CPU writes the planes in takes care of the mirror):

  qpresent(albedo)                      The display back end for the stock
                                        8-bit frame (lit on the CPU through
                                        Quake's colormap): 2x bilinear upscale
                                        to the panel, interleaved RGB888 out.
  qshade(albedo, light, blend_k, blend_c)
                                        The NPU does the lighting. The surface
                                        cache holds unlit texels plus an 8-bit
                                        light value; the NPU multiplies them in
                                        RGB (no colormap banding), adds a
                                        quarter-resolution bloom, applies
                                        Quake's screen blend (damage, pickups,
                                        under water: v_blend) and upscales.

`light` is a linear multiplier, code / 128: code 128 is exactly 1.0 and marks
unlit pixels (sky, water, particles, fullbright texels, the HUD), the codes
above it are Quake's overbright range. A product above 1.0 saturates: the
calibration keeps albedo * light within [0, 1], so the multiply is quantized on
the full 8-bit range and the NPU clamps what exceeds it, like the explicit
clamp does in the float module.

`blend_k` = 1 - a and `blend_c` = a * blend_rgb are the two per-frame
parameters of Quake's v_blend (frame = frame * (1 - a) + rgb * a).
"""

from __future__ import annotations

import torch
from torch import nn

from model import BLOOM_DOWN, BLOOM_GAIN, BLOOM_THRESHOLD, FRAME_HEIGHT, FRAME_WIDTH, GAUSS_1D, UPSCALE, MethodSpec, _depthwise

LIGHT_UNLIT_CODE = 128  # light code of 1.0: unlit pixels
LIGHT_MAX = 255.0 / LIGHT_UNLIT_CODE  # 1.992: pins the light input scale to 1 / 128


VIEW_HEIGHT, VIEW_WIDTH = FRAME_WIDTH, FRAME_HEIGHT  # Quake's view, landscape: 240 x 400


def _upscale(x: torch.Tensor, mode: str) -> torch.Tensor:
    """2x to the panel resolution. The Ethos-U85 does it bilinear; on the Ethos-U55 only the
    nearest-neighbour resize is delegated (a bilinear one stays on the CPU, as a float operator)."""
    if mode == "nearest":
        return nn.functional.interpolate(x, scale_factor=UPSCALE, mode="nearest")
    return nn.functional.interpolate(x, scale_factor=UPSCALE, mode="bilinear", align_corners=False)


class QuakePresent(nn.Module):
    """Rotate to the portrait panel (transpose), 2x upscale, NCHW -> NHWC for the display."""

    def __init__(self, upscale: str = "bilinear") -> None:
        super().__init__()
        self.upscale = upscale

    def forward(self, albedo: torch.Tensor) -> torch.Tensor:
        albedo = albedo.permute(0, 1, 3, 2)  # (1, 3, 240, 400) -> (1, 3, 400, 240)
        return _upscale(albedo, self.upscale).permute(0, 2, 3, 1)


class QuakeShade(nn.Module):
    """Lighting in RGB, bloom, screen blend, upscale."""

    def __init__(self, bloom: bool = True, upscale: str = "bilinear") -> None:
        super().__init__()
        self.bloom = bloom
        self.upscale = upscale
        gauss = torch.tensor(GAUSS_1D)
        gauss = torch.outer(gauss, gauss)
        gauss = gauss / gauss.sum()
        self.bloom_pool = _depthwise(3, torch.full((BLOOM_DOWN, BLOOM_DOWN), 1.0 / (BLOOM_DOWN * BLOOM_DOWN)), stride=BLOOM_DOWN)
        self.bloom_blur = _depthwise(3, gauss, padding=2)

    def forward(self, albedo: torch.Tensor, light: torch.Tensor, blend_k: torch.Tensor, blend_c: torch.Tensor) -> torch.Tensor:
        albedo = albedo.permute(0, 1, 3, 2)  # landscape planes -> the portrait panel
        light = light.permute(0, 1, 3, 2)
        color = torch.clamp(albedo * light, 0.0, 1.0)  # (1, 3, H, W) x (1, 1, H, W)
        if self.bloom:
            bright = torch.relu(color - BLOOM_THRESHOLD)
            small = self.bloom_blur(self.bloom_blur(self.bloom_pool(bright)))
            glow = nn.functional.interpolate(small, scale_factor=BLOOM_DOWN, mode="bilinear", align_corners=False)
            color = torch.clamp(color + BLOOM_GAIN * glow, 0.0, 1.0)
        # The blend is quantized on its own range, [0, 1] (k + c <= 1), so the
        # frame keeps the full 8 bits whatever the clamps before it share.
        color = color * blend_k + blend_c  # (1, 3, H, W) with (1, 3, 1, 1) parameters
        return _upscale(color, self.upscale).permute(0, 2, 3, 1)


SURFCACHE_BYTES = 600 * 1024 + (VIEW_WIDTH * VIEW_HEIGHT - 64000) * 3  # vid_alif.c: SURFCACHE_SIZE, 710400
DIRECT = -1  # frame byte 255 (Quake's transparent colour) as int8: "this pixel is a texel offset"
MAX_ALIAS_VERTS = 2048  # r_local.h: MAXALIASVERTS is 2000


class QuakeFetch(nn.Module):
    """The whole per-pixel back end on the NPU (Ethos-U85: it needs GATHER and SELECT).

    Quake's span drawer no longer fetches texels: it stores, per pixel, the
    offset of the texel in the surface cache. Everything that is not a world
    span (models, particles, sprites, sky, water, the 2D) still writes colour
    bytes into the 8-bit frame, where the span drawer leaves 255.

      texel  = cache[offsets]                 GATHER, 96 000 of 710 400 bytes
      pixel  = frame == 255 ? texel : frame   SELECT
      rgb    = palette[pixel + 128]           GATHER (the CPU rotates the palette by 128)
      -> flip, rotate to the portrait panel, 2x bilinear, NHWC

    All integer tensors are raw bytes (uint8 seen as int8) and int32 offsets:
    nothing is quantized before the palette, so the result is bit-exact up to
    the bilinear filter."""

    def forward(self, cache: torch.Tensor, offsets: torch.Tensor, frame: torch.Tensor, palette: torch.Tensor) -> torch.Tensor:
        texel = nn.functional.embedding(offsets, cache).view(-1).to(torch.int32)  # (N,)
        frame = frame.to(torch.int32)  # EQUAL and SELECT work on int32
        pixel = torch.where(frame == DIRECT, texel, frame)
        rgb = nn.functional.embedding(pixel + 128, palette)  # (N, 3)
        x = torch.flip(rgb.view(1, VIEW_HEIGHT, VIEW_WIDTH, 3), dims=[1])  # the panel's row order
        x = x.permute(0, 3, 2, 1)  # NHWC landscape -> NCHW portrait (1, 3, 400, 240)
        return _upscale(x, "bilinear").permute(0, 2, 3, 1)


class QuakeAliasVerts(nn.Module):
    """R_AliasTransformAndProjectFinalVerts for all the alias models of a frame in one call:
    3x4 view transform of the (already 8-bit) model vertices, perspective divide, Gouraud light.

      verts   (1, N, 4)  x, y, z of trivertx_t scaled to [0, 1], and 1
      xform   (1, 4, 3)  aliastransform transposed, per call (one model per call, or the CPU
                         pre-multiplies per-model transforms into the vertices' rows)
      ndotl   (1, N, 1)  r_avertexnormals[lightnormalindex] . r_plightvec
      light   (1, 1, 2)  r_ambientlight, r_shadelight, both / 256

    out (1, N, 4): x / z, y / z, 1 / z, light. 16-bit activations: screen coordinates need them."""

    def forward(self, verts: torch.Tensor, xform: torch.Tensor, ndotl: torch.Tensor, light: torch.Tensor) -> torch.Tensor:
        view = torch.bmm(verts, xform)  # (1, N, 3)
        zi = torch.reciprocal(view[:, :, 2:3])
        uv = view[:, :, 0:2] * zi
        lit = torch.clamp(light[:, :, 0:1] + light[:, :, 1:2] * torch.clamp(ndotl, max=0.0), min=0.0)
        return torch.cat([uv, zi, lit], dim=2)


def _fetch_samples() -> list[tuple[torch.Tensor, ...]]:
    g = torch.Generator().manual_seed(4)
    n = VIEW_HEIGHT * VIEW_WIDTH
    out = []
    for _ in range(3):
        cache = torch.randint(-128, 128, (SURFCACHE_BYTES, 1), generator=g, dtype=torch.int8)
        offsets = torch.randint(0, SURFCACHE_BYTES, (n,), generator=g, dtype=torch.int32)
        frame = torch.randint(-128, 128, (n,), generator=g, dtype=torch.int8)
        palette = torch.rand(256, 3, generator=g)
        palette[0], palette[1] = 0.0, 1.0  # the full range: scale 1 / 255
        out.append((cache, offsets, frame, palette))
    return out


def _alias_samples() -> list[tuple[torch.Tensor, ...]]:
    """View space as Quake has it for a model in front of the camera: z in [0.25, 1] of the far
    distance the calibration stands for, x and y within the frustum (|x|, |y| <= z)."""
    g = torch.Generator().manual_seed(5)
    n = MAX_ALIAS_VERTS
    out = []
    for _ in range(4):
        verts = torch.cat([torch.rand(1, n, 3, generator=g), torch.ones(1, n, 1)], dim=2)
        xform = torch.zeros(1, 4, 3)
        xform[0, :3, :2] = (torch.rand(3, 2, generator=g) - 0.5) * 0.3
        xform[0, :3, 2] = torch.rand(3, generator=g) * 0.2
        xform[0, 3, 2] = 0.25 + torch.rand(1, generator=g).item() * 0.15  # z stays in [0.25, 1]
        ndotl = torch.rand(1, n, 1, generator=g) * 2.0 - 1.0
        light = torch.rand(1, 1, 2, generator=g)
        out.append((verts, xform, ndotl, light))
    return out


def _present_samples() -> list[tuple[torch.Tensor, ...]]:
    h, w = VIEW_HEIGHT, VIEW_WIDTH
    g = torch.Generator().manual_seed(2)
    return [(torch.ones(1, 3, h, w),), (torch.zeros(1, 3, h, w),), (torch.rand(1, 3, h, w, generator=g),)]


def _shade_samples() -> list[tuple[torch.Tensor, ...]]:
    """Calibration: albedo, k, c in [0, 1], light in [0, LIGHT_MAX], and albedo * light never above 1."""
    h, w = VIEW_HEIGHT, VIEW_WIDTH
    g = torch.Generator().manual_seed(3)

    def params(a: float, rgb: tuple[float, float, float]) -> tuple[torch.Tensor, torch.Tensor]:
        return torch.full((1, 3, 1, 1), 1.0 - a), (a * torch.tensor(rgb)).view(1, 3, 1, 1)

    def random_lit() -> tuple[torch.Tensor, torch.Tensor]:
        light = torch.rand(1, 1, h, w, generator=g) * LIGHT_MAX
        albedo = torch.rand(1, 3, h, w, generator=g) * torch.clamp(1.0 / light.clamp(min=1e-3), max=1.0)
        return albedo, light

    # Bright blocks on black: bloom edges at full contrast.
    blocks = torch.zeros(1, 3, h, w)
    blocks[:, :, ::32, :] = 1.0
    blocks[:, :, :, ::32] = 1.0
    return [
        (torch.ones(1, 3, h, w), torch.ones(1, 1, h, w), *params(0.0, (0.0, 0.0, 0.0))),  # unlit white, no blend
        (torch.full((1, 3, h, w), 0.5), torch.full((1, 1, h, w), LIGHT_MAX), *params(0.0, (0.0, 0.0, 0.0))),  # full overbright
        (torch.zeros(1, 3, h, w), torch.zeros(1, 1, h, w), *params(1.0, (1.0, 1.0, 1.0))),  # black, fully blended to white
        (blocks, torch.ones(1, 1, h, w), *params(0.4, (1.0, 0.0, 0.0))),  # bloom edges, pain flash
        (*random_lit(), *params(0.2, (0.3, 0.4, 0.8))),  # under water
        (*random_lit(), *params(0.0, (0.0, 0.0, 0.0))),
    ]


def get_quake_methods(bloom: bool = True, upscale: str = "bilinear", gather: bool = True) -> list[MethodSpec]:
    methods = [
        MethodSpec("qpresent", QuakePresent(upscale).eval(), _present_samples()),
        MethodSpec("qshade", QuakeShade(bloom=bloom, upscale=upscale).eval(), _shade_samples()),
    ]
    if gather:  # Ethos-U85 material: GATHER, SELECT, the int16 matmul
        methods += [
            MethodSpec("qfetch", QuakeFetch().eval(), _fetch_samples()),
            MethodSpec("qvert", QuakeAliasVerts().eval(), _alias_samples(), activation_bits=16),
        ]
    return methods


class QuakePresentLowRes(nn.Module):
    """qpresent for a quarter-size view (200 x 120): the CPU renders a quarter of the pixels,
    the NPU scales 4x to the panel (nearest neighbour: what the Ethos-U55 resizes).

    Measured on the M55-HE + Ethos-U55 (timedemo demo1): CPU busy 37.3 -> 23.6 ms per frame,
    NPU busy 14.4 ms, but 2625 KiB of scratch, and the picture was judged not acceptable.
    Not part of any layer; kept for the record."""

    def forward(self, albedo: torch.Tensor) -> torch.Tensor:
        x = albedo.permute(0, 1, 3, 2)  # (1, 3, 120, 200) -> (1, 3, 200, 120)
        return nn.functional.interpolate(x, scale_factor=2 * UPSCALE, mode="nearest").permute(0, 2, 3, 1)


class QuakePresentNHWC(nn.Module):
    """qpresent for the Ethos-U55: the CPU hands over the frame as the panel wants it, rotated and
    interleaved (1, 400, 240, 3). The permute cancels the one the backend puts at the input, the
    graph is the resize alone: one NPU operator."""

    def forward(self, frame: torch.Tensor) -> torch.Tensor:
        return _upscale(frame.permute(0, 3, 1, 2), "nearest").permute(0, 2, 3, 1)


class QuakeShadeNHWC(nn.Module):
    """qshade for the Ethos-U55 on rotated NHWC planes: lighting, screen blend, upscale; no bloom."""

    def forward(self, albedo: torch.Tensor, light: torch.Tensor, blend_k: torch.Tensor, blend_c: torch.Tensor) -> torch.Tensor:
        color = torch.clamp(albedo.permute(0, 3, 1, 2) * light.permute(0, 3, 1, 2), 0.0, 1.0)
        color = color * blend_k + blend_c
        return _upscale(color, "nearest").permute(0, 2, 3, 1)


def _nhwc(samples: list[tuple[torch.Tensor, ...]], planes: int) -> list[tuple[torch.Tensor, ...]]:
    """Landscape NCHW calibration planes as rotated NHWC ones; the parameters after them stay."""
    return [tuple(t.permute(0, 3, 2, 1).contiguous() if i < planes else t for i, t in enumerate(sample)) for sample in samples]


def get_quake_u55_methods() -> list[MethodSpec]:
    """The flavour for the Ethos-U55 (the NPU of the M55-HE): what that NPU takes whole.
    No bloom (its depthwise convolutions are rejected), nearest-neighbour upscale."""
    return [
        *get_quake_methods(bloom=False, upscale="nearest", gather=False),
        MethodSpec("qpresent_nhwc", QuakePresentNHWC().eval(), _nhwc(_present_samples(), 1)),
        MethodSpec("qshade_nhwc", QuakeShadeNHWC().eval(), _nhwc(_shade_samples(), 2)),
    ]
