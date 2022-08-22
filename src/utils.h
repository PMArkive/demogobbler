#pragma once

#include "dynamic_array.h"
#include <stdint.h>
#include <stdlib.h>

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))
#define MAX(X, Y) (((X) > (Y)) ? (X) : (Y))
#define CLAMP(min, value, max) MIN(MAX(min, value), max)

#define COORD_INTEGER_BITS 14
#define COORD_FRACTIONAL_BITS 5
#define COORD_DENOMINATOR (1<<(COORD_FRACTIONAL_BITS))
#define COORD_RESOLUTION 1.0f / (1 << COORD_FRACTIONAL_BITS)

enum { COORD_INT_BITS_MP = 11 };
enum { FRAC_BITS_LP = 3 };
enum { FRAC_BITS = 5 };
enum { MAX_EDICTS = 4095 };

#define ARRAYSIZE(a) \
  ((sizeof(a) / sizeof(*(a))) / \
  (size_t)(!(sizeof(a) % sizeof(*(a)))))


void* demogobbler_dynamic_array_add(dynamic_array* thisptr, size_t count);
void demogobbler_dynamic_array_free(dynamic_array* thisptr);
void demogobbler_dynamic_array_init(dynamic_array* thisptr, size_t min_allocation, size_t item_size);
void* demogobbler_dynamic_array_get(dynamic_array* thisptr, int64_t offset); // Gets pointer to offset, does bound checking

static inline int64_t demogobbler_dynamic_array_offset(dynamic_array* thisptr, void* ptr) {
  return (uint8_t*)ptr - (uint8_t*)thisptr->ptr;
}

#define dynamic_array_add demogobbler_dynamic_array_add
#define dynamic_array_free demogobbler_dynamic_array_free
#define dynamic_array_init demogobbler_dynamic_array_init
#define dynamic_array_get demogobbler_dynamic_array_get
#define dynamic_array_offset demogobbler_dynamic_array_offset

unsigned int highest_bit_index(unsigned int number);
