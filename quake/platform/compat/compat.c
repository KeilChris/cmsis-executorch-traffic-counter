/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The POSIX calls of Quake's console debug log; see unistd.h.
 */
#include "unistd.h"

int open(const char *path, int flags, ...) {
  (void)path;
  (void)flags;
  return -1;
}

int close(int fd) {
  (void)fd;
  return -1;
}

int write(int fd, const void *data, unsigned count) {
  (void)fd;
  (void)data;
  (void)count;
  return -1;
}

int unlink(const char *path) {
  (void)path;
  return -1;
}
