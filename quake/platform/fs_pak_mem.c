/*
 * Copyright 2026 Arm Limited and/or its affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CMSIS-Compiler File Interface (Custom) over one pak image in memory.
 *
 * Quake reads its data with stdio: it opens id1/pak0.pak, reads the directory
 * and then reopens the pak for every file in it (COM_FOpenFile), seeks and
 * reads. The pak is not on a file system here: it is an image in memory, OSPI
 * flash in XIP mode on the DevKit-E8 and preloaded DDR on the FVP
 * (port_pak_image()). This file serves that image as the read-only file
 * "id1/pak0.pak" under any base directory; every other path does not exist,
 * and nothing can be written (Quake then skips config.cfg and save games).
 */

#include <string.h>

#include "retarget_fs.h"

#include "port.h"

#define PAK_NAME  "id1/pak0.pak"
#define MAX_OPEN  8 /* Quake holds the pak open once, plus one file in it at a time, plus a demo */

typedef struct {
  char    id[4]; /* "PACK" */
  int32_t dirofs;
  int32_t dirlen;
} pak_header_t;

typedef struct {
  uint8_t  open;
  uint32_t pos;
} file_t;

static file_t files[MAX_OPEN];

static uint32_t pak_size(const uint8_t *image) {
  pak_header_t header;
  memcpy(&header, image, sizeof(header));
  if (memcmp(header.id, "PACK", 4) != 0 || header.dirofs < 0 || header.dirlen < 0) {
    return 0U;
  }
  return (uint32_t)header.dirofs + (uint32_t)header.dirlen; /* the directory ends the file */
}

static int is_pak(const char *path) {
  size_t length = strlen(path);
  size_t name = sizeof(PAK_NAME) - 1U;
  if (length < name || strcmp(path + length - name, PAK_NAME) != 0) {
    return 0;
  }
  return (length == name) || (path[length - name - 1U] == '/');
}

int32_t rt_fs_open(const char *path, int32_t mode) {
  const uint8_t *image = port_pak_image();
  if (mode != RT_OPEN_RDONLY) {
    return RT_ERR_READONLY;
  }
  if (image == NULL || !is_pak(path) || pak_size(image) == 0U) {
    return RT_ERR_NOTFOUND;
  }
  for (int32_t fd = 0; fd < MAX_OPEN; fd++) {
    if (!files[fd].open) {
      files[fd].open = 1U;
      files[fd].pos = 0U;
      return fd;
    }
  }
  return RT_ERR_MAXFILES;
}

static file_t *file_of(int32_t fd) {
  return (fd >= 0 && fd < MAX_OPEN && files[fd].open) ? &files[fd] : NULL;
}

int32_t rt_fs_close(int32_t fd) {
  file_t *file = file_of(fd);
  if (file == NULL) {
    return RT_ERR_FILEDES;
  }
  file->open = 0U;
  return 0;
}

int32_t rt_fs_write(int32_t fd, const void *buf, uint32_t cnt) {
  (void)fd;
  (void)buf;
  (void)cnt;
  return RT_ERR_READONLY;
}

int32_t rt_fs_read(int32_t fd, void *buf, uint32_t cnt) {
  file_t *file = file_of(fd);
  if (file == NULL) {
    return RT_ERR_FILEDES;
  }
  const uint8_t *image = port_pak_image();
  uint32_t size = pak_size(image);
  uint32_t left = (file->pos < size) ? size - file->pos : 0U;
  if (cnt > left) {
    cnt = left;
  }
  memcpy(buf, image + file->pos, cnt);
  file->pos += cnt;
  return (int32_t)cnt;
}

int64_t rt_fs_seek(int32_t fd, int64_t offset, int32_t whence) {
  file_t *file = file_of(fd);
  if (file == NULL) {
    return RT_ERR_FILEDES;
  }
  int64_t size = (int64_t)pak_size(port_pak_image());
  int64_t base = (whence == RT_SEEK_SET) ? 0 : (whence == RT_SEEK_CUR) ? (int64_t)file->pos : size;
  int64_t pos = base + offset;
  if ((whence != RT_SEEK_SET && whence != RT_SEEK_CUR && whence != RT_SEEK_END) || pos < 0 || pos > size) {
    return RT_ERR_INVAL;
  }
  file->pos = (uint32_t)pos;
  return pos;
}

int64_t rt_fs_size(int32_t fd) {
  return (file_of(fd) == NULL) ? RT_ERR_FILEDES : (int64_t)pak_size(port_pak_image());
}

int32_t rt_fs_stat(int32_t fd, rt_fs_stat_t *stat) {
  if (file_of(fd) == NULL) {
    return RT_ERR_FILEDES;
  }
  memset(stat, 0, sizeof(*stat));
  stat->attr = RT_ATTR_FILE | RT_ATTR_RD;
  return 0;
}

int32_t rt_fs_remove(const char *path) {
  (void)path;
  return RT_ERR_READONLY;
}

int32_t rt_fs_rename(const char *oldpath, const char *newpath) {
  (void)oldpath;
  (void)newpath;
  return RT_ERR_READONLY;
}
