#ifndef MY_VM_H_INCLUDED
#define MY_VM_H_INCLUDED
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

//Assume the address space is 32 bits, so the max memory size is 4GB
//Page size is 4KB

//Add any important includes here which you may need

#define PGSIZE (4096)

// Maximum size of virtual memory
#define MAX_MEMSIZE (4ULL*1024*1024*1024)

// Size of "physcial memory"
#define MEMSIZE (1ULL*1024*1024*1024)

typedef unsigned long pte_t;
typedef unsigned long pde_t;

#define TLB_ENTRIES 1024
//Structure to represents TLB
typedef struct tlb_entry_t {
    unsigned long tag;
    void *addr;
} tlb_entry_t;

typedef struct tlb_t {
    tlb_entry_t *bins;
} tlb_t;


void *a_malloc(unsigned int num_bytes);
void a_free(void *va, int size);
void put_value(void *va, void *val, int size);
void get_value(void *va, void *val, int size);
void mat_mult(void *mat1, void *mat2, int size, void *answer);
void print_TLB_missrate();

#endif
