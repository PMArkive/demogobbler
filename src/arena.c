#include "demogobbler_arena.h"
#include "utils.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

arena demogobbler_arena_create(uint32_t first_block_size) {
  arena out;
  memset(&out, 0, sizeof(arena));
  out.first_block_size = first_block_size;

  return out;
}


void demogobbler_arena_clear(arena* a) {
  for(size_t i=0; i < a->block_count; ++i) {
    a->blocks[i].bytes_used = 0;
  }
  a->current_block = 0;
}

static size_t block_bytes_left(struct demogobbler_arena_block* block, uint32_t size, uint32_t alignment) {
  if(block == NULL) {
    return 0;
  }
  else {
    size_t inc = alignment_loss(block, alignment);
    return block->total_bytes - (block->bytes_used + inc);
  }
}

static struct demogobbler_arena_block* get_last_block(arena* a) {
  if(a->block_count == 0) {
    return NULL;
  }
  else {
    return &a->blocks[a->block_count - 1];
  }
}

static void allocate_new_block(arena* a, uint32_t requested_size) {
  size_t allocated_size;
  if(a->block_count == 0) {
    allocated_size = a->first_block_size;
  }
  else {
    struct demogobbler_arena_block* ptr = get_last_block(a);
    allocated_size = ptr->total_bytes * 2; // Double the size for the next block
  }

  allocated_size = MAX(1, allocated_size); // Allocate at least 1 byte

  // This shouldnt usually happen since allocations are supposed to be a fraction of the total size of the block
  // But make sure that we have enough space for the next allocation
  while(allocated_size < requested_size) {
    allocated_size *= 2;
  }

  // God help you if you allocate this much memory
  if(allocated_size > UINT32_MAX) {
    allocated_size = UINT32_MAX;
  }

  ++a->block_count;
  a->blocks = realloc(a->blocks, sizeof(struct demogobbler_arena_block) * a->block_count);
  struct demogobbler_arena_block* ptr = get_last_block(a);
  ptr->data = malloc(allocated_size);
  ptr->bytes_used = 0;
  ptr->total_bytes = allocated_size;
}

static struct demogobbler_arena_block* find_block_with_memory(arena* a, uint32_t requested_size, uint32_t alignment) {
  struct demogobbler_arena_block* ptr;

  if(a->block_count == 0) {
    allocate_new_block(a, requested_size);
  }

  ptr = &a->blocks[a->current_block];

  while(block_bytes_left(ptr, requested_size, alignment) < requested_size) {
    ++a->current_block;
    if(a->current_block >= a->block_count) {
      allocate_new_block(a, requested_size);
      return &a->blocks[a->current_block];
    }
    ptr = &a->blocks[a->current_block];
  }

  return ptr;
}

static void* allocate(struct demogobbler_arena_block* block, uint32_t size, uint32_t alignment) {
  size_t increment = alignment_loss(block->bytes_used, alignment);
  size_t allocation_size = size + increment; // size_t here in case overflow

  if(block->data == NULL || allocation_size > block->total_bytes - block->bytes_used) { 
    return NULL;
  }
  else {
    void* out = (uint8_t*)block->data + block->bytes_used + increment;
    block->bytes_used += size + increment;
    return out;
  }
}

void* demogobbler_arena_allocate(arena* a, uint32_t size, uint32_t alignment) {
  if(size == 0) {
    return NULL;
  }

  struct demogobbler_arena_block* ptr = find_block_with_memory(a, size, alignment);
  return allocate(ptr, size, alignment);
}

void demogobbler_arena_free(arena* a) {
  for(size_t i=0; i < a->block_count; ++i) {
    free(a->blocks[i].data);
  }
  free(a->blocks);
  memset(a, 0, sizeof(arena));
}
