// SPDX-License-Identifier: GPL-2.0-or-later
// sce::Json values in guest memory (ps4_json2_core.h).
#include "ps4_json2_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t *p64(const Json2Heap *h, uint64_t a) { return (uint64_t *)(h->base + a); }

static void rel(Json2Heap *h, uint64_t a) {
  if (a)
    h->release(h, a);
}

static uint64_t zalloc(Json2Heap *h, uint64_t size) {
  uint64_t a = h->alloc(h, size);
  memset(h->base + a, 0, size);
  return a;
}

// ---- String representations

static uint64_t str_rep_new(Json2Heap *h, const char *chars, size_t n) {
  uint64_t rep = h->alloc(h, 8 + n + 1);
  *p64(h, rep) = n;
  if (n)
    memcpy(h->base + rep + 8, chars, n);
  h->base[rep + 8 + n] = '\0';
  return rep;
}

static uint64_t str_rep_copy(Json2Heap *h, uint64_t rep) {
  return str_rep_new(h, (const char *)h->base + rep + 8, json2_u64(h, rep));
}

void json2_string_init(Json2Heap *h, uint64_t s, const char *chars, size_t n) {
  *p64(h, s) = str_rep_new(h, chars, n);
}

void json2_string_destroy(Json2Heap *h, uint64_t s) {
  rel(h, json2_u64(h, s));
  *p64(h, s) = 0;
}

void json2_string_assign(Json2Heap *h, uint64_t dst, uint64_t src) {
  uint64_t rep = str_rep_copy(h, json2_u64(h, src));
  rel(h, json2_u64(h, dst));
  *p64(h, dst) = rep;
}

uint64_t json2_string_length(const Json2Heap *h, uint64_t s) {
  return json2_u64(h, json2_u64(h, s));
}

uint64_t json2_string_cstr(const Json2Heap *h, uint64_t s) { return json2_u64(h, s) + 8; }

// ---- Values

static void value_set(Json2Heap *h, uint64_t v, uint32_t type, uint64_t payload) {
  memset(h->base + v, 0, JSON2_VALUE_SIZE);
  *(uint32_t *)(h->base + v) = type;
  *p64(h, v + JSON2_PAYLOAD) = payload;
}

void json2_value_init(Json2Heap *h, uint64_t v, uint32_t type) {
  value_set(h, v, type, 0);
  switch (type) {
  case JSON2_STRING:
    json2_string_init(h, v + JSON2_PAYLOAD, "", 0);
    break;
  case JSON2_ARRAY:
    json2_array_init(h, v + JSON2_PAYLOAD);
    break;
  case JSON2_OBJECT:
    json2_object_init(h, v + JSON2_PAYLOAD);
    break;
  default:
    break;
  }
}

void json2_value_init_bool(Json2Heap *h, uint64_t v, int b) { value_set(h, v, JSON2_BOOLEAN, b != 0); }
void json2_value_init_int(Json2Heap *h, uint64_t v, int64_t i) { value_set(h, v, JSON2_INTEGER, (uint64_t)i); }
void json2_value_init_uint(Json2Heap *h, uint64_t v, uint64_t u) { value_set(h, v, JSON2_UINTEGER, u); }

void json2_value_init_real(Json2Heap *h, uint64_t v, double r) {
  uint64_t bits;
  memcpy(&bits, &r, sizeof(bits));
  value_set(h, v, JSON2_REAL, bits);
}

void json2_value_init_chars(Json2Heap *h, uint64_t v, const char *s, size_t n) {
  value_set(h, v, JSON2_STRING, 0);
  json2_string_init(h, v + JSON2_PAYLOAD, s, n);
}

void json2_value_init_string(Json2Heap *h, uint64_t v, uint64_t str) {
  value_set(h, v, JSON2_STRING, str_rep_copy(h, json2_u64(h, str)));
}

static uint64_t array_rep_copy(Json2Heap *h, uint64_t rep);
static uint64_t object_rep_copy(Json2Heap *h, uint64_t rep);

void json2_value_init_array(Json2Heap *h, uint64_t v, uint64_t arr) {
  value_set(h, v, JSON2_ARRAY, array_rep_copy(h, json2_u64(h, arr)));
}

void json2_value_init_object(Json2Heap *h, uint64_t v, uint64_t obj) {
  value_set(h, v, JSON2_OBJECT, object_rep_copy(h, json2_u64(h, obj)));
}

