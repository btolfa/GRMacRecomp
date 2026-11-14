// SPDX-License-Identifier: GPL-2.0-or-later
// libSceJson2 (sce::Json) host shims over ps4_json2_core.c.
//
// Like the firmware library, every byte of JSON storage comes from the sce::Json::MemAllocator
// the game hands to Initializer::initialize, so the values the game references live in guest
// memory. The allocator is a C++ object; its virtual functions are called in the guest:
//   vtable +0x00 complete destructor, +0x08 deleting destructor,
//   +0x10 void *allocate(size_t size, void *user_data),
//   +0x18 void deallocate(void *ptr, void *user_data),
//   +0x20 void notifyError(int32_t error, size_t size, void *user_data)
// (Gravity Rush 2's allocator vtable at 0x16f1a48 of its v1.11 eboot has this layout).
// InitParameter is {MemAllocator *allocator; void *user_data; size_t file_buffer_size} (the same
// game fills one at 0x44b2d9); InitParameter2 keeps its first three fields in the same order.
#include "ps4_json2.h"
#include "ps4_json2_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  int ready;
  uint64_t allocator, user_data;
  // Referenced by const getters on a value of another type; allocated when first needed.
  uint64_t null_value, empty_string, empty_array, empty_object;
} g_json;

typedef struct GuestHeap {
  Json2Heap h;
  GuestContext *ctx;
} GuestHeap;

static uint64_t *gp64(GuestContext *ctx, uint64_t a) { return (uint64_t *)(ctx->mem_base + a); }

static void json_fatal(const char *what) {
  fprintf(stderr, "[json2] FATAL: %s\n", what);
  fflush(stderr);
  abort();
}

// Calls the allocator's virtual function at vtable offset slot with (arg, user_data).
static uint64_t call_allocator(GuestContext *ctx, uint64_t slot, uint64_t arg) {
  uint64_t vtable = *gp64(ctx, g_json.allocator);
  GuestContext sub = *ctx;
  sub.rdi = g_json.allocator;
  sub.rsi = arg;
  sub.rdx = g_json.user_data;
  recomp_call_guest(&sub, *gp64(ctx, vtable + slot));
  return sub.rax;
}

static uint64_t guest_alloc(Json2Heap *h, uint64_t size) {
  GuestHeap *gh = (GuestHeap *)h;
  if (!g_json.ready)
    json_fatal("sce::Json used before Initializer::initialize");
  uint64_t a = call_allocator(gh->ctx, 0x10, size);
  if (!a) {
    // The firmware reports this through notifyError with an error code this runtime does not
    // know yet; stop with a report instead of continuing without the memory.
    fprintf(stderr, "[json2] FATAL: MemAllocator::allocate(%llu) returned NULL\n", (unsigned long long)size);
    abort();
  }
  return a;
}

static void guest_release(Json2Heap *h, uint64_t a) {
  GuestHeap *gh = (GuestHeap *)h;
  call_allocator(gh->ctx, 0x18, a);
}

static GuestHeap heap(GuestContext *ctx) {
  GuestHeap gh = {{ctx->mem_base, guest_alloc, guest_release}, ctx};
  return gh;
}

static uint64_t static_null(GuestHeap *gh) {
  if (!g_json.null_value) {
    g_json.null_value = gh->h.alloc(&gh->h, JSON2_VALUE_SIZE);
    json2_value_init(&gh->h, g_json.null_value, JSON2_NULL);
  }
  return g_json.null_value;
}

static uint64_t static_empty(GuestHeap *gh, uint64_t *slot, uint32_t type) {
  if (!*slot) {
    *slot = gh->h.alloc(&gh->h, 8);
    if (type == JSON2_STRING)
      json2_string_init(&gh->h, *slot, "", 0);
    else if (type == JSON2_ARRAY)
      json2_array_init(&gh->h, *slot);
    else
      json2_object_init(&gh->h, *slot);
  }
  return *slot;
}

static const char *host_chars(GuestContext *ctx, uint64_t a) {
  return a ? (const char *)(ctx->mem_base + a) : "";
}

