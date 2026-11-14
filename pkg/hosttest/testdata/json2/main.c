// SPDX-License-Identifier: GPL-2.0-or-later
// Host test of sce::Json values in guest memory (libSceJson2/ps4_json2_core.c) over a fake guest
// heap that records every block, so the cases also prove that nothing leaks.
#include "ps4_json2_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA (1u << 22)
#define MAXBLK 4096
static uint8_t g_mem[ARENA];
static uint64_t g_top = 0x1000; // guest address 0 is never handed out
static uint64_t g_blocks[MAXBLK];
static int g_live;

static uint64_t heap_alloc(Json2Heap *h, uint64_t size) {
  (void)h;
  uint64_t a = (g_top + 15) & ~15ull;
  g_top = a + size + 16; // a gap so that overruns hit the canary below
  memset(g_mem + a + size, 0xcd, 16);
  for (int i = 0; i < MAXBLK; i++)
    if (!g_blocks[i]) {
      g_blocks[i] = a;
      g_live++;
      return a;
    }
  abort();
}

static void heap_release(Json2Heap *h, uint64_t a) {
  (void)h;
  for (int i = 0; i < MAXBLK; i++)
    if (g_blocks[i] == a) {
      g_blocks[i] = 0;
      g_live--;
      return;
    }
  printf("FAIL: release of a block never allocated 0x%llx\n", (unsigned long long)a);
  exit(1);
}

static Json2Heap g_h = {g_mem, heap_alloc, heap_release};
static Json2Heap *H = &g_h;

static void fail(const char *c, const char *msg) {
  printf("FAIL %s: %s\n", c, msg);
  exit(1);
}

// A guest object of size bytes the test owns (the game's stack), with canaries around it.
static uint64_t object(uint64_t size) {
  static uint64_t next = ARENA / 2;
  uint64_t a = next + 16;
  memset(g_mem + a - 16, 0xab, 16);
  memset(g_mem + a + size, 0xab, 16);
  next = a + size + 16;
  return a;
}

static void check_canaries(const char *c, uint64_t a, uint64_t size) {
  for (int i = 0; i < 16; i++)
    if (g_mem[a - 16 + i] != 0xab || g_mem[a + size + i] != 0xab)
      fail(c, "a guest object was written outside its firmware size");
}

static void expect_json(const char *c, uint64_t v, const char *want) {
  uint64_t s = object(8);
  json2_string_init(H, s, "", 0);
  json2_serialize(H, v, s);
  const char *got = (const char *)g_mem + json2_string_cstr(H, s);
  if (strcmp(got, want) != 0 || json2_string_length(H, s) != strlen(want)) {
    printf("FAIL %s: serialized %s, want %s\n", c, got, want);
    exit(1);
  }
  json2_string_destroy(H, s);
}

