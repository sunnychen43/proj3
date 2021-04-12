#include "my_vm.h"
#include <math.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
        _a > _b ? _a : _b; })

#define ADDR_SIZE sizeof(void*)  // change when 32-bit
#define VADDR_BASE PGSIZE

#define NUM_VPAGES      MAX_MEMSIZE/PGSIZE
#define NUM_PPAGES      MEMSIZE/PGSIZE

#define PPAGE_BITMAP_SIZE NUM_PPAGES/8
#define VPAGE_BITMAP_SIZE PPAGE_BITMAP_SIZE*2

#define OFFSET_BITS (int)log2(PGSIZE)
#define PT_BITS (int)log2(PGSIZE/ADDR_SIZE)
#define PD_BITS 64-PT_BITS*3-OFFSET_BITS
#define TLB_INDEX_BITS (int)log2(TLB_ENTRIES)

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
    int num_bits_to_prune = 64 - num_bits; //32 assuming we are using 32-bit address 
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

static char *mem, *vbitmap, *pbitmap;
static pde_t *pd;
static int pd_size;
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
    mem = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    pbitmap = malloc(PPAGE_BITMAP_SIZE);
    memset(pbitmap, 0, PPAGE_BITMAP_SIZE);
    vbitmap = malloc(VPAGE_BITMAP_SIZE);
    memset(vbitmap, 0, VPAGE_BITMAP_SIZE);

    // reserve one page for pd
    pd = find_next_ppage();
    
    printf("%d %d\n", PD_BITS, (int)pow(2, PD_BITS));
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
    int tag = get_top_bits(addr, 64-TLB_INDEX_BITS-OFFSET_BITS);
    int index = get_mid_bits(addr, TLB_INDEX_BITS, OFFSET_BITS);

    tlb.bins[index].tag = tag;
    tlb.bins[index].addr = (void*)((unsigned long)pa >> OFFSET_BITS);

    return 0;
}


void *check_TLB(void *va) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    int tag = get_top_bits(addr, 64-TLB_INDEX_BITS-OFFSET_BITS);
    int index = get_mid_bits(addr, TLB_INDEX_BITS, OFFSET_BITS);
    int offset = get_low_bits(addr, OFFSET_BITS);

    if (tlb.bins[index].addr != NULL && tlb.bins[index].tag == tag) {
        void *pa = (void*)((unsigned long)tlb.bins[index].addr << OFFSET_BITS) + offset;
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
    int offset = get_low_bits(addr, OFFSET_BITS);

    void *pa = check_TLB(va);
    if (pa != NULL) {
        return pa+offset;
    }
    else {
        pte_t *page = (pte_t*)pd[pd_index];

        for (int i=0; i < 3; i++) {
            if (page == NULL)
                return NULL;
            int index = get_mid_bits(addr, PT_BITS, OFFSET_BITS+PT_BITS*(2-i));
            page = (pte_t*)page[index];
        }

        if (page == NULL)
            return NULL;
        
        pa = (void*)page+offset;
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
        return 1;
    }

    pte_t *pt = (pte_t*)pd[pd_index];
    if (pt == NULL)
        return 1;
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


void change_vbitmap(void *va, int val) {
    unsigned long addr = (unsigned long)va - VADDR_BASE;
    unsigned long index = addr >> OFFSET_BITS;
    
    if (val == 1) {
        set_bit_at_index(&vbitmap[index/8], index%8);
    }
    else if (val == 0) {
        clear_bit_at_index(&vbitmap[index/8], index%8);
    }
}

pte_t *get_page_entry(pte_t *page, int index) {
    pte_t *entry = (pte_t*)page[index];
    if (entry == NULL) {
        void *ppage = find_next_ppage();
        if (ppage == NULL)
            return NULL;

        page[index] = (pte_t)ppage;
        memset((void*)page[index], 0, PGSIZE);
    }
    return (pte_t*)page[index];
}

/* Function responsible for allocating pages
and used by the benchmark
*/
void *a_malloc(unsigned int num_bytes) {
    while (__sync_lock_test_and_set(&flag, 1) == 1) {
    }

    if (mem == NULL) {
        printf("a\n");
        set_physical_mem();
    }

    int num_pages = (int)ceil((double)num_bytes/PGSIZE);
    void *base_va = get_next_avail(num_pages);
    for (int i=0; i < num_pages; i++) {
        void *va = base_va+i*PGSIZE;
        unsigned long addr = (unsigned long)va-VADDR_BASE;

        int pd_index = get_top_bits(addr, PD_BITS);
        pte_t *page = get_page_entry((pte_t*)pd, pd_index);
        if (page == NULL) {
            printf("PTE allocation failed\n");
            return NULL;
        }

        // loop for each page level
        for (int level=0; level < 2; level++) {
            int index = get_mid_bits(addr, PT_BITS, OFFSET_BITS+PT_BITS*(2-i));
            page = get_page_entry(page, index);
            if (page == NULL) {
                printf("PTE allocation failed\n");
                return NULL;
            }
        }

        int last_index = get_mid_bits(addr, PT_BITS, OFFSET_BITS);
        if (get_page_entry(page, last_index) == 0) {
            void *ppage = find_next_ppage();
            if (ppage == NULL) {
                printf("Page allocation failed\n");
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

int main() {
    // a_malloc(PGSIZE*1024);
    // int count = 0;
    // for (int i=0; i < VPAGE_BITMAP_SIZE; i++) {
    //     for (int j=0; j < 8; j++) {
    //         if (get_bit_at_index(&pbitmap[i], j) == 1)
    //             count++;
    //     }
    // }
    // printf("%d\n", count);


    printf("%p\n", a_malloc(5000));
    printf("%p\n", a_malloc(5000));
    printf("%p\n", a_malloc(5000));
    printf("%d\n", PD_BITS);
    print_bitmap(pbitmap);
}