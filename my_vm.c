#include "my_vm.h"
#include "bit.h"
#include <math.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))


/********** Local Constants and Defs **********/

#define ADDR_SIZE           sizeof(void*)
#define VADDR_BASE          PGSIZE  
#define PPAGE_BITMAP_SIZE   (MEMSIZE/PGSIZE/8)
#define VPAGE_BITMAP_SIZE   (MAX_MEMSIZE/PGSIZE/8)

static int PD_BITS, PT_BITS, OFFSET_BITS;
static int TAG_BITS, TLB_INDEX_BITS;


/********** Local Function Definitions **********/

void set_physical_mem();
void *find_next_page();
void set_pbitmap(char *bitmap, void *pa, int val);

int add_TLB(void *va, void *pa);
void *check_TLB(void *va);
void print_TLB_missrate();

void set_vbitmap(char *bitmap, void *va, int val);
void *translate(pde_t *pgdir, void *va);
int page_map(pde_t *pgdir, void *va, void* pa);
void *find_next_addr(int num_pages);


/********** Static Variable Definitions **********/

static char *mem, *pbitmap, *vbitmap;
static pde_t *pd;
static tlb_t tlb;
static bool  flag;

void print_bitmap(char *bitmap) {
    for (int i=0; i < 8; i++) {
        printf("%d", get_bit_at_index(bitmap, i));
    }
    printf("\n");
}


/********** Physical Mem Functions **********/

/* 
 * Startup function called the first time a_malloc() is run. Defines how page table
 * is structured and initializes physical mem, tlb, bitmaps, and page dir.
 */
void set_physical_mem() {
    /* Define how the bits in addr index into page table */
    OFFSET_BITS = (int)log2(PGSIZE);
    PT_BITS = MIN(14, (int)log2(PGSIZE/ADDR_SIZE));
    PD_BITS = 32-PT_BITS-OFFSET_BITS;
    TLB_INDEX_BITS = (int)log2(TLB_ENTRIES);
    TAG_BITS = 32-TLB_INDEX_BITS-OFFSET_BITS;

    /* Initialize main mem */
    mem = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* Initialize bitmaps */
    pbitmap = malloc(PPAGE_BITMAP_SIZE);
    memset(pbitmap, 0, PPAGE_BITMAP_SIZE);
    vbitmap = malloc(VPAGE_BITMAP_SIZE);
    memset(vbitmap, 0, VPAGE_BITMAP_SIZE);

    // reserve one page for pd
    pd = find_next_page();
    if (PD_BITS > 10)  // reserve another page if pd has more than 2^10 entries
        find_next_page();

    memset(pd, 0, (int)pow(2, PD_BITS)*ADDR_SIZE);

    /* Initialize tlb */
    tlb.bins = malloc(sizeof(*(tlb.bins)) * TLB_ENTRIES);
    for (int i=0; i < TLB_ENTRIES; i++) {
        tlb.bins[i].addr = NULL;  // addr == NULL means tlb entry isn't valid
    }
}

/* 
 * Scans physical bitmap for an avaliable page. If a page is found,
 * return physical address of the page. Otherwise if all pages are
 * in use, return NULL
 */
void *find_next_page() {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {

        /* Instead of checking bit by bit, we can check the value of
        each element in pbitmap. If pbitmap[i] == ~0, that means all
        bits are set to 1 and we can skip it.*/
        if (pbitmap[i] != ~0) {
            for (int j=0; j < 8; j++) {  // go through each bit
                if (get_bit_at_index(pbitmap+i, j) == 0) {
                    set_bit_at_index(pbitmap+i, j);
                    size_t offset = PGSIZE * (i*8 + j);  // offset from main mem ptr
                    return mem+offset;
                }
            }
        }
    }
    return NULL;
}

