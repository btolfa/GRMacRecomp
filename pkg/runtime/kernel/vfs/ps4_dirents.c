// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4_dirents.h"
#include "ps4_vfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Directory enumeration in the guest's FreeBSD dirent format.
// Host getdirentries64 returns Darwin dirent64 records, which the guest cannot
// parse. The directory listing is snapshotted per fd (keyed by dev/ino) and
// served as FreeBSD records:
//   struct dirent { uint32_t d_fileno; uint16_t d_reclen; uint8_t d_type;
//                   uint8_t d_namlen; char d_name[d_namlen + 1]; } (4-aligned)
// AppleDouble "._*" files (created by macOS on exFAT/FAT volumes) are hidden.
typedef struct GuestDirCache {
  int fd;
  dev_t dev;
  ino_t ino;
  int count;
  int pos;
  char **names;
  uint32_t *inos;
  uint8_t *types;
} GuestDirCache;

#define GUEST_DIR_CACHE_SLOTS 64
static GuestDirCache g_dir_cache[GUEST_DIR_CACHE_SLOTS];
static pthread_mutex_t g_dir_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void guest_dir_cache_free(GuestDirCache *c) {
  for (int i = 0; i < c->count; i++)
    free(c->names[i]);
  free(c->names);
  free(c->inos);
  free(c->types);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

void ps4_guest_dir_drop_fd(int fd) {
  pthread_mutex_lock(&g_dir_cache_lock);
  for (int i = 0; i < GUEST_DIR_CACHE_SLOTS; i++) {
    if (g_dir_cache[i].names && g_dir_cache[i].fd == fd)
      guest_dir_cache_free(&g_dir_cache[i]);
  }
  pthread_mutex_unlock(&g_dir_cache_lock);
}

// Appends the entries of d; with skip_present, names already listed are left out.
static void guest_dir_cache_add(GuestDirCache *slot, int *cap, DIR *d, bool skip_present) {
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "._", 2) == 0)
      continue;
    if (strlen(e->d_name) > 255)
      continue;
    if (skip_present) {
      bool present = false;
      for (int i = 0; i < slot->count && !present; i++)
        present = strcmp(slot->names[i], e->d_name) == 0;
      if (present)
        continue;
    }
    if (slot->count == *cap) {
      *cap *= 2;
      slot->names = (char **)realloc(slot->names, *cap * sizeof(char *));
      slot->inos = (uint32_t *)realloc(slot->inos, *cap * sizeof(uint32_t));
      slot->types = (uint8_t *)realloc(slot->types, *cap * sizeof(uint8_t));
    }
    slot->names[slot->count] = strdup(e->d_name);
    slot->inos[slot->count] =
        (uint32_t)e->d_ino ? (uint32_t)e->d_ino : (uint32_t)(slot->count + 1);
    slot->types[slot->count] = e->d_type; // DT_* values match FreeBSD
    slot->count++;
  }
}

static GuestDirCache *guest_dir_cache_get(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0)
    return NULL;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return NULL;
  }
  GuestDirCache *slot = NULL;
  for (int i = 0; i < GUEST_DIR_CACHE_SLOTS; i++) {
    GuestDirCache *c = &g_dir_cache[i];
    if (c->names && c->fd == fd) {
      if (c->dev == st.st_dev && c->ino == st.st_ino)
        return c;
      guest_dir_cache_free(c);
      slot = c;
      break;
    }
  }
  if (!slot) {
    for (int i = 0; i < GUEST_DIR_CACHE_SLOTS && !slot; i++) {
      if (!g_dir_cache[i].names)
        slot = &g_dir_cache[i];
    }
    if (!slot) {
      slot = &g_dir_cache[0];
      guest_dir_cache_free(slot);
    }
  }
  int dfd = dup(fd);
  if (dfd < 0)
    return NULL;
  DIR *d = fdopendir(dfd);
  if (!d) {
    close(dfd);
    return NULL;
  }
  rewinddir(d);
  int cap = 64;
  slot->names = (char **)calloc(cap, sizeof(char *));
  slot->inos = (uint32_t *)calloc(cap, sizeof(uint32_t));
  slot->types = (uint8_t *)calloc(cap, sizeof(uint8_t));
  guest_dir_cache_add(slot, &cap, d, false);
  // A directory of the installed update lists the base application's entries too (/app0 is the
  // update layered over the base per file); names the update has hide the base's.
  char host_dir[PATH_MAX], lower_dir[1024];
  if (fcntl(dfd, F_GETPATH, host_dir) == 0 &&
      ps4_vfs_lower_dir(host_dir, lower_dir, sizeof(lower_dir)) == 0) {
    DIR *lower = opendir(lower_dir);
    if (lower) {
      guest_dir_cache_add(slot, &cap, lower, true);
      closedir(lower);
    }
  }
  closedir(d); // closes dfd
  slot->fd = fd;
  slot->dev = st.st_dev;
  slot->ino = st.st_ino;
  slot->pos = 0;
  return slot;
}

// Fills buf with FreeBSD dirents (ps4_dirents.h). *basep (if given) is the entry index; a
// caller-supplied base restarts enumeration from that index. Returns bytes
// written, 0 at end, or -1 with errno set.
int ps4_guest_getdirentries(int fd, char *buf, size_t nbytes,
                            int64_t *basep) {
  pthread_mutex_lock(&g_dir_cache_lock);
  GuestDirCache *c = guest_dir_cache_get(fd);
  if (!c) {
    pthread_mutex_unlock(&g_dir_cache_lock);
    return -1;
  }
  if (basep && *basep >= 0 && *basep <= c->count)
    c->pos = (int)*basep;
  if (basep && *basep < 0)
    c->pos = 0;
  size_t off = 0;
  while (c->pos < c->count) {
    size_t namlen = strlen(c->names[c->pos]);
    size_t reclen = (8 + namlen + 1 + 3) & ~(size_t)3;
    if (off + reclen > nbytes)
      break;
    char *r = buf + off;
    memset(r, 0, reclen);
    *(uint32_t *)(r + 0) = c->inos[c->pos];
    *(uint16_t *)(r + 4) = (uint16_t)reclen;
    *(uint8_t *)(r + 6) = c->types[c->pos];
    *(uint8_t *)(r + 7) = (uint8_t)namlen;
    memcpy(r + 8, c->names[c->pos], namlen);
    off += reclen;
    c->pos++;
  }
  if (off == 0 && c->pos < c->count) {
    pthread_mutex_unlock(&g_dir_cache_lock);
    errno = EINVAL;
    return -1;
  }
  if (basep)
    *basep = c->pos;
  pthread_mutex_unlock(&g_dir_cache_lock);
  return (int)off;
}

