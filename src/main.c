#include "mem.h"
#include "mem_internals.h"
#include <assert.h>
#include <stdio.h>

#define HEAP_SIZE (4 * 4096)

void end_test(){
    printf("\n");
    heap_term();
}

void start_test(){
    void* addr = heap_init(HEAP_SIZE);
    debug_heap(stdout, addr);
    assert(addr != NULL);
}

void print_test(int num_test, char* msg){
    printf("TEST #%d\n", num_test);
    printf("%s\n", msg);
}

void debug_info(void** block_arr){
    size_t n = (int32_t)sizeof(block_arr)/(int32_t)sizeof(void*);
    for (size_t i = 0; i < n; ++i){
        debug_struct_info(stdout, block_arr[i] - offsetof(struct block_header, contents));
    }
}


void success_allocate_memory(){
    print_test(1, "SUCCESS ALLOCATE MEMORY");
    size_t str_size = 24;
    start_test();   
    end_test();
}

void free_one_block(){
    print_test(2, "FREE ONE BLOCK");
    start_test();

    void* arr[3] = {_malloc(24), _malloc(24), _malloc(24)};
    _free(arr[1]);
    debug_info(arr);

    end_test();
}

void free_two_blocks(){
    print_test(3, "FREE TWO BLOCKS");
    start_test();

    void* arr[3] = {_malloc(24), _malloc(24), _malloc(24)};
    _free(arr[0]);
    _free(arr[2]);
    debug_info(arr);

    end_test();
}

void mem_end(){
    print_test(4, "MEM END");
    start_test();

    void* arr[2] = {_malloc(HEAP_SIZE), _malloc(HEAP_SIZE)};
    debug_info(arr);
    
    end_test();
}

void mem_end_another_place(){
    print_test(5, "ANOTHER PLACE");
    start_test();

    void* arr[2] = {_malloc(HEAP_SIZE - 13), _malloc(HEAP_SIZE)};
    debug_info(arr);
    
    end_test();
}

int main() {

    success_allocate_memory();
    free_one_block();
    free_two_blocks();
    mem_end();
    mem_end_another_place();

    return 0;
}