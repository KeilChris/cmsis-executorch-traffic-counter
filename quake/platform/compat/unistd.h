/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Quake's console.c includes <unistd.h> and <fcntl.h> for its debug log
 * (-condebug): open/write/close/unlink. There is no file system to write to
 * here; compat.c answers them with "no".
 */
#ifndef QUAKE_COMPAT_UNISTD_H
#define QUAKE_COMPAT_UNISTD_H

int open(const char *path, int flags, ...);
int close(int fd);
int write(int fd, const void *data, unsigned count);
int unlink(const char *path);

#endif
