// SPDX-License-Identifier: GPL-2.0-or-later
// sce::Json (libSceJson2) values in guest memory, independent of the guest CPU context.
//
// The guest objects have the firmware's sizes: Value is 0x20 bytes; String, Array and Object are
// 8 bytes, each the guest address of its representation. Every representation and every Value
// the guest can hold a reference to (array elements, object members) is allocated from a
// Json2Heap, which in the game is the MemAllocator passed to Initializer::initialize.
//
//   Value  @0 u32 type, @8 u64 payload (bool, integer, the bits of a double, or the String,
//          Array or Object slot: the payload of a string Value is a String object), @0x10 unused
//   String rep: u64 length, then the bytes and a NUL
//   Array rep:  u64 count, u64 capacity, u64 address of capacity Value addresses
//   Object rep: u64 count, u64 capacity, u64 address of capacity {u64 key String rep, u64 Value
//               address} entries, sorted by key bytes
#ifndef PS4_JSON2_CORE_H
#define PS4_JSON2_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// sce::Json::ValueType
enum {
  JSON2_NULL = 0,
  JSON2_BOOLEAN = 1,
  JSON2_INTEGER = 2,
  JSON2_UINTEGER = 3,
  JSON2_REAL = 4,
  JSON2_STRING = 5,
  JSON2_ARRAY = 6,
  JSON2_OBJECT = 7,
};

#define JSON2_VALUE_SIZE 0x20
#define JSON2_PAYLOAD 8 // offset of the payload (and of a string/array/object Value's slot)

typedef struct Json2Heap {
  uint8_t *base; // host address of guest address 0
  // Returns the guest address of size bytes; it does not return when the memory is exhausted.
  uint64_t (*alloc)(struct Json2Heap *h, uint64_t size);
  void (*release)(struct Json2Heap *h, uint64_t addr);
} Json2Heap;

static inline uint64_t json2_u64(const Json2Heap *h, uint64_t a) {
  return *(const uint64_t *)(h->base + a);
}
static inline uint32_t json2_type(const Json2Heap *h, uint64_t v) {
  return *(const uint32_t *)(h->base + v);
}

// Value
void json2_value_init(Json2Heap *h, uint64_t v, uint32_t type); // the empty value of type
void json2_value_init_bool(Json2Heap *h, uint64_t v, int b);
void json2_value_init_int(Json2Heap *h, uint64_t v, int64_t i);
void json2_value_init_uint(Json2Heap *h, uint64_t v, uint64_t u);
void json2_value_init_real(Json2Heap *h, uint64_t v, double r);
void json2_value_init_chars(Json2Heap *h, uint64_t v, const char *s, size_t n);
void json2_value_init_string(Json2Heap *h, uint64_t v, uint64_t str);  // copies a String
void json2_value_init_array(Json2Heap *h, uint64_t v, uint64_t arr);   // copies an Array
void json2_value_init_object(Json2Heap *h, uint64_t v, uint64_t obj);  // copies an Object
void json2_value_init_copy(Json2Heap *h, uint64_t v, uint64_t src);
void json2_value_destroy(Json2Heap *h, uint64_t v);
// dst = src; src may be dst or a value inside dst.
void json2_value_assign(Json2Heap *h, uint64_t dst, uint64_t src);
int64_t json2_value_int(const Json2Heap *h, uint64_t v);
double json2_value_real(const Json2Heap *h, uint64_t v);

// String
void json2_string_init(Json2Heap *h, uint64_t s, const char *chars, size_t n);
void json2_string_destroy(Json2Heap *h, uint64_t s);
void json2_string_assign(Json2Heap *h, uint64_t dst, uint64_t src);
uint64_t json2_string_length(const Json2Heap *h, uint64_t s);
uint64_t json2_string_cstr(const Json2Heap *h, uint64_t s); // guest address of the bytes

// Array
void json2_array_init(Json2Heap *h, uint64_t a);
void json2_array_destroy(Json2Heap *h, uint64_t a);
void json2_array_push_back(Json2Heap *h, uint64_t a, uint64_t value);
uint64_t json2_array_size(const Json2Heap *h, uint64_t a);
uint64_t json2_array_at(const Json2Heap *h, uint64_t a, uint64_t i); // 0 when out of range

// Object
void json2_object_init(Json2Heap *h, uint64_t o);
void json2_object_init_copy(Json2Heap *h, uint64_t o, uint64_t src);
void json2_object_destroy(Json2Heap *h, uint64_t o);
void json2_object_assign(Json2Heap *h, uint64_t dst, uint64_t src);
void json2_object_clear(Json2Heap *h, uint64_t o);
// The member named key, inserted as null when missing.
uint64_t json2_object_member(Json2Heap *h, uint64_t o, const char *key, size_t n);
uint64_t json2_object_find(const Json2Heap *h, uint64_t o, const char *key, size_t n); // or 0

// Writes v as JSON text into the String out, as the firmware's serializer does.
void json2_serialize(Json2Heap *h, uint64_t v, uint64_t out);

#ifdef __cplusplus
}
#endif

#endif // PS4_JSON2_CORE_H
