# Copyright 2026 Arm Limited and/or its affiliates.
# SPDX-License-Identifier: Apache-2.0
"""NPU render: the tensor-shaped stages of a small 3D pipeline, for Ethos-U85.

The Ethos-U85 executes a precompiled command stream over static shapes, so it
cannot rasterize. It can, however, run the parts of a 3D pipeline that are
plain tensor math, and this module defines those as two ExecuTorch methods
that create_ai_layer.py quantizes and compiles for the NPU:

  vertex(pos, nrm, mvp, mv)  int16   Batch vertex transform: clip-space
                                     positions and view-space normals as two
                                     batched matmuls; the batch dimension is
                                     the object, each with its own matrices.
  shade(normal, albedo, depth) int8  Deferred shading of a G-buffer: three
                                     coloured directional lights (1x1 convs),
                                     a Blinn-Phong specular highlight (n.h
                                     to the 16th power, a table lookup),
                                     a screen-space ambient-occlusion term
                                     from the depth plane (7x7 box blur),
                                     depth fog, a bloom pass at quarter
                                     resolution (threshold, 4x4 pool, two
                                     5x5 Gaussians, 4x bilinear up), a clamp
                                     and a 5x5 depthwise post filter, then a 2x
                                     bilinear upscale to the panel
                                     resolution and a transpose to an
                                     interleaved RGB888 frame the display
                                     controller scans out directly.

The CPU (src/app_main.cpp) owns everything in between: perspective divide,
culling, edge-function rasterization into the G-buffer, and the z-buffer.
Shapes are fixed at export time, so the vertex batch is padded to
MAX_OBJECTS x MAX_VERTICES, the G-buffer is FRAME_HEIGHT x FRAME_WIDTH and
the output is UPSCALE times that: the DevKit-E8 panel, 480 x 800 portrait.

The weights are hand-set (no training): light directions and colours, an
ambient term, the specular half vector, a fog colour and the filter kernels,
so the shaded image is predictable and the CPU-side reference in
app_main.cpp can check the NPU output. The constants below are mirrored
there.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import torch
from torch import nn

MAX_OBJECTS = 8  # bmm batch: one model-view / MVP pair per object
MAX_VERTICES = 512  # per object, padded (static shape)
FRAME_WIDTH = 240  # G-buffer, portrait: half the 480 x 800 panel in each axis
FRAME_HEIGHT = 400
UPSCALE = 2  # bilinear, on the NPU; the output is (1, H * UPSCALE, W * UPSCALE, 3)

# View space: the camera looks down +z, so a light in front of the scene has
# a negative z. Directions point towards the light and are unit length.
LIGHTS = [  # (direction, colour): key (warm), fill (cool), rim (from behind)
    ((0.30, 0.50, -0.81), (0.80, 0.72, 0.60)),
    ((-0.60, 0.10, -0.79), (0.15, 0.22, 0.35)),
    ((0.20, 0.60, 0.77), (0.55, 0.35, 0.60)),
]
AMBIENT = (0.12, 0.13, 0.16)
SPEC_COLOR = (0.90, 0.90, 0.85)  # Blinn-Phong highlight of the key light
SPEC_POWER = 16.0  # (n.h)^16, one int8 table lookup on the NPU
AO_RADIUS = 3  # 7x7 box blur of the depth plane
AO_STRENGTH = 6.0
FOG_COLOR = (0.10, 0.12, 0.18)
BLOOM_THRESHOLD = 0.55
BLOOM_GAIN = 0.7
BLOOM_DOWN = 4  # bloom runs at 60 x 100
GAUSS_1D = [1.0, 4.0, 6.0, 4.0, 1.0]  # 5x5 separable binomial kernel


def half_vector() -> tuple[float, float, float]:
    """Blinn-Phong half vector of the key light and the view direction (0, 0, -1)."""
    lx, ly, lz = LIGHTS[0][0]
    hx, hy, hz = lx, ly, lz - 1.0
    n = math.sqrt(hx * hx + hy * hy + hz * hz)
    return hx / n, hy / n, hz / n


@dataclass
class MethodSpec:
    """One ExecuTorch method: the module, its calibration/example inputs and the activation width."""

    name: str
    module: nn.Module
    samples: list[tuple[torch.Tensor, ...]] = field(default_factory=list)
    activation_bits: int = 8  # 8 or 16 (int16 activations, int8 weights)

    @property
    def example(self) -> tuple[torch.Tensor, ...]:
        return self.samples[0]


class VertexStage(nn.Module):
    """clip = pos x mvp, nview = nrm x mv, both as batched matmuls (row-vector convention)."""

    def forward(
        self, pos: torch.Tensor, nrm: torch.Tensor, mvp: torch.Tensor, mv: torch.Tensor
    ) -> tuple[torch.Tensor, torch.Tensor]:
        clip = torch.bmm(pos, mvp)  # (B, N, 4) x (B, 4, 4) -> (B, N, 4)
        nview = torch.bmm(nrm, mv)  # (B, N, 4) x (B, 4, 4) -> (B, N, 4), w = 0
        return clip, nview


def _depthwise(channels: int, kernel: torch.Tensor, stride: int = 1, padding: int = 0) -> nn.Conv2d:
    """A fixed depthwise convolution with the same 2D kernel on every channel."""
    k = kernel.shape[-1]
    conv = nn.Conv2d(channels, channels, kernel_size=k, stride=stride, padding=padding, groups=channels, bias=False)
    conv.weight.data = kernel.expand(channels, 1, k, k).clone()
    return conv


class ShadeStage(nn.Module):
    """Deferred shading of a planar G-buffer: lights, specular, AO, fog, bloom, post filter, upscale."""

    def __init__(self) -> None:
        super().__init__()
        # n.L of the three lights as one 1x1 conv, then the light colours as
        # another (its bias is the ambient term): light_rgb = C^T relu(L n) + ambient.
        self.lights = nn.Conv2d(3, 3, kernel_size=1, bias=False)
        self.lights.weight.data = torch.tensor([d for d, _ in LIGHTS]).view(3, 3, 1, 1)
        self.light_color = nn.Conv2d(3, 3, kernel_size=1, bias=True)
        self.light_color.weight.data = torch.tensor([c for _, c in LIGHTS]).t().contiguous().view(3, 3, 1, 1)
        self.light_color.bias.data = torch.tensor(AMBIENT)
        # Specular: n.h, relu, raised to a power, tinted by a 1x1 conv.
        self.half_dot = nn.Conv2d(3, 1, kernel_size=1, bias=False)
        self.half_dot.weight.data = torch.tensor(half_vector()).view(1, 3, 1, 1)
        self.spec_color = nn.Conv2d(1, 3, kernel_size=1, bias=False)
        self.spec_color.weight.data = torch.tensor(SPEC_COLOR).view(3, 1, 1, 1)
        # Ambient occlusion: how far the pixel is behind the mean depth around it.
        box = 2 * AO_RADIUS + 1
        self.ao_blur = _depthwise(1, torch.full((box, box), 1.0 / (box * box)), padding=AO_RADIUS)
        # Bloom: 4x4 mean pool as a strided depthwise conv, 5x5 Gaussians.
        gauss = torch.tensor(GAUSS_1D)
        gauss = torch.outer(gauss, gauss)
        gauss = gauss / gauss.sum()
        self.bloom_pool = _depthwise(3, torch.full((BLOOM_DOWN, BLOOM_DOWN), 1.0 / (BLOOM_DOWN * BLOOM_DOWN)), stride=BLOOM_DOWN)
        self.bloom_blur = _depthwise(3, gauss, padding=2)
        self.post = _depthwise(3, gauss, padding=2)
        self.register_buffer("fog_color", torch.tensor(FOG_COLOR).view(1, 3, 1, 1))

    def forward(self, normal: torch.Tensor, albedo: torch.Tensor, depth: torch.Tensor) -> torch.Tensor:
        ndotl = torch.relu(self.lights(normal))  # (1, 3, H, W): one channel per light
        light = self.light_color(ndotl)  # (1, 3, H, W): RGB irradiance incl. ambient
        # pow with a scalar exponent is a TABLE op once quantized. (Squaring
        # by spec * spec four times instead lowers to int32 multiplies whose
        # 16-channel-padded intermediates cost 6 MB each.)
        spec = torch.pow(torch.relu(self.half_dot(normal)), SPEC_POWER)  # (1, 1, H, W)
        occlusion = torch.relu(depth - self.ao_blur(depth))
        ao = torch.clamp(1.0 - AO_STRENGTH * occlusion, 0.0, 1.0)
        lit = albedo * light * ao + self.spec_color(spec)
        color = lit * (1.0 - depth) + self.fog_color * depth  # depth in [0, 1] is the fog factor
        bright = torch.relu(color - BLOOM_THRESHOLD)
        small = self.bloom_blur(self.bloom_blur(self.bloom_pool(bright)))
        glow = nn.functional.interpolate(small, scale_factor=BLOOM_DOWN, mode="bilinear", align_corners=False)
        color = torch.clamp(color + BLOOM_GAIN * glow, 0.0, 1.0)
        # The clamp shares its quantization with the unclamped colour (a range
        # of about [0, 3] in the calibration); the post filter after it gets
        # its own observer, on [0, 1], so the frame is quantized on the full
        # 8-bit range. A Gaussian of values in [0, 1] stays in [0, 1].
        color = self.post(color)
        frame = nn.functional.interpolate(color, scale_factor=UPSCALE, mode="bilinear", align_corners=False)
        return frame.permute(0, 2, 3, 1)  # NCHW -> NHWC: interleaved RGB rows, the display's RGB888 layout


def _vertex_samples() -> list[tuple[torch.Tensor, ...]]:
    """Calibration that pins the int16 ranges: |pos| <= 1, |nrm| <= 1, |matrix| <= 4, |out| <= 8."""
    b, n = MAX_OBJECTS, MAX_VERTICES
    # Every tensor is a distinct object: torch.export aliases inputs that
    # share one tensor and then drops the duplicate from the graph.
    ones = lambda: torch.ones(b, n, 4)  # noqa: E731
    two = lambda: torch.full((b, 4, 4), 2.0)  # noqa: E731  1 * 2 * 4 columns = 8 at the output
    ext = torch.zeros(b, 4, 4)
    ext[:, 0, 0], ext[:, 1, 1] = 4.0, -4.0
    g = torch.Generator().manual_seed(0)
    rnd = lambda *shape: torch.rand(*shape, generator=g) * 2 - 1  # noqa: E731
    return [
        (ones(), ones(), two(), two()),
        (-ones(), -ones(), two(), two()),
        (rnd(b, n, 4), rnd(b, n, 4), ext, ext.clone()),
        (rnd(b, n, 4), rnd(b, n, 4), rnd(b, 4, 4) * 2, rnd(b, 4, 4) * 2),
    ]


def _shade_samples() -> list[tuple[torch.Tensor, ...]]:
    """Calibration that pins the int8 ranges: normal in [-1, 1], albedo and depth in [0, 1]."""
    h, w = FRAME_HEIGHT, FRAME_WIDTH
    g = torch.Generator().manual_seed(1)

    def sample(normal_value: float | None, albedo_value: float | None, depth_value: float | None):
        normal = torch.rand(1, 3, h, w, generator=g) * 2 - 1
        normal = normal / normal.norm(dim=1, keepdim=True)
        albedo = torch.rand(1, 3, h, w, generator=g)
        depth = torch.rand(1, 1, h, w, generator=g)
        if normal_value is not None:
            normal.fill_(normal_value)
        if albedo_value is not None:
            albedo.fill_(albedo_value)
        if depth_value is not None:
            depth.fill_(depth_value)
        return normal, albedo, depth

    def facing(direction) -> torch.Tensor:
        return torch.tensor(direction).view(1, 3, 1, 1).expand(1, 3, h, w).clone()

    # Depth with sharp steps: the AO term needs occluded pixels in its calibration.
    stepped = torch.zeros(1, 1, h, w)
    stepped[:, :, ::16, :] = 1.0
    stepped[:, :, :, ::16] = 1.0
    return [
        (facing(LIGHTS[0][0]), torch.ones(1, 3, h, w), torch.zeros(1, 1, h, w)),  # key light head-on, white
        (facing(half_vector()), torch.ones(1, 3, h, w), torch.zeros(1, 1, h, w)),  # full specular
        sample(-1.0, 0.0, 1.0),  # darkest / full fog
        (facing(LIGHTS[0][0]), torch.ones(1, 3, h, w), stepped),  # AO edges, bloom edges
        sample(None, None, None),
        sample(None, None, None),
    ]


def get_methods() -> list[MethodSpec]:
    torch.manual_seed(0)
    return [
        MethodSpec("vertex", VertexStage().eval(), _vertex_samples(), activation_bits=16),
        MethodSpec("shade", ShadeStage().eval(), _shade_samples(), activation_bits=8),
    ]
