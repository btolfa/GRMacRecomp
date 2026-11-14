// SPDX-License-Identifier: GPL-2.0-or-later
// Host test of /app0 with an installed update (ps4_vfs.c, ps4_dirents.c): the update is layered
// over the base application per file, and a directory lists the entries of both.
#include "ps4_dirents.h"
#include "ps4_vfs.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_base[1024], g_patch[1024];

static void fail(const char *c, const char *msg) {
  printf("FAIL %s: %s\n", c, msg);
  exit(1);
}

static void put(const char *root, const char *rel, const char *text) {
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", root, rel);
  FILE *f = fopen(p, "w");
  if (!f) {
    perror(p);
    exit(1);
  }
  fputs(text, f);
  fclose(f);
}

static void mkdirs(const char *root, const char *rel) {
  char p[2048];
  snprintf(p, sizeof(p), "%s/%s", root, rel);
  mkdir(p, 0755);
}

// Makes base/ and patch/ under a fresh temporary directory; both get dir/ with shared.txt.
static void make_roots(void) {
  char tmpl[] = "/tmp/vfs_overlay_XXXXXX";
  char *t = mkdtemp(tmpl);
  if (!t) {
    perror("mkdtemp");
    exit(1);
  }
  char real[1024];
  realpath(t, real);
  snprintf(g_base, sizeof(g_base), "%s/base", real);
  snprintf(g_patch, sizeof(g_patch), "%s/patch", real);
  mkdir(g_base, 0755);
  mkdir(g_patch, 0755);
  mkdirs(g_base, "dir");
  mkdirs(g_patch, "dir");
  mkdirs(g_base, "base_only_dir");
  put(g_base, "eboot.bin", "base");
  put(g_patch, "eboot.bin", "patch");
  put(g_base, "base_only.txt", "base");
  put(g_patch, "patch_only.txt", "patch");
  put(g_base, "dir/shared.txt", "base");
  put(g_patch, "dir/shared.txt", "patch");
  put(g_base, "dir/from_base.txt", "base");
  put(g_patch, "dir/from_patch.txt", "patch");
}

// Reads the guest file through the VFS and compares its text.
static void expect_text(const char *c, const char *guest, const char *want) {
  int fd = ps4_vfs_open(guest, O_RDONLY, 0);
  if (fd < 0)
    fail(c, guest);
  char buf[64] = {0};
  ssize_t n = ps4_vfs_read(fd, buf, sizeof(buf) - 1);
  ps4_vfs_close(fd);
  if (n < 0 || strcmp(buf, want) != 0) {
    printf("FAIL %s: %s read '%s', want '%s'\n", c, guest, buf, want);
    exit(1);
  }
}

// Lists the guest directory through getdirentries and returns a "|"-joined sorted-by-arrival
// list of names, without "." and "..".
static void list_dir(const char *c, const char *guest, char *out, size_t out_sz) {
  int fd = ps4_vfs_open(guest, O_RDONLY | O_DIRECTORY, 0);
  if (fd < 0)
    fail(c, guest);
  out[0] = '\0';
  char buf[4096];
  int n;
  while ((n = ps4_guest_getdirentries(fd, buf, sizeof(buf), NULL)) > 0) {
    for (int off = 0; off < n;) {
      uint16_t reclen = *(uint16_t *)(buf + off + 4);
      const char *name = buf + off + 8;
      if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
        strlcat(out, name, out_sz);
        strlcat(out, "|", out_sz);
      }
      off += reclen;
    }
  }
  if (n < 0)
    fail(c, "getdirentries");
  ps4_guest_dir_drop_fd(fd);
  ps4_vfs_close(fd);
}

static int count_of(const char *list, const char *name) {
  char key[256];
  snprintf(key, sizeof(key), "%s|", name);
  int k = 0;
  for (const char *p = list; (p = strstr(p, key)) != NULL; p += strlen(key))
    if (p == list || p[-1] == '|')
      k++;
  return k;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const char *c = argv[1];
  make_roots();
  unsetenv("PS4_APP_DIR");
  unsetenv("PS4_PATCH_DIR");
  setenv("PS4_APP_DIR", g_base, 1);

  if (strcmp(c, "update-first") == 0) {
    ps4_vfs_init(NULL, g_patch);
    expect_text(c, "/app0/eboot.bin", "patch");
    expect_text(c, "/app0/dir/shared.txt", "patch");
    expect_text(c, "/app0/patch_only.txt", "patch");
    expect_text(c, "/app0/base_only.txt", "base");
    expect_text(c, "/app0/dir/from_base.txt", "base");
    struct stat st;
    if (ps4_vfs_stat("/app0/base_only_dir", &st) != 0 || !S_ISDIR(st.st_mode))
      fail(c, "base-only directory is not visible");
  } else if (strcmp(c, "env-overrides-hint") == 0) {
    setenv("PS4_PATCH_DIR", g_patch, 1);
    ps4_vfs_init(NULL, "/nonexistent/hint");
    expect_text(c, "/app0/eboot.bin", "patch");
    if (strcmp(ps4_vfs_get_patch_root(), g_patch) != 0)
      fail(c, "patch root is not PS4_PATCH_DIR");
  } else if (strcmp(c, "no-update") == 0) {
    ps4_vfs_init(NULL, NULL);
    if (ps4_vfs_get_patch_root() != NULL)
      fail(c, "patch root set without an update");
    expect_text(c, "/app0/eboot.bin", "base");
    char host[1024];
    if (ps4_vfs_resolve("/app0/patch_only.txt", host, sizeof(host)) == 0 && access(host, F_OK) == 0)
      fail(c, "update file visible without an update");
  } else if (strcmp(c, "lower-dir") == 0) {
    ps4_vfs_init(NULL, g_patch);
    char in[2048], out[1024], want[2048];
    snprintf(in, sizeof(in), "%s/dir", g_patch);
    snprintf(want, sizeof(want), "%s/dir", g_base);
    if (ps4_vfs_lower_dir(in, out, sizeof(out)) != 0 || strcmp(out, want) != 0)
      fail(c, "update dir has no base counterpart");
    snprintf(in, sizeof(in), "%s/dir", g_base);
    if (ps4_vfs_lower_dir(in, out, sizeof(out)) == 0)
      fail(c, "a base dir has a lower dir");
    snprintf(in, sizeof(in), "%s/patch_only.txt", g_patch);
    if (ps4_vfs_lower_dir(in, out, sizeof(out)) == 0)
      fail(c, "a file has a lower dir");
  } else if (strcmp(c, "union-listing") == 0) {
    ps4_vfs_init(NULL, g_patch);
    char list[4096];
    list_dir(c, "/app0/dir", list, sizeof(list));
    if (count_of(list, "shared.txt") != 1 || count_of(list, "from_base.txt") != 1 ||
        count_of(list, "from_patch.txt") != 1) {
      printf("FAIL %s: /app0/dir lists '%s'\n", c, list);
      return 1;
    }
    list_dir(c, "/app0", list, sizeof(list));
    if (count_of(list, "eboot.bin") != 1 || count_of(list, "base_only.txt") != 1 ||
        count_of(list, "patch_only.txt") != 1 || count_of(list, "base_only_dir") != 1 ||
        count_of(list, "dir") != 1) {
      printf("FAIL %s: /app0 lists '%s'\n", c, list);
      return 1;
    }
  } else {
    return 2;
  }
  printf("PASS %s\n", c);
  return 0;
}
