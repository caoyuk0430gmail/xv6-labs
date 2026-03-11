#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h" 
#include "proc.h" 

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  // kernel initialize a pagetable for the top level PT (3-level) and zero all PTE
  kernel_pagetable = kvminit_pgt();
}

// lab pgt, wrap kvminit by generic pgt
/*
 * create a direct-map page table for the page table.
 */
pagetable_t
kvminit_pgt()
{
  // kernel initialize a pagetable for the top level PT (3-level) and zero all PTE
  pagetable_t pgt = (pagetable_t) kalloc();
  memset(pgt, 0, PGSIZE);
  // starts mapping all I/O devices one by one
  // uart registers
  // va is #define UART0 0x10000000L, it correspond to pa 0x10000000.
  kvmmap_pgt(pgt, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap_pgt(pgt, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap_pgt(pgt, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap_pgt(pgt, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap_pgt(pgt, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap_pgt(pgt, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap_pgt(pgt, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  return pgt;
}


// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // write satp register and enable pgtable using the kernel_pagetable created in kvminit
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  // Safety check: Don't allow lookups beyond the top of memory.
  if(va >= MAXVA)
    panic("walk");

  // Loop through Level 2 (Top) and Level 1 (Middle).
  // Level 0 (Leaf) is handled after the loop.
  for(int level = 2; level > 0; level--) {
    
    // --- STEP 1: Find the entry in the current directory ---
    // PX(level, va) extracts the 9 bits of index for this level.
    // &pagetable[...] gets the address of that slot.
    // CRITICAL: 'pagetable' is a pointer. In C, dereferencing it creates a VA access.
    pte_t *pte = &pagetable[PX(level, va)];

    // --- STEP 2: Is there a path down? ---
    if(*pte & PTE_V) {
      // YES: The entry is Valid (PTE_V is 1).
      
      // --- STEP 3: The "Magic" Link (The Quote!) ---
      // PTE2PA(*pte) extracts the raw PHYSICAL Address (e.g., 0x5555000) from the bits.
      // We cast it to (pagetable_t), effectively turning a PA into a VA.
      // This only works because of Identity Mapping (VA 0x5555000 maps to PA 0x5555000).
      pagetable = (pagetable_t)PTE2PA(*pte);
      
    } else {
      // NO: The entry is empty (Invalid).
      
      // If 'alloc' is 0, we give up (return null).
      // If 'alloc' is 1, we create a new page table page on the fly.
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      
      // kalloc() returns a Kernel Virtual Address (e.g., 0x9000).
      // We clean the new page with zeros.
      memset(pagetable, 0, PGSIZE);
      
      // We link the new page into the current directory.
      // PA2PTE converts the pointer (VA/PA) back into the physical bits for the PTE.
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  
  // Return the address of the final Leaf Entry (Level 0).
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// add a mapping to the generic page table.
void
kvmmap_pgt(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap_pgt");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  // lab pgt
  // kvmpa is used to translate stack addresses for disk I/O.
  //  Since the Kernel Stack is now mapped only in the process's private page table (and not the global one) via allocproc,
  // kvmpa must look in the right place
  pagetable_t kpgt = kernel_pagetable;
  struct proc *p = myproc();
  // If we are running a process, and that process has a kernel map...
  if(p != 0 && p->kpagetable != 0) {
    kpgt = p->kpagetable; // ...use the private map instead.
  }
  
  pte = walk(kpgt, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      // panic("uvmunmap: walk");
      continue;
    if((*pte & PTE_V) == 0)
      // panic("uvmunmap: not mapped");
      continue;
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// recursively walk through page-table pages to print PTEs.
void
vmprint_walk(pagetable_t pagetable, int level)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    // if it is a valid PTE entry
    if(pte & PTE_V) {
      // we first print the indentation
      for (int j = 0; j <= level; j++) {
         printf(" ..");
      }
      // Print the Index, PTE hex, and Physical Address hex
      // %p in xv6 handles the 0x prefix and zero-padding automatically
      printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));
      // Recurse deeper IF this PTE points to another Page Table.
      // In xv6/RISC-V, it is a directory if the R/W/X bits are ALL zero.
      // If any of R/W/X are set, it is a leaf (maps to actual memory), so stop.
      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
         // PTE2PA extracts the physical address of the next page table
        // We cast it to pagetable_t (which is just uint64*)
        uint64 child = PTE2PA(pte);
        vmprint_walk((pagetable_t)child, level + 1);
      }
    }
  }
}

void
vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprint_walk(pagetable, 0);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      // lab alloc fork calls uvmcopy
      // panic("uvmcopy: pte should exist");
      continue;
    if((*pte & PTE_V) == 0)
      // lab alloc fork calls uvmcopy
      // panic("uvmcopy: page not present");
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// read -> sys_read -> fileread -> piperead -> copyout
// write -> sys_write -> filewrite -> pipewrite -> copyin
// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    // make sure alloc before copy
    if (pa0 == 0) {
      if ((va0 >= p->sz) || (va0 < p->trapframe->sp)) {
        return -1;
      } else {
        pa0 = (uint64)kalloc();
        if (pa0 == 0) {
          p->killed = 1;
        } else {
          memset((void *)pa0, 0, PGSIZE);
          va0 = PGROUNDDOWN(va0);
          if(mappages(p->pagetable, va0, PGSIZE, pa0, PTE_W|PTE_R|PTE_U) != 0) {
            kfree((void *)pa0);
            p->killed = 1;
          }
        }
      }
    }
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    // make sure alloc before copy
    if (pa0 == 0) {
      if ((va0 >= p->sz) || (va0 < p->trapframe->sp)) {
        return -1;
      } else {
        pa0 = (uint64) kalloc();
        if (pa0 == 0) {
          p->killed = 1;
        } else {
          memset((void *)pa0, 0, PGSIZE);
          va0 = PGROUNDDOWN(va0);
          if(mappages(p->pagetable, va0, PGSIZE, pa0, PTE_W|PTE_R|PTE_U) != 0) {
            kfree((void *)pa0);
            p->killed = 1;
          }
        }
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
