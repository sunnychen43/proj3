#include "my_vm.h"
#include <math.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define ADDR_SIZE 4  // change when 32-bit
#define VADDR_BASE PGSIZE

#define NUM_VPAGES      MAX_MEMSIZE/PGSIZE
#define NUM_PPAGES      MEMSIZE/PGSIZE

#define PPAGE_BITMAP_SIZE NUM_PPAGES/8
#define VPAGE_BITMAP_SIZE NUM_VPAGES/8

#define TAG_BITS (int)log2(TLB_ENTRIES)

static int PT_BITS, PD_BITS, OFFSET_BITS;



void set_bit_at_index(char *bitmap, int index) {
    *bitmap |= (1 << index);
}

void clear_bit_at_index(char *bitmap, int index) {
    *bitmap &= ~(1 << index);
}


int get_bit_at_index(char *bitmap, int index) {
    return (*bitmap >> index) & 1;
}


void print_bitmap(char *bitmap) {
    for (int i=0; i < 8; i++) {
        printf("%d", get_bit_at_index(bitmap, i));
    }
    printf("\n");
}

unsigned int get_top_bits(unsigned int value, int num_bits) {
    int num_bits_to_prune = 32 - num_bits; //32 assuming we are using 32-bit address 
    return (value >> num_bits_to_prune);
}

unsigned int get_mid_bits(unsigned int value, int num_middle_bits, int num_lower_bits) {
    unsigned int mid_bits_value = 0;   
    value = value >> num_lower_bits; 
    unsigned int outer_bits_mask =   (1 << num_middle_bits);  
    outer_bits_mask = outer_bits_mask-1;
    mid_bits_value = value & outer_bits_mask;
    return mid_bits_value;
}

unsigned int get_low_bits(unsigned int value, int num_bits) {
    return value & ((1<<num_bits)-1);
}

static char *mem, *pbitmap, *vbitmap;
static pde_t *pd;
static tlb_t tlb;
static bool flag;

void *find_next_ppage() {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {
        if (pbitmap[i] != ~0) {
            for (int j=0; j < 8; j++) {
                if (get_bit_at_index(pbitmap+i, j) == 0) {
                    set_bit_at_index(pbitmap+i, j);
                    size_t offset = PGSIZE * (i*8 + j);
                    return mem+offset;
                }
            }
        }
    }
    return NULL;
}

void set_physical_mem() {
    OFFSET_BITS = (int)log2(PGSIZE);
    PT_BITS = MIN(14, (int)log2(PGSIZE/ADDR_SIZE));
    PD_BITS = 32-PT_BITS-OFFSET_BITS;

    mem = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    pbitmap = malloc(PPAGE_BITMAP_SIZE);
    memset(pbitmap, 0, PPAGE_BITMAP_SIZE);
    vbitmap = malloc(VPAGE_BITMAP_SIZE);
    memset(vbitmap, 0, VPAGE_BITMAP_SIZE);

    // reserve one page for pd
    pd = find_next_ppage();
    // find_next_ppage();
    memset(pd, 0, (int)pow(2, PD_BITS)*ADDR_SIZE);

    pd[0] = (pte_t)find_next_ppage();
    memset((void *)pd[0], 0, PGSIZE);

    tlb.bins = malloc(sizeof(*tlb.bins) * TLB_ENTRIES);
    for (int i=0; i < TLB_ENTRIES; i++) {
        tlb.bins[i].addr = NULL;
    }
}


int add_TLB(void *va, void *pa) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int tag = get_top_bits(addr, TAG_BITS);
    int index = get_mid_bits(addr, 32-TAG_BITS-OFFSET_BITS, OFFSET_BITS);

    tlb.bins[index].tag = tag;
    tlb.bins[index].addr = (void*)((unsigned long)pa >> 12);

    return 0;
}


void *check_TLB(void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int tag = get_top_bits(addr, TAG_BITS);
    int index = get_mid_bits(addr, 32-TAG_BITS-OFFSET_BITS, OFFSET_BITS);
    int offset = get_low_bits(addr, OFFSET_BITS);

    if (tlb.bins[index].addr != NULL && tlb.bins[index].tag == tag) {
        void *pa = (void*)((unsigned long)tlb.bins[index].addr << 12) + offset;
        return pa;
    }
    else {
        return NULL;
    }
}


/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
void *translate(pde_t *pgdir, void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);
    int offset = get_low_bits(addr, OFFSET_BITS);

    void *pa = check_TLB(va);
    if (pa != NULL) {
        return pa+offset;
    }
    else {
        pte_t *pt = (pte_t*)pd[pd_index];
        pa = (void*)pt[pt_index]+offset;
        add_TLB(va, pa);
        return pa; 
    }
}


