// SPDX-License-Identifier: GPL-2.0-or-later
// libSceContentSearch: queries the system album (the photos and videos a title exported with
// libSceContentExport).
//
// The album is a host folder per title, under the runtime's data root (as savedata and
// download0): $PS4RECOMP_DATA_DIR/album/<TITLE_ID>, or
// ~/Library/Application Support/PS4Recomp/album/<TITLE_ID>. Each content is one entry
// <content id as 16 hex digits>.json there. libSceContentExport, which creates the entries, is not
// implemented yet, so the album of a title holds only what was put there by hand.
//
// A query is a list of conditions; each condition is a list of terms comparing one column with a
// value. What most columns mean is not known yet, so a query over a non-empty album stops with a
// report instead of guessing which entries match. Over an empty album every query has no hits.
#include "recomp_runtime.h"
#include "ps4_sfo.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The term of a condition, as Gravity Rush 2 builds it (0x10cc6b9 in its v1.11 eboot).
typedef struct OrbisContentSearchValue {
  uint32_t size;  // of value: 8
  uint32_t pad;
  uint64_t value;
} OrbisContentSearchValue;

typedef struct OrbisContentSearchTerm {
  uint32_t column;
  uint32_t comparison;
  uint64_t value;  // guest OrbisContentSearchValue *
  uint32_t join;
  uint32_t pad;
} OrbisContentSearchTerm;
_Static_assert(sizeof(OrbisContentSearchTerm) == 0x18, "OrbisContentSearchTerm ABI size");

typedef struct OrbisContentSearchCondition {
  uint64_t terms;  // guest OrbisContentSearchTerm *
  int32_t num_terms;
  uint32_t join;
} OrbisContentSearchCondition;
_Static_assert(sizeof(OrbisContentSearchCondition) == 0x10, "OrbisContentSearchCondition ABI size");

static int g_initialized;

static void content_search_fatal(GuestContext *ctx, const char *what) {
  fprintf(stderr, "\nFATAL: [content-search] %s\n", what);
  recomp_dump_guest_context(ctx);
  fflush(stderr);
  abort();
}

// Writes the album folder of the running title to out. Returns false when there is no data root
// or no title id.
static bool album_dir(char *out, size_t out_size) {
  char title_id[32] = {0};
  Ps4SfoContext sfo = {0};
  if (ps4_sfo_find_and_load(&sfo)) {
    ps4_sfo_get_string(&sfo, "TITLE_ID", title_id, sizeof(title_id));
    ps4_sfo_free(&sfo);
  }
  if (!title_id[0]) return false;
  const char *data_dir = getenv("PS4RECOMP_DATA_DIR");
  if (data_dir && *data_dir) {
    snprintf(out, out_size, "%s/album/%s", data_dir, title_id);
    return true;
  }
  const char *home = getenv("HOME");
  if (!home || !*home) return false;
  snprintf(out, out_size, "%s/Library/Application Support/PS4Recomp/album/%s", home, title_id);
  return true;
}

// Number of entries in the album of the running title (a missing folder is an empty album).
static uint32_t album_count(GuestContext *ctx) {
  char dir[1024];
  if (!album_dir(dir, sizeof(dir))) content_search_fatal(ctx, "no data root or title id for the album");
  DIR *d = opendir(dir);
  if (!d) {
    if (errno == ENOENT) return 0;
    fprintf(stderr, "[content-search] cannot open the album %s: %s\n", dir, strerror(errno));
    content_search_fatal(ctx, "album folder unreadable");
  }
  uint32_t n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    const size_t len = strlen(e->d_name);
    if (len == 16 + 5 && strcmp(e->d_name + 16, ".json") == 0) n++;
  }
  closedir(d);
  return n;
}