void set_pbitmap(char *bitmap, void *pa, int val) {
    unsigned long offset = (unsigned long)(pa - (void*)mem);
    unsigned long index = offset >> OFFSET_BITS;
    
    if (val == 1) {
        set_bit_at_index(&bitmap[index/8], index%8);
    }
    else if (val == 0) {
        clear_bit_at_index(&bitmap[index/8], index%8);
    }
}


/********** TLB Functions **********/

static int miss, access;
static bool tlb_flag;
/*
 * Stores a mapping from va -> pa in the tlb. Since this is an internal
 * function (can only be called within my_vm.c), assumes that va is a valid
 * virtual address within bounds.
 */
int add_TLB(void *va, void *pa) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int tag = get_top_bits(addr, TAG_BITS);
    int index = get_mid_bits(addr, TLB_INDEX_BITS, OFFSET_BITS);
    
    tlb.bins[index].tag = tag;
    tlb.bins[index].addr = (void*)((unsigned long)pa >> OFFSET_BITS);

    return 0;
}

/*
 * Searches TLB to see if there is an existing mapping for va. Returns
 * the physical address if there is, otherwise return NULL.
 */
void *check_TLB(void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int tag = get_top_bits(addr, TAG_BITS);
    int index = get_mid_bits(addr, TLB_INDEX_BITS, OFFSET_BITS);

    // check if tlb entry is valid and tag matches
    if (tlb.bins[index].addr != NULL && tlb.bins[index].tag == tag) { 
        // addresses are stored w/o offset
        void *pa = (void*)((unsigned long)tlb.bins[index].addr << OFFSET_BITS);
        return pa;
    }
    else {
        return NULL;  // tlb entry invalid or tag mismatch
    }
}

void print_TLB_missrate() {
    double miss_rate = (double)miss / access;
    fprintf(stderr, "TLB miss rate %lf \n", miss_rate);
}


/********** Virtual Mem Functions **********/

/*
 * Finds the corresponding bit to virtual address `va` and sets it to
 * val. Assume that bitmap is a valid virtual bitmap with the correct
 * size.
 */
void set_vbitmap(char *bitmap, void *va, int val) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    if (val == 1) {
        set_bit_at_index(&bitmap[index/8], index%8);  // bitmap is an array of chars, so 8 bits in each index
    }
    else if (val == 0) {
        clear_bit_at_index(&bitmap[index/8], index%8);
    }
}

/*
 * Translates a virtual address to a physical address. First checks the tlb 
 * to see if a mapping exists there. If there is, return the address from 
 * tlb. Otherwise, go through the page table and find the physical address, 
 * return it and add mapping to tlb.
 */
void *translate(pde_t *pgdir, void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);
    int offset = get_low_bits(addr, OFFSET_BITS);

    void *pa = check_TLB(va);
    access++;
    if (pa != NULL) {
        return pa+offset;
    }
    else {
        miss++;
        pte_t *pt = (pte_t*)pd[pd_index];
        pa = (void*)pt[pt_index];
        add_TLB(va, pa);
        return pa+offset; 
    }
}

/*
 * This function will walk the page dir to see if there is an existing 
 * mapping for a virtual address. If the virtual address is not present, 
 * returns 1 and a new mapping will be made. Otherwise returns 0 and does 
 * nothing.
 */
int page_map(pde_t *pgdir, void *va, void *pa) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int pd_index = get_top_bits(addr, PD_BITS);
    int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    int bit = get_bit_at_index(&vbitmap[index/8], index%8);
    if (bit == 1 || pd[pd_index] == 0)
        return 1;

    pte_t *pt = (pte_t*)pd[pd_index];
    pt[pt_index] = (pte_t)pa;
    return 0;
}

/*
 * Searchs virtual bitmap for a set of contiguous avaliable virtual addresses 
 * that can hold the given number of pages. If a set is found, return the 
 * lowest virtual address that's free in the set. Otherwise, return NULL.
 */