static uint64_t str(const char *s) {
  uint64_t o = object(8);
  json2_string_init(H, o, s, strlen(s));
  return o;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  const char *c = argv[1];
  if (strcmp(c, "sizes") == 0) {
    // String, Array and Object are 8 bytes and Value 0x20, as the game allocates them.
    uint64_t s = object(8), a = object(8), o = object(8), v = object(JSON2_VALUE_SIZE);
    json2_string_init(H, s, "text", 4);
    json2_array_init(H, a);
    json2_object_init(H, o);
    json2_value_init(H, v, JSON2_OBJECT);
    uint64_t e = object(JSON2_VALUE_SIZE);
    json2_value_init_chars(H, e, "x", 1);
    json2_array_push_back(H, a, e);
    json2_value_assign(H, json2_object_member(H, o, "k", 1), e);
    check_canaries(c, s, 8);
    check_canaries(c, a, 8);
    check_canaries(c, o, 8);
    check_canaries(c, v, JSON2_VALUE_SIZE);
    json2_string_destroy(H, s);
    json2_array_destroy(H, a);
    json2_object_destroy(H, o);
    json2_value_destroy(H, v);
    json2_value_destroy(H, e);
  } else if (strcmp(c, "serialize") == 0) {
    // A telemetry-like report: members come out sorted by key.
    uint64_t root = object(JSON2_VALUE_SIZE), o = object(8), a = object(8), e = object(JSON2_VALUE_SIZE);
    json2_object_init(H, o);
    json2_value_init_int(H, json2_object_member(H, o, "int", 3), -42);
    json2_value_init_uint(H, json2_object_member(H, o, "uint", 4), 18446744073709551615ull);
    json2_value_init_real(H, json2_object_member(H, o, "real", 4), 1.5);
    json2_value_init_real(H, json2_object_member(H, o, "whole", 5), 2.0);
    json2_value_init_bool(H, json2_object_member(H, o, "bool", 4), 1);
    json2_value_init_chars(H, json2_object_member(H, o, "str", 3), "a\"b/\\\n\x01", 7);
    json2_array_init(H, a);
    json2_value_init_int(H, e, 1);
    json2_array_push_back(H, a, e);
    json2_value_destroy(H, e);
    json2_value_init(H, e, JSON2_NULL);
    json2_array_push_back(H, a, e);
    json2_value_init_array(H, json2_object_member(H, o, "arr", 3), a);
    json2_value_init_object(H, root, o);
    expect_json(c, root,
                "{\"arr\":[1,null],\"bool\":true,\"int\":-42,\"real\":1.500000,"
                "\"str\":\"a\\\"b\\/\\\\\\n\\u0001\",\"uint\":18446744073709551615,\"whole\":2}");
    json2_value_destroy(H, root);
    json2_value_destroy(H, e);
    json2_array_destroy(H, a);
    json2_object_destroy(H, o);
  } else if (strcmp(c, "deep-copy") == 0) {
    // A Value built from an Array owns its own copy.
    uint64_t a = object(8), e = object(JSON2_VALUE_SIZE), v = object(JSON2_VALUE_SIZE);
    json2_array_init(H, a);
    json2_value_init_chars(H, e, "one", 3);
    json2_array_push_back(H, a, e);
    json2_value_init_array(H, v, a);
    json2_array_destroy(H, a);
    json2_value_destroy(H, e);
    expect_json(c, v, "[\"one\"]");
    json2_value_destroy(H, v);
  } else if (strcmp(c, "assign-from-child") == 0) {
    // v = v["k"] replaces v by a member of itself.
    uint64_t v = object(JSON2_VALUE_SIZE);
    json2_value_init(H, v, JSON2_OBJECT);
    uint64_t k = json2_object_member(H, v + JSON2_PAYLOAD, "k", 1);
    json2_value_init(H, k, JSON2_ARRAY);
    uint64_t e = object(JSON2_VALUE_SIZE);
    json2_value_init_uint(H, e, 7);
    json2_array_push_back(H, k + JSON2_PAYLOAD, e);
    json2_value_destroy(H, e);
    json2_value_assign(H, v, k);
    expect_json(c, v, "[7]");
    json2_value_destroy(H, v);
  } else if (strcmp(c, "object-ops") == 0) {
    uint64_t o = object(8), p = object(8), key = str("b");
    json2_object_init(H, o);
    json2_value_init_int(H, json2_object_member(H, o, "b", 1), 2);
    json2_value_init_int(H, json2_object_member(H, o, "a", 1), 1);
    if (json2_object_member(H, o, "b", 1) != json2_object_find(H, o, "b", 1))
      fail(c, "member and find disagree");
    if (json2_object_find(H, o, "zz", 2) != 0)
      fail(c, "find invented a member");
    json2_object_init_copy(H, p, o);
    json2_object_clear(H, o);
    if (json2_object_find(H, o, "a", 1) != 0)
      fail(c, "clear left a member");
    json2_object_assign(H, o, p);
    if (json2_value_int(H, json2_object_find(H, o, "a", 1)) != 1)
      fail(c, "assign lost a member");
    json2_string_destroy(H, key);
    json2_object_destroy(H, o);
    json2_object_destroy(H, p);
  } else if (strcmp(c, "string-ops") == 0) {
    uint64_t s = str("hello"), t = str("");
    json2_string_assign(H, t, s);
    json2_string_destroy(H, s);
    if (strcmp((const char *)g_mem + json2_string_cstr(H, t), "hello") != 0 || json2_string_length(H, t) != 5)
      fail(c, "assigned string");
    uint64_t v = object(JSON2_VALUE_SIZE);
    json2_value_init_string(H, v, t);
    json2_string_destroy(H, t);
    expect_json(c, v, "\"hello\"");
    json2_value_destroy(H, v);
  } else {
    return 2;
  }
  if (g_live != 0) {
    printf("FAIL %s: %d guest blocks leaked\n", c, g_live);
    return 1;
  }
  printf("PASS %s\n", c);
  return 0;
}
