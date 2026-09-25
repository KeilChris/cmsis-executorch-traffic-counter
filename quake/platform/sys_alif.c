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
// sys_alif.c -- system driver for the Alif DevKit-E8 and the Corstone-320
// FVP, from sys_null.c. File IO stays on stdio: the C library's file
// interface is retargeted to the pak image in memory (fs_pak_mem.c). Memory,
// time and the way out come from the target side through port.h.

#include "quakedef.h"
#include "errno.h"
#include "port.h"

qboolean			isDedicated;

/*
===============================================================================

FILE IO

===============================================================================
*/

#define MAX_HANDLES             10
FILE    *sys_handles[MAX_HANDLES];

int             findhandle (void)
{
	int             i;

	for (i=1 ; i<MAX_HANDLES ; i++)
		if (!sys_handles[i])
			return i;
	Sys_Error ("out of handles");
	return -1;
}

/*
================
filelength
================
*/
int filelength (FILE *f)
{
	int             pos;
	int             end;

	pos = ftell (f);
	fseek (f, 0, SEEK_END);
	end = ftell (f);
	fseek (f, pos, SEEK_SET);

	return end;
}

int Sys_FileOpenRead (char *path, int *hndl)
{
	FILE    *f;
	int             i;

	i = findhandle ();

	f = fopen(path, "rb");
	if (!f)
	{
		*hndl = -1;
		return -1;
	}
	sys_handles[i] = f;
	*hndl = i;

	return filelength(f);
}

int Sys_FileOpenWrite (char *path)
{
	FILE    *f;
	int             i;

	i = findhandle ();

	f = fopen(path, "wb");
	if (!f)
		Sys_Error ("Error opening %s: %s", path,strerror(errno));
	sys_handles[i] = f;

	return i;
}

void Sys_FileClose (int handle)
{
	fclose (sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	fseek (sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	return fread (dest, 1, count, sys_handles[handle]);
}

int Sys_FileWrite (int handle, void *data, int count)
{
	return fwrite (data, 1, count, sys_handles[handle]);
}

int     Sys_FileTime (char *path)
{
	FILE    *f;

	f = fopen(path, "rb");
	if (f)
	{
		fclose(f);
		return 1;
	}

	return -1;
}

void Sys_mkdir (char *path)
{
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
}


void Sys_Error (char *error, ...)
{
	va_list         argptr;

	printf ("Sys_Error: ");
	va_start (argptr,error);
	vprintf (error,argptr);
	va_end (argptr);
	printf ("\n");

	port_exit (1);
}

void Sys_Printf (char *fmt, ...)
{
	va_list         argptr;
	static char     text[1024];

	va_start (argptr,fmt);
	vsnprintf (text, sizeof(text), fmt, argptr);
	va_end (argptr);

	port_console_line (text);
#ifdef PORT_TIMEDEMO
	if (!strncmp (text, "PackFile:", 9))
		return;		// one line per file Quake opens: not in a benchmark log
#endif
	fputs (text, stdout);
}

void Sys_Quit (void)
{
	port_exit (0);
}

double Sys_FloatTime (void)
{
	return port_time ();
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_Sleep (void)
{
}

void Sys_SendKeyEvents (void)
{
}

void Sys_HighFPPrecision (void)
{
}

void Sys_LowFPPrecision (void)
{
}

// For the target side, which does not see Quake's headers: run a console command.
void port_queue_command (const char *text)
{
	Cbuf_AddText ((char *)text);
}

//=============================================================================

int quake_main (int argc, char **argv)
{
	static quakeparms_t    parms;
	double          time, oldtime, newtime;

	parms.memsize = (int)port_hunk_size ();
	parms.membase = port_hunk_base ();
	parms.basedir = ".";

	COM_InitArgv (argc, argv);

	parms.argc = com_argc;
	parms.argv = com_argv;

	printf ("Host_Init\n");
	Host_Init (&parms);

	oldtime = Sys_FloatTime ();
	while (1)
	{
		newtime = Sys_FloatTime ();
		time = newtime - oldtime;
		oldtime = newtime;
		Host_Frame (time);
	}
	return 0;
}