/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int page_map(pde_t *pgdir, void *va, void *pa) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    int bit = get_bit_at_index(&vbitmap[index/8], index%8);
    if (bit == 1) {
        return -1;
    }

    pte_t *pt = (pte_t*)pd[pd_index];
    if (pt != NULL) {
        pt[pt_index] = (pte_t)pa;
        return 0;
    }
    else {
        return 1;
    }
}


/*Function that gets the next available page
*/
void *get_next_avail(int num_pages) {
    int count = 0;
    for (int i=0; i < VPAGE_BITMAP_SIZE; i++) {
        for (int j=0; j < 8; j++) {
            int bit = get_bit_at_index(&vbitmap[i], j);
            if (bit == 0) {
                count++;
                if (count == num_pages) {
                    int index = (i*8 + j) - (num_pages-1);
                    return (void*)(unsigned long)(VADDR_BASE + index*PGSIZE);
                }
            }
            else {
                count = 0;
            }
        }
    }
    return NULL;
}


void change_vbitmap(void *va, int val) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    if (val == 1) {
        set_bit_at_index(&vbitmap[index/8], index%8);
    }
    else if (val == 0) {
        clear_bit_at_index(&vbitmap[index/8], index%8);
    }
}

pde_t get_pd_entry(void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    return pd[pd_index];
}

pte_t get_pt_entry(void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    pte_t *pt = (pte_t*)pd[pd_index];
    return pt[pt_index];
}

/* Function responsible for allocating pages
and used by the benchmark
*/
void *a_malloc(unsigned int num_bytes) {
    while (__sync_lock_test_and_set(&flag, 1) == 1) {
    }

    if (mem == NULL) {
        set_physical_mem();
    }

    int num_pages = (int)ceil((double)num_bytes/PGSIZE);
    void *base_va = get_next_avail(num_pages);
    for (int i=0; i < num_pages; i++) {
        void *va = base_va+i*PGSIZE;
        if (get_pd_entry(va) == 0) {
            void *ppage = find_next_ppage();
            if (ppage == NULL) {
                printf("Mem fulla\n");
                return NULL;
            }
            unsigned long addr = (unsigned long)va - VADDR_BASE;
            int pd_index = get_top_bits(addr, PD_BITS);
            pd[pd_index] = (pte_t)ppage;
            memset((void*)pd[pd_index], 0, PGSIZE);
        }
        if (get_pt_entry(va) == 0) {
            void *ppage = find_next_ppage();
            if (ppage == NULL) {
                printf("Mem fullb\n");
                return NULL;
            }
            page_map(pd, va, ppage);
        }
        change_vbitmap(va, 1);
    }
    __sync_lock_test_and_set(&flag, 0);
    return base_va;
}

/* Responsible for releasing one or more memory pages using virtual address (va)
*/
void a_free(void *va, int size) {
    while (__sync_lock_test_and_set(&flag, 1) == 1) {
    }
    for (void *addr=va; addr < va+size; addr += PGSIZE) {
        change_vbitmap(addr, 0);
    }
    __sync_lock_test_and_set(&flag, 0);
}


/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
*/
void put_value(void *va, void *val, int size) {
    char *addr;
    for (int i=0; i < size; i++) {
        addr = translate(pd, va+i);
        *addr = *((char*)val+i);
    }
}


/*Given a virtual address, this function copies the contents of the page to val*/
void get_value(void *va, void *val, int size) {
    char *addr;
    for (int i=0; i < size; i++) {
        addr = translate(pd, va+i);
        *((char*)val+i) = *addr;
    }
}



/*
This function receives two matrices mat1 and mat2 as an argument with size
argument representing the number of rows and columns. After performing matrix
multiplication, copy the result to answer.
*/
void mat_mult(void *mat1, void *mat2, int size, void *answer) {
    for (int i=0; i < size; i++) {
        for (int j=0; j < size; j++) {
            int sum = 0;
            for (int k=0; k < size; k++) {
                void *a = mat1 + (i*size + k)*sizeof(int);
                void *b = mat2 + (k*size + j)*sizeof(int);

                int mat1_val, mat2_val;
                get_value(a, &mat1_val, sizeof(int));
                get_value(b, &mat2_val, sizeof(int));
                sum += mat1_val * mat2_val;
            }
            void *addr = answer + (i*size + j)*sizeof(int);
            put_value(addr, &sum, sizeof(int));
        }
    }
}


// int main() {
//     // a_malloc(PGSIZE*1024);
//     // int count = 0;
//     // for (int i=0; i < VPAGE_BITMAP_SIZE; i++) {
//     //     for (int j=0; j < 8; j++) {
//     //         if (get_bit_at_index(&pbitmap[i], j) == 1)
//     //             count++;
//     //     }
//     // }
//     // printf("%d\n", count);

//     a_malloc(1);
//     printf("%d %d %d\n", PD_BITS, PT_BITS, OFFSET_BITS);
// }