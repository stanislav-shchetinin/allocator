#define _DEFAULT_SOURCE

#define BLOCK_MIN_CAPACITY 24

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mem_internals.h"
#include "mem.h"
#include "util.h"

void debug_block(struct block_header* b, const char* fmt, ... );
void debug(const char* fmt, ... );

extern inline block_size size_from_capacity( block_capacity cap );
extern inline block_capacity capacity_from_size( block_size sz );

static bool            block_is_big_enough( size_t query, struct block_header* block ) { return block->capacity.bytes >= query; }
static size_t          pages_count   ( size_t mem )                      { return mem / getpagesize() + ((mem % getpagesize()) > 0); }
static size_t          round_pages   ( size_t mem )                      { return getpagesize() * pages_count( mem ) ; }

static void block_init( void* restrict addr, block_size block_sz, void* restrict next ) {
  *((struct block_header*)addr) = (struct block_header) {
    .next = next,
    .capacity = capacity_from_size(block_sz),
    .is_free = true
  };
}

static size_t region_actual_size( size_t query ) { return size_max( round_pages( query ), REGION_MIN_SIZE ); }

extern inline bool region_is_invalid( const struct region* r );

static void* map_pages(void const* addr, size_t length, int additional_flags) {
  return mmap( (void*) addr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | additional_flags , -1, 0 );
}

static void* block_after( struct block_header const* block ) {
  if (!block) return 0;
  return  (void*) (block->contents + block->capacity.bytes);
}

static bool blocks_continuous (
                               struct block_header const* fst,
                               struct block_header const* snd ) {
  return (void*)snd == block_after(fst);
}

/*  аллоцировать регион памяти и инициализировать его блоком */
static struct region alloc_region  ( void const * addr, size_t query ) {
  size_t size = region_actual_size(size_from_capacity( (block_capacity) {.bytes = query} ).bytes);
  bool extends = true;
  void* _malloc_addr = map_pages(addr, size, MAP_FIXED);
  if (_malloc_addr == MAP_FAILED){
     _malloc_addr = map_pages(addr, size, 0);
     extends = false;
     if (_malloc_addr == MAP_FAILED) {
      return REGION_INVALID;
     }
  }
  block_init(_malloc_addr, (block_size) {size}, 0);
  return (struct region) {.addr = _malloc_addr, .size = size, .extends = extends};
}

void* heap_init( size_t initial ) {
  const struct region region = alloc_region( HEAP_START, initial );
  if ( region_is_invalid(&region) ) return NULL;

  return region.addr;
}

/*  освободить всю память, выделенную под кучу */
void heap_term() {

  struct block_header* heap_pt = (struct block_header*)HEAP_START;
  struct block_header* cur_block = heap_pt;
  size_t heap_size = 0;

  while (cur_block != 0){
    heap_size += size_from_capacity(cur_block->capacity).bytes;
    struct block_header* next_block = cur_block->next;
    if (!next_block || !blocks_continuous(cur_block, next_block)){
      munmap(heap_pt, heap_size);
      heap_pt = next_block;
      cur_block = heap_pt;
      heap_size = 0;
    } else {
      cur_block = cur_block->next;
    }
  }
}

/*  --- Разделение блоков (если найденный свободный блок слишком большой )--- */

static bool block_splittable( struct block_header* restrict block, size_t query) {
  return block && block-> is_free && query + offsetof( struct block_header, contents ) + BLOCK_MIN_CAPACITY <= block->capacity.bytes;
}

static bool split_if_too_big( struct block_header*  block, size_t query ) {
  if (!block || !block_splittable(block, query)){
    return false;
  }
  
  size_t size_left_block = size_from_capacity( (block_capacity) {query} ).bytes;
  size_t size_all_block = size_from_capacity( block->capacity ).bytes;

  block->capacity = (block_capacity) {query};

  block_init(block_after(block),
            (block_size) {size_all_block - size_left_block},
            block->next);

  
  block->next = block_after(block);
  return true;
}


/*  --- Слияние соседних свободных блоков --- */

static bool mergeable(struct block_header const* restrict fst, struct block_header const* restrict snd) {
  return fst->is_free && snd->is_free && blocks_continuous( fst, snd ) ;
}

static bool try_merge_with_next( struct block_header* block ) {
  if (!block || !block->next || !mergeable(block, block->next)){
    return false;
  }
  block->capacity = (block_capacity) {block->capacity.bytes + size_from_capacity(block->next->capacity).bytes};
  block->next = block->next->next;
  return true;
}

/*  --- ... ecли размера кучи хватает --- */

struct block_search_result {
  enum {BSR_FOUND_GOOD_BLOCK, BSR_REACHED_END_NOT_FOUND, BSR_CORRUPTED} type;
  struct block_header* block;
};


static struct block_search_result find_good_or_last  ( struct block_header* restrict block, size_t sz )    {
  if (!block){
    return (struct block_search_result){BSR_CORRUPTED};
  }
  while (block){
    while (try_merge_with_next(block)){}
    if (block->capacity.bytes >= sz && block->is_free){
      return (struct block_search_result) {.type = BSR_FOUND_GOOD_BLOCK, .block = block};
    }
    if (!block->next) break;
    block = block->next;
  }
  return (struct block_search_result) {.type = BSR_REACHED_END_NOT_FOUND, .block = block};
}

/*  Попробовать выделить память в куче начиная с блока `block` не пытаясь расширить кучу
 Можно переиспользовать как только кучу расширили. */
static struct block_search_result try_memalloc_existing ( size_t query, struct block_header* block ) {
  struct block_search_result res = find_good_or_last(block, query);
  if (res.type == BSR_FOUND_GOOD_BLOCK){
    split_if_too_big(res.block, query);
    res.block->is_free = false;
    return (struct block_search_result) {.type = BSR_FOUND_GOOD_BLOCK, .block = res.block};
  }
  return res;
}

static struct block_header* grow_heap( struct block_header* restrict last, size_t query ) {
  if (!last) return 0;
  struct region reg = alloc_region(block_after(last), query);
  if (reg.addr){
    last->next = reg.addr;
    if (try_merge_with_next(last)){
      return last;
    }
  }
  return (struct block_header*) reg.addr;
}

/*  Реализует основную логику malloc и возвращает заголовок выделенного блока */
static struct block_header* memalloc( size_t query, struct block_header* heap_start) {
  if (!heap_start) return 0;
  query = size_max(query, BLOCK_MIN_CAPACITY);
  struct block_search_result res = try_memalloc_existing(query, heap_start);

  if (res.type == BSR_REACHED_END_NOT_FOUND){
    struct block_header* block = grow_heap(res.block, query);
    if (!block){
      return 0;
    }
    return try_memalloc_existing(query, block).block;
  }

  return res.block;

}

void* _malloc( size_t query ) {
  struct block_header* const addr = memalloc( query, (struct block_header*) HEAP_START );
  if (addr) return addr->contents;
  return NULL;
}

static struct block_header* block_get_header(void* contents) {
  return (struct block_header*) (((uint8_t*)contents)-offsetof(struct block_header, contents));
}

void _free( void* mem ) {
  if (!mem) return ;
  struct block_header* header = block_get_header( mem );
  header->is_free = true;
  while(try_merge_with_next(header)){}
}