void *find_next_addr(int num_pages) {
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


/********** Public Functions **********/

/* 
 * Memory allocation function. Given a number of bytes, tries to find room 
 * to allocate in the virtual address space. If there is room, create mappings 
 * for virtual addresses to physical pages and return the virtual address. 
 * Will always return the lowest valid virtual address. If there's no room, 
 * return NULL.
 */
void *a_malloc(unsigned int num_bytes) {
    while (__sync_lock_test_and_set(&flag, 1) == 1) {
    }

    if (mem == NULL)
        set_physical_mem();

    int num_pages = (int)ceil((double)num_bytes/PGSIZE);
    void *base_va = find_next_addr(num_pages);
    if (base_va == NULL) {
        printf("No room in virt address space\n");
        return NULL;
    }
    for (int i=0; i < num_pages; i++) {
        void *va = base_va+i*PGSIZE;
        unsigned long addr = (unsigned long)va-VADDR_BASE;

        int pd_index = get_top_bits(addr, PD_BITS);
        if (pd[pd_index] == 0) {
            void *ppage = find_next_page();
            if (ppage == NULL) {
                printf("PDE allocation failed\n");
                return NULL;
            }
            pd[pd_index] = (pde_t)ppage;
        }

        pte_t *pt = (pte_t*)pd[pd_index];
        int pt_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);
        if (pt[pt_index] == 0) {
            void *ppage = find_next_page();
            if (ppage == NULL) {
                printf("PTE allocation failed\n");
                return NULL;
            }
            page_map(pd, va, ppage);
        }
        set_vbitmap(vbitmap, va, 1);
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
        set_vbitmap(vbitmap, addr, 0);

        void *pa = translate(pd, addr);
        page_map(pd, va, NULL);
        set_pbitmap(pbitmap, pa, 0);

        add_TLB(addr, NULL);
    }
    __sync_lock_test_and_set(&flag, 0);
}

/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
*/
void put_value(void *va, void *val, int size) {
    void *base = (void*)((unsigned long)va >> OFFSET_BITS << OFFSET_BITS);
    void *next_page = base+PGSIZE;

    if (size <= next_page-va) {
        memcpy(translate(pd, va), val, size);
        return;
    }

    int first_size = next_page-va;
    int bytes_copied = first_size;
    memcpy(translate(pd, va), val, first_size);

    while (bytes_copied+PGSIZE <= size) {
        memcpy(translate(pd, next_page), val+bytes_copied, PGSIZE);
        next_page += PGSIZE;
        bytes_copied += PGSIZE;
    }

    if (bytes_copied < size) {
        memcpy(translate(pd, next_page), val+bytes_copied, size - bytes_copied);
    }
}

/*Given a virtual address, this function copies the contents of the page to val*/
void get_value(void *va, void *val, int size) {
    void *base = (void*)((unsigned long)va >> OFFSET_BITS << OFFSET_BITS);
    void *next_page = (void*)(base+PGSIZE);

    if (size <= next_page-va) {
        memcpy(val, translate(pd, va), size);
        return;
    }

    memcpy(val, translate(pd, va), next_page-va);
    int bytes_copied = next_page-va;

    while (bytes_copied+PGSIZE <= size) {
        memcpy(val+bytes_copied, translate(pd, va+bytes_copied), PGSIZE);
        bytes_copied += PGSIZE;
    }

    if (bytes_copied < size) {
        memcpy(val+bytes_copied, translate(pd, va+bytes_copied), size-bytes_copied);
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
    void *addr = a_malloc(1ULL*1024*1024*1024);
    printf("%p\n", addr);
    // printf("%p\n", translate(pd, addr));
    // printf("%p\n", check_TLB(addr));
    // print_bitmap(vbitmap);
    // print_bitmap(pbitmap);
    // a_free(addr, 1);
    // print_bitmap(vbitmap);
    // print_bitmap(pbitmap);
    // print_TLB_missrate();
}