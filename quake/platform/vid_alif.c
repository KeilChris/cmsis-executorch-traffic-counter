/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright 2026 Arm Limited and/or its affiliates.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_alif.c -- video driver for the Alif DevKit-E8 and the Corstone-320 FVP,
// from vid_null.c. Quake renders QUAKE_VID_WIDTH x QUAKE_VID_HEIGHT palette
// indices into a buffer of its own; VID_Update hands the finished frame and
// the current palette to the target side, which presents it (CPU or NPU).

#include "quakedef.h"
#include "d_local.h"
#include "port.h"

// The buffers the inner loops live in: the scatter file of the board puts
// this section into the DTCM.
#define FAST_BSS	__attribute__((section(".bss.quake_fast"), aligned(32)))

#define	BASEWIDTH	QUAKE_VID_WIDTH
#define	BASEHEIGHT	QUAKE_VID_HEIGHT
#define	SURFCACHE_SIZE	(SURFCACHE_SIZE_AT_320X200 + (BASEWIDTH*BASEHEIGHT - 64000)*3)	// D_SurfaceCacheForRes

static byte	vid_buffer[BASEWIDTH*BASEHEIGHT] FAST_BSS;
// a section of its own: on the M55-HE it is the ITCM that has room for it (the scatter files place it)
static short	zbuffer[BASEWIDTH*BASEHEIGHT] __attribute__((section(".bss.quake_zbuf"), aligned(32)));
static byte	surfcache[SURFCACHE_SIZE] __attribute__((section(".bss.quake_surfcache"), aligned(32)));

static byte	vid_palette[768];	// the palette on screen: gamma and screen blend applied

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];

void	VID_SetPalette (unsigned char *palette)
{
	memcpy (vid_palette, palette, sizeof(vid_palette));
}

void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}

void	VID_Init (unsigned char *palette)
{
	vid.maxwarpwidth = vid.width = vid.conwidth = BASEWIDTH;
	vid.maxwarpheight = vid.height = vid.conheight = BASEHEIGHT;
	vid.aspect = 1.0;
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid.buffer = vid.conbuffer = vid_buffer;
	vid.rowbytes = vid.conrowbytes = BASEWIDTH;

	d_pzbuffer = zbuffer;
	D_InitCaches (surfcache, sizeof(surfcache));
#ifdef PORT_SPANSTEP
	{
		extern int	d_spanstep;		// d_scan.c: perspective-correct every 8 (default) or 16 pixels

		d_spanstep = PORT_SPANSTEP;
	}
#endif

	VID_SetPalette (palette);
}

void	VID_Shutdown (void)
{
}

// Frames per second, drawn into the finished frame with Quake's console font,
// top right. On by default; not in the benchmark build, whose frame checksums
// are compared between runs.
#if !defined(PORT_SHOW_FPS) && !defined(PORT_TIMEDEMO)
#define PORT_SHOW_FPS 1
#endif

#if PORT_SHOW_FPS
static void VID_DrawFps (void)
{
	static double	last;
	static int		count;
	static char		text[16];
	double			now = Sys_FloatTime ();

	count++;
	if (now - last >= 0.5)
	{
		sprintf (text, "%3d fps", (int)(count / (now - last) + 0.5));
		last = now;
		count = 0;
	}
	if (text[0])
		Draw_String (vid.width - 8 * (int)strlen(text) - 8, 8, text);
#ifdef PORT_SPANSTEP_AB
	// A/B on the panel: the span drawer's perspective step changes every four
	// seconds between 8 pixels (the C renderer) and 16 (id's assembly), labelled.
	{
		extern int	d_spanstep;		// d_scan.c

		d_spanstep = (((int)now / 4) & 1) ? 16 : 8;
		Draw_String (8, 8, d_spanstep == 16 ? "persp 16" : "persp  8");
	}
#endif
}
#endif

#ifdef __ARM_FEATURE_MVE
extern uint32_t	*d_spanoffsets;		// d_scan.c
extern byte		*d_spanoffsetbase;
#endif

void	VID_Update (vrect_t *rects)
{
#if PORT_SHOW_FPS
	VID_DrawFps ();
#endif
	port_present (vid_buffer, vid_palette);
#ifdef __ARM_FEATURE_MVE
	// the next frame: texels as ever, or texel offsets for the NPU to fetch (d_scan.c)
	d_spanoffsetbase = surfcache;
	d_spanoffsets = port_span_offsets (surfcache);
#endif
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}