void json2_value_init_copy(Json2Heap *h, uint64_t v, uint64_t src) {
  uint32_t type = json2_type(h, src);
  uint64_t payload = json2_u64(h, src + JSON2_PAYLOAD);
  switch (type) {
  case JSON2_STRING:
    payload = str_rep_copy(h, payload);
    break;
  case JSON2_ARRAY:
    payload = array_rep_copy(h, payload);
    break;
  case JSON2_OBJECT:
    payload = object_rep_copy(h, payload);
    break;
  default:
    break;
  }
  value_set(h, v, type, payload);
}

void json2_value_destroy(Json2Heap *h, uint64_t v) {
  switch (json2_type(h, v)) {
  case JSON2_STRING:
    json2_string_destroy(h, v + JSON2_PAYLOAD);
    break;
  case JSON2_ARRAY:
    json2_array_destroy(h, v + JSON2_PAYLOAD);
    break;
  case JSON2_OBJECT:
    json2_object_destroy(h, v + JSON2_PAYLOAD);
    break;
  default:
    break;
  }
  value_set(h, v, JSON2_NULL, 0);
}

void json2_value_assign(Json2Heap *h, uint64_t dst, uint64_t src) {
  if (dst == src)
    return;
  // The copy is complete before dst changes: src may live inside dst.
  uint64_t tmp = h->alloc(h, JSON2_VALUE_SIZE);
  json2_value_init_copy(h, tmp, src);
  json2_value_destroy(h, dst);
  memcpy(h->base + dst, h->base + tmp, JSON2_VALUE_SIZE);
  rel(h, tmp);
}

int64_t json2_value_int(const Json2Heap *h, uint64_t v) {
  uint32_t t = json2_type(h, v);
  return (t == JSON2_INTEGER || t == JSON2_UINTEGER) ? (int64_t)json2_u64(h, v + JSON2_PAYLOAD) : 0;
}

double json2_value_real(const Json2Heap *h, uint64_t v) {
  uint64_t bits = json2_u64(h, v + JSON2_PAYLOAD);
  switch (json2_type(h, v)) {
  case JSON2_REAL: {
    double r;
    memcpy(&r, &bits, sizeof(r));
    return r;
  }
  case JSON2_INTEGER:
    return (double)(int64_t)bits;
  case JSON2_UINTEGER:
    return (double)bits;
  default:
    return 0.0;
  }
}

static uint64_t value_new_copy(Json2Heap *h, uint64_t src) {
  uint64_t v = h->alloc(h, JSON2_VALUE_SIZE);
  json2_value_init_copy(h, v, src);
  return v;
}

static void value_free(Json2Heap *h, uint64_t v) {
  json2_value_destroy(h, v);
  rel(h, v);
}

// ---- Arrays: rep = {count, capacity, items}

static uint64_t array_rep_copy(Json2Heap *h, uint64_t rep) {
  uint64_t n = json2_u64(h, rep);
  uint64_t copy = zalloc(h, 24);
  if (n) {
    uint64_t items = h->alloc(h, n * 8);
    uint64_t src = json2_u64(h, rep + 16);
    for (uint64_t i = 0; i < n; i++)
      *p64(h, items + 8 * i) = value_new_copy(h, json2_u64(h, src + 8 * i));
    *p64(h, copy) = n;
    *p64(h, copy + 8) = n;
    *p64(h, copy + 16) = items;
  }
  return copy;
}

void json2_array_init(Json2Heap *h, uint64_t a) { *p64(h, a) = zalloc(h, 24); }

void json2_array_destroy(Json2Heap *h, uint64_t a) {
  uint64_t rep = json2_u64(h, a);
  uint64_t n = json2_u64(h, rep), items = json2_u64(h, rep + 16);
  for (uint64_t i = 0; i < n; i++)
    value_free(h, json2_u64(h, items + 8 * i));
  rel(h, items);
  rel(h, rep);
  *p64(h, a) = 0;
}

void json2_array_push_back(Json2Heap *h, uint64_t a, uint64_t value) {
  uint64_t elem = value_new_copy(h, value); // before growing: value may be an element
  uint64_t rep = json2_u64(h, a);
  uint64_t n = json2_u64(h, rep), cap = json2_u64(h, rep + 8), items = json2_u64(h, rep + 16);
  if (n == cap) {
    uint64_t ncap = cap ? cap * 2 : 4;
    uint64_t nitems = h->alloc(h, ncap * 8);
    if (n)
      memcpy(h->base + nitems, h->base + items, n * 8);
    rel(h, items);
    items = nitems;
    *p64(h, rep + 8) = ncap;
    *p64(h, rep + 16) = items;
  }
  *p64(h, items + 8 * n) = elem;
  *p64(h, rep) = n + 1;
}

