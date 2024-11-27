// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "fs.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
struct page dummy_page;
int num_free_pages;
int num_lru_pages;
int has_released = 0;

struct spinlock lock_of_bitmap;
struct spinlock lru_lock;
uint *bit_map;
// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

//initialize 1
void initialize_bitmap()
{
    bit_map = (uint *)kalloc();
    if (!bit_map)
        panic("Failed to allocate memory for swap bit_map");
    memset(bit_map, 0, PGSIZE);
}

//initalize 2
void initialize_lru_list()
{
    page_lru_head = &dummy_page;
    page_lru_head->prev = page_lru_head;
    page_lru_head->next = page_lru_head;
}

void clear_accessed_bit(struct page *page, pte_t *pte) {
    *pte &= ~PTE_A; // Accessed 비트를 0으로 초기화

    // LRU 리스트 끝으로 이동
    page->prev->next = page->next;
    page->next->prev = page->prev;

    page_lru_head->prev->next = page;
    page->prev = page_lru_head->prev;
    page_lru_head->prev = page;
    page->next = page_lru_head;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

try_again:
  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;

  if(!r){
    if(kmem.use_lock)
      release(&kmem.lock);
    if(reclaim() == 0) goto try_again;
    else{
      cprintf("kalloc: out of memory\n");
      return 0;
    }
  }
  kmem.freelist = r->next;
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}




//to find usable index 2
int find_free_bit(int start_index, int mask) {
  const int blocks_per_page = PGSIZE / BSIZE;
  for (int bit = 0; bit < 32; bit++) {
    if(bit + start_index >= SWAPMAX/blocks_per_page) break;

    if (!(mask & (1 << bit))) {
      bit_map[start_index / 32] |= (1 << bit);
      return start_index + bit;
    }
  }
  return -1;  //there is no index.
}

//to find usable index.
int bitmap_index() {
  acquire(&lock_of_bitmap);

  uint full_mask = 0xffffffff;

  for (int i = 0; i < SWAPMAX / (PGSIZE / BSIZE); i += 32) {
    if (bit_map[i / 32] == full_mask) { //all page is used
      continue; 
    }
    int free_bit = find_free_bit(i, bit_map[i / 32]);
    if(free_bit != -1){  //success to get free_bit
      release(&lock_of_bitmap);
      return free_bit;
    }
  }
  release(&lock_of_bitmap);
  return -1;
}

//move to the end.
void move_page_to_lru_tail(struct page *p){
  p->prev->next = p->next;
  p->next->prev = p->prev;

  page_lru_head->prev->next = p;
  p->prev = page_lru_head->prev;
  p->next = page_lru_head;
  page_lru_head->prev = p;
}

//swap out when pte_a in select_victim_page
int swap_out_victim_page(struct page* p, pte_t* pte) {
  p->prev->next = p->next;
  p->next->prev = p->prev;
  p->prev = p->next = 0;

  int swap_index = bitmap_index();
  if (swap_index < 0) {
    return -1; 
  }

  uint pa = PTE_ADDR(*pte);
  release(&lru_lock);

  swapwrite((char*)P2V(pa), swap_index);
  kfree((char*)P2V(pa));
  
  *pte &= ~PTE_P; 
  *pte &= 0xfff;  
  *pte |= (swap_index << 12); 
  *pte |= PTE_S; 

  return 0; 
}

// success :0 , fail : -1
int select_victim_page() {
  struct page* p = page_lru_head->next;
  if (p == page_lru_head) { //lru list is empty
    release(&lru_lock);
    return -1;
  }
  
  while (1) {
    if (p == page_lru_head) { 
      if (p->next == page_lru_head) {
        release(&lru_lock);
        return -1; // empty lru
      }
      p = p->next;
    }

    pte_t* pte = walkpgdir(p->pgdir, p->vaddr, 0);

    if ((*pte & PTE_U) == 0) {
      uint pa = PTE_ADDR(*pte);
      struct page *not_user = &pages[pa/PGSIZE];
      not_user->prev->next = not_user->next;
      not_user->next->prev = not_user->prev;
      not_user->prev = not_user->next = 0;

      not_user->pgdir = 0;
      not_user->vaddr = 0;

      num_lru_pages--;
      continue;
    }

    if(!(*pte & PTE_A)){
      int result = swap_out_victim_page(p, pte);
      if(result < 0){
        release(&lru_lock);
        return -1; //out of memory
      }

      return 0;
    }
    *pte &= ~PTE_A; //initialize access_bit

    move_page_to_lru_tail(p);
    p = p->next;
  }
}

// reclaim 메인 함수 : success 0, fail -1
int reclaim() {
  acquire(&lru_lock);

  int victim = select_victim_page();
  if (victim < 0) { //fail 
    return -1; // cannot find victim page
  }
  return 0; //success
}

//page_fault : execute swap_in function
void page_fault(uint va){
  swap_in(va);
}

void swap_in(uint va){
  pde_t *pgdir = myproc()->pgdir;
  pte_t *pte = walkpgdir(pgdir, (void *)va, 0);
  int offset = (PTE_ADDR(*pte) >> 12);
  bitmap_free(offset);  //set 0 in bit map
  char* new_page = kalloc();
  if(new_page == 0){
    panic("swap_in() : Out of memory!!\n");
  }
  swapread(new_page, offset);

  *pte = V2P(new_page) | PTE_FLAGS(*pte) | PTE_P;
  *pte &= ~PTE_S;
  add_to_lru(pgdir, (char*) va);
}

//lru push : page is first element in lru push
int
add_to_lru(pde_t* pgdir, char* va){
  pte_t *pte = walkpgdir(pgdir, va, 0);
  uint pa = PTE_ADDR(*pte);
  struct page *page = &pages[V2P(pa)/PGSIZE];
  page->pgdir = pgdir;
  page->vaddr = (char *)PGROUNDDOWN((uint) va);

  acquire(&lru_lock);

  page_lru_head->next->prev = page;
  page_lru_head->next = page;
  page->next = page_lru_head->next;
  page->prev = page_lru_head;
  
  num_lru_pages += 1;

  release(&lru_lock);
  return 0;
}

int lru_remove(pde_t *page_dir, char *virtual_addr) {
  pte_t *page_table_entry = walkpgdir(page_dir, virtual_addr, 0);
  uint physical_address = PTE_ADDR(*page_table_entry);
  struct page *current_page = &pages[physical_address / PGSIZE];

  acquire(&lru_lock);

  current_page->pgdir = 0;
  current_page->vaddr = 0;

  if (current_page->prev) {
    current_page->prev->next = current_page->next;
  }
  if (current_page->next) {
    current_page->next->prev = current_page->prev;
  }

  current_page->prev = current_page->next = 0;
  num_lru_pages--;

  release(&lru_lock);

  return 0; 
}

//clear bitmap with specific offset
void bitmap_free(int offset){
  acquire(&lock_of_bitmap);

  int bit_map_index = offset/32;
  int temp = 1 << (offset%32);

  bit_map[bit_map_index] &= ~temp; //set 0

  release(&lock_of_bitmap);
}