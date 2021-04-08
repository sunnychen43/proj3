#include "my_vm.h"
#include <math.h>
#include <sys/mman.h>
#include <string.h>

#define ADDR_SIZE 8  // change when 32-bit

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


static pde_t *pd;
static block mem_blocks[NUM_BLOCKS];
static char *vbitmap;

char *find_next_ppage(char *mem, char *pbitmap) {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {
        if (pbitmap[i] != 255) {
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

void *find_next_vpage(char *vbitmap) {
    for (int i=0; i < PPAGE_BITMAP_SIZE; i++) {
        if (vbitmap[i] != 255) {
            for (int j=0; j < 8; j++) {
                if (get_bit_at_index(vbitmap+i, j) == 0) {
                    // set_bit_at_index(vbitmap+i, j);
                    size_t offset = PGSIZE * (i*8 + j);
                    return (void *)(0ULL+offset);
                }
            }
        }
    }
    return NULL;
}

/*
Function responsible for allocating and setting your physical memory 
*/
void init_block(block *block_ptr) {
    block_ptr->mem = mmap(NULL, MEMSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    block_ptr->pbitmap = malloc(PPAGE_BITMAP_SIZE);
    memset(block_ptr->pbitmap, 0, PPAGE_BITMAP_SIZE);
}

void init_pt(char *mem) {
    memset(mem, 0, PGSIZE);
}

void set_physical_mem() {
    vbitmap = malloc(VPAGE_BITMAP_SIZE);
    memset(vbitmap, 0, VPAGE_BITMAP_SIZE);

    pd = malloc(ADDR_SIZE*(int)pow(2, PD_BITS));
    memset(pd, 0, ADDR_SIZE*(int)pow(2, PD_BITS));

    init_block(&mem_blocks[0]);
    pd[0] = (pde_t)find_next_ppage(mem_blocks[0].mem, mem_blocks[0].pbitmap);
    find_next_vpage(vbitmap);
    page_map(pd, 0x0, (void*)pd[0]);
    // *(pte_t*)pd[0] = pd[0];  // set first entry of page to refer to itself
    printf("%p %p\n", *(pte_t*)pd[0], mem_blocks[0].mem);
}


/*
The function takes a virtual address and page directories starting address and
performs translation to return the physical address
*/
pte_t *translate(pde_t *pgdir, void *va) {
    /* Part 1 HINT: Get the Page directory index (1st level) Then get the
    * 2nd-level-page table index using the virtual address.  Using the page
    * directory index and page table index get the physical address.
    *
    * Part 2 HINT: Check the TLB before performing the translation. If
    * translation exists, then you can return physical address from the TLB.
    */


    //If translation not successfull
    return NULL; 
}


/*
The function takes a page directory address, virtual address, physical address
as an argument, and sets a page table entry. This function will walk the page
directory to see if there is an existing mapping for a virtual address. If the
virtual address is not present, then a new entry will be added
*/
int page_map(pde_t *pgdir, void *va, void *pa) {
    unsigned int pd_index = get_top_bits((unsigned int)va, PD_BITS);
    unsigned int pt_index = get_mid_bits((unsigned int)va, PT_BITS, OFFSET_BITS);

    int pte_per_page = PGSIZE / ADDR_SIZE;
    int index = pd_index * pte_per_page + pt_index;
    
    int bit = get_bit_at_index(&vbitmap[index/8], index%8);
    if (bit == 1) {
        return -1;
    }
    else {
        *(pte_t*)pgdir[pd_index] = pa;
    }
    return 0;
}


/*Function that gets the next available page
*/
void *get_next_avail(int num_pages) {
 
    //Use virtual address bitmap to find the next free page
}


/* Function responsible for allocating pages
and used by the benchmark
*/
void *a_malloc(unsigned int num_bytes) {

    /* 
     * HINT: If the physical memory is not yet initialized, then allocate and initialize.
     */

   /* 
    * HINT: If the page directory is not initialized, then initialize the
    * page directory. Next, using get_next_avail(), check if there are free pages. If
    * free pages are available, set the bitmaps and map a new page. Note, you will 
    * have to mark which physical pages are used. 
    */

    return NULL;
}

/* Responsible for releasing one or more memory pages using virtual address (va)
*/
void a_free(void *va, int size) {

    /* Part 1: Free the page table entries starting from this virtual address
     * (va). Also mark the pages free in the bitmap. Perform free only if the 
     * memory from "va" to va+size is valid.
     *
     * Part 2: Also, remove the translation from the TLB
     */
     
    
}


/* The function copies data pointed by "val" to physical
 * memory pages using virtual address (va)
*/
void put_value(void *va, void *val, int size) {

    /* HINT: Using the virtual address and translate(), find the physical page. Copy
     * the contents of "val" to a physical page. NOTE: The "size" value can be larger 
     * than one page. Therefore, you may have to find multiple pages using translate()
     * function.
     */




}


/*Given a virtual address, this function copies the contents of the page to val*/
void get_value(void *va, void *val, int size) {

    /* HINT: put the values pointed to by "va" inside the physical memory at given
    * "val" address. Assume you can access "val" directly by derefencing them.
    */




}



/*
This function receives two matrices mat1 and mat2 as an argument with size
argument representing the number of rows and columns. After performing matrix
multiplication, copy the result to answer.
*/
void mat_mult(void *mat1, void *mat2, int size, void *answer) {

    /* Hint: You will index as [i * size + j] where  "i, j" are the indices of the
     * matrix accessed. Similar to the code in test.c, you will use get_value() to
     * load each element and perform multiplication. Take a look at test.c! In addition to 
     * getting the values from two matrices, you will perform multiplication and 
     * store the result to the "answer array"
     */

       
}

int main() {
    set_physical_mem();
    return 0;
}