uint64_t json2_array_size(const Json2Heap *h, uint64_t a) { return json2_u64(h, json2_u64(h, a)); }

uint64_t json2_array_at(const Json2Heap *h, uint64_t a, uint64_t i) {
  uint64_t rep = json2_u64(h, a);
  if (i >= json2_u64(h, rep))
    return 0;
  return json2_u64(h, json2_u64(h, rep + 16) + 8 * i);
}

// ---- Objects: rep = {count, capacity, entries}, entry = {key String rep, Value address}

static int key_cmp(const Json2Heap *h, uint64_t key_rep, const char *key, size_t n) {
  uint64_t kn = json2_u64(h, key_rep);
  int c = memcmp(h->base + key_rep + 8, key, kn < n ? kn : n);
  if (c)
    return c;
  return kn < n ? -1 : kn > n ? 1 : 0;
}

// Index of key, or of where it belongs (with *found 0).
static uint64_t object_search(const Json2Heap *h, uint64_t rep, const char *key, size_t n, int *found) {
  uint64_t lo = 0, hi = json2_u64(h, rep), entries = json2_u64(h, rep + 16);
  while (lo < hi) {
    uint64_t mid = lo + (hi - lo) / 2;
    int c = key_cmp(h, json2_u64(h, entries + 16 * mid), key, n);
    if (c == 0) {
      *found = 1;
      return mid;
    }
    if (c < 0)
      lo = mid + 1;
    else
      hi = mid;
  }
  *found = 0;
  return lo;
}

static uint64_t object_rep_copy(Json2Heap *h, uint64_t rep) {
  uint64_t n = json2_u64(h, rep);
  uint64_t copy = zalloc(h, 24);
  if (n) {
    uint64_t entries = h->alloc(h, n * 16);
    uint64_t src = json2_u64(h, rep + 16);
    for (uint64_t i = 0; i < n; i++) {
      *p64(h, entries + 16 * i) = str_rep_copy(h, json2_u64(h, src + 16 * i));
      *p64(h, entries + 16 * i + 8) = value_new_copy(h, json2_u64(h, src + 16 * i + 8));
    }
    *p64(h, copy) = n;
    *p64(h, copy + 8) = n;
    *p64(h, copy + 16) = entries;
  }
  return copy;
}

static void object_rep_free(Json2Heap *h, uint64_t rep) {
  uint64_t n = json2_u64(h, rep), entries = json2_u64(h, rep + 16);
  for (uint64_t i = 0; i < n; i++) {
    rel(h, json2_u64(h, entries + 16 * i));
    value_free(h, json2_u64(h, entries + 16 * i + 8));
  }
  rel(h, entries);
  rel(h, rep);
}

void json2_object_init(Json2Heap *h, uint64_t o) { *p64(h, o) = zalloc(h, 24); }

void json2_object_init_copy(Json2Heap *h, uint64_t o, uint64_t src) {
  *p64(h, o) = object_rep_copy(h, json2_u64(h, src));
}

void json2_object_destroy(Json2Heap *h, uint64_t o) {
  object_rep_free(h, json2_u64(h, o));
  *p64(h, o) = 0;
}

void json2_object_assign(Json2Heap *h, uint64_t dst, uint64_t src) {
  if (dst == src)
    return;
  uint64_t rep = object_rep_copy(h, json2_u64(h, src)); // src may live inside dst
  object_rep_free(h, json2_u64(h, dst));
  *p64(h, dst) = rep;
}

void json2_object_clear(Json2Heap *h, uint64_t o) {
  object_rep_free(h, json2_u64(h, o));
  *p64(h, o) = zalloc(h, 24);
}

uint64_t json2_object_find(const Json2Heap *h, uint64_t o, const char *key, size_t n) {
  uint64_t rep = json2_u64(h, o);
  int found;
  uint64_t i = object_search(h, rep, key, n, &found);
  return found ? json2_u64(h, json2_u64(h, rep + 16) + 16 * i + 8) : 0;
}

