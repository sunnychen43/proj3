#include "my_vm.h"
#include <math.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define ADDR_SIZE 8  // change when 32-bit
#define VADDR_BASE 0x3000

#define NUM_VPAGES      MAX_MEMSIZE/PGSIZE
#define NUM_PPAGES      MEMSIZE/PGSIZE
#define NUM_BLOCKS      MAX_MEMSIZE/MEMSIZE  //4

#define PPAGE_BITMAP_SIZE NUM_PPAGES/8
#define VPAGE_BITMAP_SIZE NUM_VPAGES/8

#define OFFSET_BITS (int)log2(PGSIZE)
#define PT_BITS (int)log2(PGSIZE/ADDR_SIZE)
#define PD_BITS 32-PT_BITS-OFFSET_BITS



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

static char *mem;
static char *pbitmap, *vbitmap;
static pde_t *pd;
static pthread_mutex_t lock;
static bool flag;

void *find_next_ppage() {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {
        if (pbitmap[i] != 127) {
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

void *find_next_vaddr() {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {
        if (vbitmap[i] != 127) {
            for (int j=0; j < 8; j++) {
                if (get_bit_at_index(vbitmap+i, j) == 0) {
                    // set_bit_at_index(vbitmap+i, j);
                    size_t offset = PGSIZE * (i*8 + j);
                    return (void *)(0ULL+offset+VADDR_BASE);
                }
            }
        }
    }
    return NULL;
}


void set_physical_mem() {
    mem = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    pbitmap = malloc(PPAGE_BITMAP_SIZE);
    memset(pbitmap, 0, PPAGE_BITMAP_SIZE);
    vbitmap = malloc(VPAGE_BITMAP_SIZE);
    memset(vbitmap, 0, VPAGE_BITMAP_SIZE);

    // reserve two pages for pd
    pd = find_next_ppage();
    find_next_ppage();
    memset(pd, 0, (int)pow(2, PD_BITS)*ADDR_SIZE);

    pd[0] = (pte_t)find_next_ppage();
    memset((void *)pd[0], 0, PGSIZE);
}


/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
void *translate(pde_t *pgdir, void *va) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);
    int offset = get_low_bits(addr, OFFSET_BITS);

    pte_t *pt = (pte_t*)pd[pd_index];
    return (void *)pt[pt_index]+offset; 
}


/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int page_map(pde_t *pgdir, void *va, void *pa) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    int bit = get_bit_at_index(&vbitmap[index/8], index%8);
    if (bit == 1) {
        return -1;
    }

    pte_t *pt = (pte_t*)pd[pd_index];
    if (pt == NULL) {
        
    }
    pt[pt_index] = (pte_t)pa;

    return 0;
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


void reserve_vpage(void *va) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    set_bit_at_index(&vbitmap[index/8], index%8);
}

void clear_vpage(void *va) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    clear_bit_at_index(&vbitmap[index/8], index%8);
}

pde_t get_pd_entry(void *va) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    return pd[pd_index];
}

pte_t get_pt_entry(void *va) {
    unsigned int addr = (unsigned int)va - VADDR_BASE;
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

    int num_pages = ceil((double)num_bytes/PGSIZE);
    void *base_va = get_next_avail(num_pages);
    for (int i=0; i < num_pages; i++) {
        void *va = base_va+i*PGSIZE;
        if (get_pd_entry(va) == 0) {
            unsigned int addr = (unsigned int)va - VADDR_BASE;
            int pd_index = get_top_bits(addr, PD_BITS);
            pd[pd_index] = (pte_t)find_next_ppage();
            memset((void*)pd[pd_index], 0, PGSIZE);
        }
        if (get_pt_entry(va) == 0) {
            void *page = find_next_ppage();
            page_map(pd, va, page);
        }
        reserve_vpage(va);
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
        clear_vpage(addr);
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


int main() {
    a_malloc(PGSIZE*1024);
    int count = 0;
    for (int i=0; i < VPAGE_BITMAP_SIZE; i++) {
        for (int j=0; j < 8; j++) {
            if (get_bit_at_index(&pbitmap[i], j) == 1)
                count++;
        }
    }
    printf("%d\n", count);
}