// Checks that the conditions are readable guest memory and logs them.
static void check_conditions(GuestContext *ctx, uint64_t conds, uint32_t num_conds) {
  if (num_conds == 0) return;
  if (!conds) content_search_fatal(ctx, "conditions are NULL");
  for (uint32_t c = 0; c < num_conds; c++) {
    const OrbisContentSearchCondition *cond =
        (const OrbisContentSearchCondition *)(ctx->mem_base + conds + c * sizeof(*cond));
    if (cond->num_terms < 0 || (cond->num_terms > 0 && !cond->terms))
      content_search_fatal(ctx, "malformed condition");
    for (int32_t t = 0; t < cond->num_terms; t++) {
      const OrbisContentSearchTerm *term =
          (const OrbisContentSearchTerm *)(ctx->mem_base + cond->terms + t * sizeof(*term));
      uint64_t value = 0;
      if (term->value) {
        const OrbisContentSearchValue *v = (const OrbisContentSearchValue *)(ctx->mem_base + term->value);
        value = v->value;
      }
      fprintf(stderr, "[content-search]   condition %u term %d: column %u comparison %u value 0x%llx join %u\n",
              c, t, term->column, term->comparison, (unsigned long long)value, term->join);
    }
  }
}

// sceContentSearchInit(const OrbisContentSearchInitParam *param)
void shim_sceContentSearchInit(GuestContext *ctx) {
  if (!ctx->rdi) content_search_fatal(ctx, "sceContentSearchInit: param is NULL");
  if (g_initialized) content_search_fatal(ctx, "sceContentSearchInit: already initialized");
  g_initialized = 1;
  ctx->rax = 0;
  SHIM_RETURN();
}

// sceContentSearchTerm(void)
void shim_sceContentSearchTerm(GuestContext *ctx) {
  if (!g_initialized) content_search_fatal(ctx, "sceContentSearchTerm: not initialized");
  g_initialized = 0;
  ctx->rax = 0;
  SHIM_RETURN();
}

// sceContentSearchGetMyApplicationIndex(int32_t *index): the index of the running title in the
// system's content database, the value a query compares application columns with. The album of
// a title here holds only that title's content, so the index is 0.
void shim_sceContentSearchGetMyApplicationIndex(GuestContext *ctx) {
  if (!g_initialized) content_search_fatal(ctx, "sceContentSearchGetMyApplicationIndex: not initialized");
  if (!ctx->rdi) content_search_fatal(ctx, "sceContentSearchGetMyApplicationIndex: index is NULL");
  *(int32_t *)(ctx->mem_base + ctx->rdi) = 0;
  ctx->rax = 0;
  SHIM_RETURN();
}

// sceContentSearchGetNumOfContent(const OrbisContentSearchCondition *conds, uint32_t num_conds,
// uint32_t *num, uint64_t *update_counter): the number of contents matching the conditions, and
// the album's update counter (a game repeats its query when the counter changed between the count
// and the search). The album does not change while the title runs, so the counter stays 0.
void shim_sceContentSearchGetNumOfContent(GuestContext *ctx) {
  const uint64_t conds = ctx->rdi;
  const uint32_t num_conds = (uint32_t)ctx->rsi;
  const uint64_t num_out = ctx->rdx;
  const uint64_t counter_out = ctx->rcx;
  if (!g_initialized) content_search_fatal(ctx, "sceContentSearchGetNumOfContent: not initialized");
  if (!num_out) content_search_fatal(ctx, "sceContentSearchGetNumOfContent: num is NULL");
  fprintf(stderr, "[content-search] GetNumOfContent: %u condition(s)\n", num_conds);
  check_conditions(ctx, conds, num_conds);
  if (album_count(ctx) != 0)
    content_search_fatal(ctx, "the album is not empty: matching its contents against the conditions is not implemented");
  *(uint32_t *)(ctx->mem_base + num_out) = 0;
  if (counter_out) *(uint64_t *)(ctx->mem_base + counter_out) = 0;
  ctx->rax = 0;
  SHIM_RETURN();
}