uint64_t json2_object_member(Json2Heap *h, uint64_t o, const char *key, size_t n) {
  uint64_t rep = json2_u64(h, o);
  int found;
  uint64_t i = object_search(h, rep, key, n, &found);
  if (found)
    return json2_u64(h, json2_u64(h, rep + 16) + 16 * i + 8);
  // key may point into guest memory that the allocations below do not move.
  uint64_t key_rep = str_rep_new(h, key, n);
  uint64_t value = h->alloc(h, JSON2_VALUE_SIZE);
  json2_value_init(h, value, JSON2_NULL);
  uint64_t cnt = json2_u64(h, rep), cap = json2_u64(h, rep + 8), entries = json2_u64(h, rep + 16);
  if (cnt == cap) {
    uint64_t ncap = cap ? cap * 2 : 4;
    uint64_t nentries = h->alloc(h, ncap * 16);
    if (cnt)
      memcpy(h->base + nentries, h->base + entries, cnt * 16);
    rel(h, entries);
    entries = nentries;
    *p64(h, rep + 8) = ncap;
    *p64(h, rep + 16) = entries;
  }
  memmove(h->base + entries + 16 * (i + 1), h->base + entries + 16 * i, (cnt - i) * 16);
  *p64(h, entries + 16 * i) = key_rep;
  *p64(h, entries + 16 * i + 8) = value;
  *p64(h, rep) = cnt + 1;
  return value;
}

// ---- Serialization

typedef struct Buf {
  char *p;
  size_t len, cap;
} Buf;

static void put(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1)
      cap *= 2;
    char *p = (char *)realloc(b->p, cap);
    if (!p) {
      fprintf(stderr, "[json2] FATAL: out of host memory serializing\n");
      abort();
    }
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}

static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

// The escapes of the firmware's serializer (as observed by GR2port, src/core/libraries/json2).
static void put_string(Buf *b, const uint8_t *s, uint64_t n) {
  put(b, "\"", 1);
  for (uint64_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    const char *esc = NULL;
    switch (c) {
    case '\b': esc = "\\b"; break;
    case '\t': esc = "\\t"; break;
    case '\n': esc = "\\n"; break;
    case '\f': esc = "\\f"; break;
    case '\r': esc = "\\r"; break;
    case '"': esc = "\\\""; break;
    case '/': esc = "\\/"; break;
    case '\\': esc = "\\\\"; break;
    default: break;
    }
    if (esc) {
      puts_(b, esc);
    } else if (c < 0x20 || c == 0x7f) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04x", c);
      puts_(b, u);
    } else {
      put(b, (const char *)&s[i], 1);
    }
  }
  put(b, "\"", 1);
}

static void put_str_rep(const Json2Heap *h, Buf *b, uint64_t rep) {
  put_string(b, h->base + rep + 8, json2_u64(h, rep));
}

static void write_value(const Json2Heap *h, Buf *b, uint64_t v) {
  uint64_t payload = json2_u64(h, v + JSON2_PAYLOAD);
  char num[64];
  switch (json2_type(h, v)) {
  case JSON2_BOOLEAN:
    puts_(b, payload ? "true" : "false");
    break;
  case JSON2_INTEGER:
    snprintf(num, sizeof(num), "%" PRId64, (int64_t)payload);
    puts_(b, num);
    break;
  case JSON2_UINTEGER:
    snprintf(num, sizeof(num), "%" PRIu64, payload);
    puts_(b, num);
    break;
  case JSON2_REAL: {
    // Six decimals, dropped when all six are zero.
    snprintf(num, sizeof(num), "%f", json2_value_real(h, v));
    char *point = strchr(num, '.');
    if (point && strspn(point + 1, "0") == strlen(point + 1))
      *point = '\0';
    puts_(b, num);
    break;
  }
  case JSON2_STRING:
    put_str_rep(h, b, payload);
    break;
  case JSON2_ARRAY: {
    uint64_t n = json2_u64(h, payload), items = json2_u64(h, payload + 16);
    put(b, "[", 1);
    for (uint64_t i = 0; i < n; i++) {
      if (i)
        put(b, ",", 1);
      write_value(h, b, json2_u64(h, items + 8 * i));
    }
    put(b, "]", 1);
    break;
  }
  case JSON2_OBJECT: {
    uint64_t n = json2_u64(h, payload), entries = json2_u64(h, payload + 16);
    put(b, "{", 1);
    for (uint64_t i = 0; i < n; i++) {
      if (i)
        put(b, ",", 1);
      put_str_rep(h, b, json2_u64(h, entries + 16 * i));
      put(b, ":", 1);
      write_value(h, b, json2_u64(h, entries + 16 * i + 8));
    }
    put(b, "}", 1);
    break;
  }
  default:
    puts_(b, "null");
    break;
  }
}

void json2_serialize(Json2Heap *h, uint64_t v, uint64_t out) {
  Buf b = {0};
  write_value(h, &b, v);
  uint64_t rep = str_rep_new(h, b.p ? b.p : "", b.len);
  free(b.p);
  rel(h, json2_u64(h, out));
  *p64(h, out) = rep;
}