#define HEAP GuestHeap gh = heap(ctx); Json2Heap *h = &gh.h
#define RET(v)                                                                                     \
  do {                                                                                             \
    ctx->rax = (uint64_t)(v);                                                                      \
    SHIM_RETURN();                                                                                 \
  } while (0)

// ---- MemAllocator, InitParameter, Initializer

// The game's allocator class writes its own vtable after the base constructor returns.
void shim__ZN3sce4Json12MemAllocatorC2Ev(GuestContext *ctx) { RET(ctx->rdi); }
void shim__ZN3sce4Json12MemAllocatorD2Ev(GuestContext *ctx) { RET(0); }

// The base class's notifyError, which a game's allocator inherits unless it overrides it.
void shim__ZN3sce4Json12MemAllocator11notifyErrorEimPv(GuestContext *ctx) {
  fprintf(stderr, "[json2] MemAllocator::notifyError(error=0x%x, size=%llu)\n", (uint32_t)ctx->rsi,
          (unsigned long long)ctx->rdx);
  RET(0);
}

void shim__ZN3sce4Json14InitParameter2C1Ev(GuestContext *ctx) {
  memset(ctx->mem_base + ctx->rdi, 0, 24);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv(GuestContext *ctx) {
  *gp64(ctx, ctx->rdi) = ctx->rsi;
  *gp64(ctx, ctx->rdi + 8) = ctx->rdx;
  RET(0);
}

void shim__ZN3sce4Json14InitParameter217setFileBufferSizeEm(GuestContext *ctx) {
  *gp64(ctx, ctx->rdi + 16) = ctx->rsi;
  RET(0);
}

// The library state is global; the Initializer object holds nothing.
void shim__ZN3sce4Json11InitializerC1Ev(GuestContext *ctx) { RET(ctx->rdi); }
void shim__ZN3sce4Json11InitializerD1Ev(GuestContext *ctx) { RET(0); }

static void initialize(GuestContext *ctx, uint64_t param) {
  if (g_json.ready)
    json_fatal("Initializer::initialize called again without terminate (its error code is not known)");
  if (!param || !*gp64(ctx, param))
    json_fatal("Initializer::initialize without an allocator (its error code is not known)");
  g_json.allocator = *gp64(ctx, param);
  g_json.user_data = *gp64(ctx, param + 8);
  g_json.ready = 1;
}

void shim__ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE(GuestContext *ctx) {
  initialize(ctx, ctx->rsi);
  RET(0);
}

void shim__ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E(GuestContext *ctx) {
  initialize(ctx, ctx->rsi);
  RET(0);
}

void shim__ZN3sce4Json11Initializer9terminateEv(GuestContext *ctx) {
  if (!g_json.ready)
    json_fatal("Initializer::terminate without initialize (its error code is not known)");
  HEAP;
  if (g_json.null_value) {
    json2_value_destroy(h, g_json.null_value);
    h->release(h, g_json.null_value);
  }
  if (g_json.empty_string) {
    json2_string_destroy(h, g_json.empty_string);
    h->release(h, g_json.empty_string);
  }
  if (g_json.empty_array) {
    json2_array_destroy(h, g_json.empty_array);
    h->release(h, g_json.empty_array);
  }
  if (g_json.empty_object) {
    json2_object_destroy(h, g_json.empty_object);
    h->release(h, g_json.empty_object);
  }
  memset(&g_json, 0, sizeof(g_json));
  RET(0);
}

// ---- String

void shim__ZN3sce4Json6StringC1Ev(GuestContext *ctx) {
  HEAP;
  json2_string_init(h, ctx->rdi, "", 0);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json6StringC1EPKc(GuestContext *ctx) {
  HEAP;
  const char *s = host_chars(ctx, ctx->rsi);
  json2_string_init(h, ctx->rdi, s, strlen(s));
  RET(ctx->rdi);
}

void shim__ZN3sce4Json6StringD1Ev(GuestContext *ctx) {
  HEAP;
  json2_string_destroy(h, ctx->rdi);
  RET(0);
}

void shim__ZNK3sce4Json6String5c_strEv(GuestContext *ctx) {
  HEAP;
  RET(json2_string_cstr(h, ctx->rdi));
}

void shim__ZNK3sce4Json6String6lengthEv(GuestContext *ctx) {
  HEAP;
  RET(json2_string_length(h, ctx->rdi));
}

void shim__ZN3sce4Json6StringaSERKS1_(GuestContext *ctx) {
  HEAP;
  if (ctx->rdi != ctx->rsi)
    json2_string_assign(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

// ---- Value

void shim__ZN3sce4Json5ValueC1Ev(GuestContext *ctx) {
  HEAP;
  json2_value_init(h, ctx->rdi, JSON2_NULL);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1Eb(GuestContext *ctx) {
  HEAP;
  json2_value_init_bool(h, ctx->rdi, (uint8_t)ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1El(GuestContext *ctx) {
  HEAP;
  json2_value_init_int(h, ctx->rdi, (int64_t)ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1Em(GuestContext *ctx) {
  HEAP;
  json2_value_init_uint(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1Ed(GuestContext *ctx) {
  HEAP;
  json2_value_init_real(h, ctx->rdi, ctx->xmm[0].f64[0]);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1EPKc(GuestContext *ctx) {
  HEAP;
  const char *s = host_chars(ctx, ctx->rsi);
  json2_value_init_chars(h, ctx->rdi, s, strlen(s));
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1ERKNS0_6StringE(GuestContext *ctx) {
  HEAP;
  json2_value_init_string(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1ERKNS0_5ArrayE(GuestContext *ctx) {
  HEAP;
  json2_value_init_array(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueC1ERKNS0_6ObjectE(GuestContext *ctx) {
  HEAP;
  json2_value_init_object(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ValueD1Ev(GuestContext *ctx) {
  HEAP;
  json2_value_destroy(h, ctx->rdi);
  RET(0);
}

void shim__ZN3sce4Json5ValueaSERKS1_(GuestContext *ctx) {
  HEAP;
  json2_value_assign(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5Value3setERKS1_(GuestContext *ctx) {
  HEAP;
  json2_value_assign(h, ctx->rdi, ctx->rsi);
  RET(0);
}

void shim__ZN3sce4Json5Value3setENS0_9ValueTypeE(GuestContext *ctx) {
  HEAP;
  uint32_t type = (uint32_t)ctx->rsi;
  if (type > JSON2_OBJECT)
    json_fatal("Value::set with an unknown ValueType");
  json2_value_destroy(h, ctx->rdi);
  json2_value_init(h, ctx->rdi, type);
  RET(0);
}

void shim__ZN3sce4Json5Value5clearEv(GuestContext *ctx) {
  HEAP;
  json2_value_destroy(h, ctx->rdi); // a cleared value is null
  RET(0);
}

void shim__ZNK3sce4Json5Value7getTypeEv(GuestContext *ctx) {
  HEAP;
  RET(json2_type(h, ctx->rdi));
}

void shim__ZNK3sce4Json5Value10getIntegerEv(GuestContext *ctx) {
  HEAP;
  RET(json2_value_int(h, ctx->rdi));
}

void shim__ZNK3sce4Json5Value7getRealEv(GuestContext *ctx) {
  HEAP;
  ctx->xmm[0].f64[0] = json2_value_real(h, ctx->rdi);
  SHIM_RETURN();
}

void shim__ZNK3sce4Json5Value10getBooleanEv(GuestContext *ctx) {
  HEAP;
  uint64_t v = ctx->rdi;
  RET(json2_type(h, v) == JSON2_BOOLEAN ? json2_u64(h, v + JSON2_PAYLOAD) : 0);
}

// Getters of a string, array or object: the value's own slot, or an empty one when the value
// has another type.
static uint64_t slot_of(GuestHeap *gh, uint64_t v, uint32_t type, uint64_t *empty) {
  if (json2_type(&gh->h, v) == type)
    return v + JSON2_PAYLOAD;
  return static_empty(gh, empty, type);
}

void shim__ZNK3sce4Json5Value9getStringEv(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  RET(slot_of(&gh, ctx->rdi, JSON2_STRING, &g_json.empty_string));
}

void shim__ZNK3sce4Json5Value9getObjectEv(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  RET(slot_of(&gh, ctx->rdi, JSON2_OBJECT, &g_json.empty_object));
}

void shim__ZNK3sce4Json5Value8getArrayEv(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  RET(slot_of(&gh, ctx->rdi, JSON2_ARRAY, &g_json.empty_array));
}

// refer*: the value becomes an empty object/array unless it is one, and its slot is returned.
static uint64_t refer(Json2Heap *h, uint64_t v, uint32_t type) {
  if (json2_type(h, v) != type) {
    json2_value_destroy(h, v);
    json2_value_init(h, v, type);
  }
  return v + JSON2_PAYLOAD;
}

void shim__ZN3sce4Json5Value11referObjectEv(GuestContext *ctx) {
  HEAP;
  RET(refer(h, ctx->rdi, JSON2_OBJECT));
}

void shim__ZN3sce4Json5Value10referArrayEv(GuestContext *ctx) {
  HEAP;
  RET(refer(h, ctx->rdi, JSON2_ARRAY));
}

void shim__ZNK3sce4Json5ValueixEPKc(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  uint64_t v = ctx->rdi, m = 0;
  if (json2_type(&gh.h, v) == JSON2_OBJECT) {
    const char *key = host_chars(ctx, ctx->rsi);
    m = json2_object_find(&gh.h, v + JSON2_PAYLOAD, key, strlen(key));
  }
  RET(m ? m : static_null(&gh));
}

void shim__ZNK3sce4Json5ValueixEm(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  uint64_t v = ctx->rdi, e = 0;
  if (json2_type(&gh.h, v) == JSON2_ARRAY)
    e = json2_array_at(&gh.h, v + JSON2_PAYLOAD, ctx->rsi);
  RET(e ? e : static_null(&gh));
}

void shim__ZN3sce4Json5Value9serializeERNS0_6StringE(GuestContext *ctx) {
  HEAP;
  json2_serialize(h, ctx->rdi, ctx->rsi);
  RET(0);
}

// ---- Array

void shim__ZN3sce4Json5ArrayC1Ev(GuestContext *ctx) {
  HEAP;
  json2_array_init(h, ctx->rdi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json5ArrayD1Ev(GuestContext *ctx) {
  HEAP;
  json2_array_destroy(h, ctx->rdi);
  RET(0);
}

void shim__ZNK3sce4Json5Array4sizeEv(GuestContext *ctx) {
  HEAP;
  RET(json2_array_size(h, ctx->rdi));
}

void shim__ZNK3sce4Json5Array4backEv(GuestContext *ctx) {
  GuestHeap gh = heap(ctx);
  uint64_t n = json2_array_size(&gh.h, ctx->rdi);
  RET(n ? json2_array_at(&gh.h, ctx->rdi, n - 1) : static_null(&gh));
}

void shim__ZN3sce4Json5Array9push_backERKNS0_5ValueE(GuestContext *ctx) {
  HEAP;
  json2_array_push_back(h, ctx->rdi, ctx->rsi);
  RET(0);
}

// ---- Object

void shim__ZN3sce4Json6ObjectC1Ev(GuestContext *ctx) {
  HEAP;
  json2_object_init(h, ctx->rdi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json6ObjectC1ERKS1_(GuestContext *ctx) {
  HEAP;
  json2_object_init_copy(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json6ObjectD1Ev(GuestContext *ctx) {
  HEAP;
  json2_object_destroy(h, ctx->rdi);
  RET(0);
}

void shim__ZN3sce4Json6Object5clearEv(GuestContext *ctx) {
  HEAP;
  json2_object_clear(h, ctx->rdi);
  RET(0);
}

void shim__ZN3sce4Json6ObjectaSERKS1_(GuestContext *ctx) {
  HEAP;
  json2_object_assign(h, ctx->rdi, ctx->rsi);
  RET(ctx->rdi);
}

void shim__ZN3sce4Json6ObjectixERKNS0_6StringE(GuestContext *ctx) {
  HEAP;
  uint64_t key = ctx->rsi;
  RET(json2_object_member(h, ctx->rdi, host_chars(ctx, json2_string_cstr(h, key)), json2_string_length(h, key)));
}
