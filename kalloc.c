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

void initialize_bitmap()
{
    bit_map = (uint *)kalloc();
    if (!bit_map)
        panic("Failed to allocate memory for swap bit_map");
    memset(bit_map, 0, PGSIZE);
}

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
  if(!r && reclaim() > -1)
	  goto try_again;

  if(r) {
    kmem.freelist = r->next;
  } else {
    if(kmem.use_lock)
      release(&kmem.lock);
    if(reclaim() <= -1){
      cprintf("Out of memory\n");
      return 0;
    }
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

// Victim 페이지를 선택하는 함수
struct page* select_victim_page() {
  struct page* p = page_lru_head->next;
  if (p == page_lru_head) { //lru list is empty
    return 0;
  }
  
  while (1) {
    if (p == page_lru_head) { 
      if (p->next == page_lru_head) {
        return 0; // 적합한 Victim 페이지가 없음
      }
      p = p->next;
    }

    pte_t* pte = walkpgdir(p->pgdir, p->vaddr, 0);

    // PTE가 유효하지 않거나 사용자 접근 불가능한 페이지는 건너뜀
    if (!pte || !(*pte & PTE_U)) {
      p->prev->next = p->next;
      p->next->prev = p->prev;
      p->prev = p->next = 0;

      p->pgdir = 0;
      p->vaddr = 0;

      num_lru_pages--;

      p = p->next;
      continue;
    }

    // Accessed 비트가 클리어된 페이지를 Victim으로 선택
    if (!(*pte & PTE_A)) {
      return p;
    }

    // Accessed 비트를 초기화
    *pte &= ~PTE_A;

    // LRU 리스트에서 페이지를 끝으로 이동
    p->prev->next = p->next;
    p->next->prev = p->prev;
    page_lru_head->prev->next = p;
    p->prev = page_lru_head->prev;
    p->next = page_lru_head;
    page_lru_head->prev = p;

    p = p->next;
  }

  return 0; 
}

// 비트맵에서 사용 가능한 비트를 찾는 함수
int find_free_bit(int start_index, int mask) {
  for (int bit = 0; bit < 32; bit++) {
    if (!(mask & (1 << bit))) {
      return start_index + bit;
    }
  }
  return -1; // 사용 가능한 비트를 찾지 못함
}

// 비트맵에 비트를 설정하는 함수
void set_bit_in_bitmap(int bit_index) {
  int array_index = bit_index / 32;
  int bit_offset = bit_index % 32;
  bit_map[array_index] |= (1 << bit_offset);
}

// 비트맵에서 사용 가능한 인덱스를 반환하는 메인 함수
int bitmap_index() {
  acquire(&lock_of_bitmap);

  const int blocks_per_page = PGSIZE / BSIZE;
  uint full_mask = 0xffffffff;

  for (int i = 0; i < SWAPMAX / blocks_per_page; i += 32) {
    if (bit_map[i / 32] == full_mask) {
      continue; // 해당 블록이 모두 사용 중
    }

    int free_bit = find_free_bit(i, bit_map[i / 32]);
    if (free_bit != -1) {
      set_bit_in_bitmap(free_bit);
      release(&lock_of_bitmap);
      return free_bit;
    }
  }

  release(&lock_of_bitmap);
  return -1; // 사용 가능한 블록이 없음
}



void remove_page_from_lru(struct page* p) {
  p->prev->next = p->next;
  p->next->prev = p->prev;
  p->prev = p->next = 0;
  num_lru_pages--;
}

int swap_out_page(struct page* victim) {
  pte_t* pte = walkpgdir(victim->pgdir, victim->vaddr, 0);
  if (!pte) return -1;

  uint pa = PTE_ADDR(*pte);
  int swap_slot = bitmap_index(); 
  if (swap_slot == -1) return -1;

  swapwrite((char*)P2V(pa), swap_slot); 
  kfree((char*)P2V(pa));              

  // PTE 업데이트
  *pte = (swap_slot << 12) | (PTE_FLAGS(*pte) & 0xfff);
  *pte &= ~PTE_P; // Present 비트 제거
  *pte |= PTE_S;  // 스왑 상태 비트 설정

  return 0;
}

// reclaim 메인 함수
int reclaim() {
  acquire(&lru_lock);

  struct page* victim = select_victim_page();
  if (!victim) {
    release(&lru_lock);
    return -1; // Victim 페이지를 찾지 못함
  }

  remove_page_from_lru(victim); // LRU 리스트에서 제거
  release(&lru_lock);

  return swap_out_page(victim); // Victim 페이지를 스왑 아웃
}



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

  //lru push : page is first element in lru push
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
}


void bitmap_free(int offset){
  acquire(&lock_of_bitmap);

  int bit_map_index = offset/32;
  int temp = 1 << (offset%32);

  bit_map[bit_map_index] &= ~temp; //set 0

  release(&lock_of_bitmap);
}