#include "types.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "mmap.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
 
#define NULL 0
#define FAIL 0xFFFFFFFF
 
// 0b1110
#define PROT_MASK 0xE
#define PTE_DIRTY (1L << 7)
 
struct
{
   struct spinlock lk;         // VMA table lock
   struct mmap_vma tbl[NMMAP]; // array of VMAs
} vma_table;
 
// Why do we need a lock?
// Because if we get interrupted just before line 44 (vma->f = f),
// another process may try to use the same slot in the tbl.
//
// Editing the VMA once allocated without lock is not a problem
// since then we are guaranteed to be the only process that is using that slot
 
void mmapinit(void)
{
   initlock(&vma_table.lk, "vma_table");
}
 
// page fault handler
// returns:
// 0 - success
// 1 - mmap mapping not found
// 2 - error
int mmap_handlepgfault(uint64 va)
{
   // function must be called with a process context
   struct proc *p = myproc();
   if (p == NULL)
       return 2;
 
   // find corresponding mapping
   struct mmap_vma *vma;
   for (vma = vma_table.tbl; vma < vma_table.tbl + NMMAP; ++vma)
       if (p == vma->p && va >= vma->va && va < (vma->va + vma->len))
       { // mapping found!
           // find the corresponding page and offset
           uint64 pg_va = PGROUNDDOWN(va);
           uint offset = (pg_va - vma->va) + vma->f_offset;
           pte_t *pg_pte = walk(p->pagetable, pg_va, 0);
 
           // check other similar mappings
           struct mmap_vma *o_vma;
           if (vma->flags & MAP_SHARED)
               for (o_vma = vma_table.tbl; o_vma < vma_table.tbl + NMMAP; ++o_vma)
                   if (o_vma->f && o_vma != vma && o_vma->f_inum == vma->f_inum && o_vma->f_offset == vma->f_offset && o_vma->len >= vma->len)
                   { // found another mapping, get the physical page from it and return
                       pte_t *o_pte = walk(o_vma->p->pagetable, o_vma->va + (pg_va - vma->va), 0);
                       if ((*o_pte & PROT_MASK) == 0) // nevermind, invalid mapping
                           continue;
                       uint64 pg_pa = PTE2PA(*o_pte);
                       *pg_pte = PA2PTE(pg_pa) | vma->prot | PTE_V | PTE_U;
 
                       // success
                       return 0;
                   }
 
           // get new physical page, rewrite pa on PTE
           uint64 pg_pa = (uint64)kalloc();
           *pg_pte = PA2PTE(pg_pa) | PTE_V | PTE_U;
 
           // read from file
           ilock(vma->f->ip);
           if (readi(vma->f->ip, 1, pg_va, offset, PGSIZE) < 0)
               return 2;
           iunlock(vma->f->ip);
 
           // set PTE prot flags in this mapping and all other similar mappings
           *pg_pte |= vma->prot;
 
           if (vma->flags & MAP_SHARED)
               for (o_vma = vma_table.tbl; o_vma < vma_table.tbl + NMMAP; ++o_vma)
                   if (o_vma->f && o_vma != vma && o_vma->f_inum == vma->f_inum && o_vma->f_offset == vma->f_offset && o_vma->len >= vma->len)
                       (*walk(o_vma->p->pagetable, o_vma->va + (pg_va - vma->va), 0)) = PA2PTE(pg_pa) | o_vma->prot | PTE_U | PTE_V;
 
           // success
           return 0;
       }
 
   return 1;
}
 
// loop through the VMA table, find a free slot
static struct mmap_vma *vma_alloc(struct file *f)
{
   acquire(&vma_table.lk);
   struct mmap_vma *vma;
   for (vma = vma_table.tbl; vma < vma_table.tbl + NMMAP; ++vma) // search the table
       if (vma->f == NULL)
       { // found an empty slot!
           vma->f = f;
           release(&vma_table.lk);
           return vma;
       }
   release(&vma_table.lk);
   return NULL;
}
 
// free a VMA entry
//
// explicitly defined as a function as to emphasize
// the entry should no longer be used after setting file
// to NULL, as it is up for grabs by other mmap operations
static inline void vma_free(struct mmap_vma *vma)
{
   vma->p = NULL;
   fileclose(vma->f);
   vma->f = NULL;
}
 
// get the first page of a MAP_PRIVATE mapping
// returns physical address of page
static void *get_priv_firstpg(struct file *f, uint offset, uint length)
{
   void *firstpg = kalloc();
   ilock(f->ip);
   readi(f->ip, 0, (uint64)firstpg, offset, PGSIZE);
   iunlock(f->ip);
   return firstpg;
}
 
// find a similar mapping for MAP_SHARED, otherwise NULL
static void *find_shared_firstpg(struct file *f, struct mmap_vma *vma)
{
   struct mmap_vma *othervma;
   for (othervma = vma_table.tbl; othervma < vma_table.tbl + NMMAP; ++othervma)
       if (othervma->f && vma != othervma && othervma->f_inum == vma->f_inum && othervma->f_offset == vma->f_offset && othervma->len >= vma->len) // same file & offset
       {
           // check valid page
           if ((*walk(othervma->p->pagetable, othervma->va, 0) & PROT_MASK) == 0)
               continue;
           return (void *)walkaddr(othervma->p->pagetable, othervma->va);
       }
   return NULL;
}
 
// maps a file to memory
// returns the virtual address of where it is mapped
uint64 mmap_map(struct file *f, uint offset, uint length, uint64 start_va, int prot, int flags)
{
   if (f == NULL) // anonymous mappings not yet supported
       return FAIL;
 
   if (f->type != FD_INODE) // hello user seekable inodes only pls
       return FAIL;
 
   if (start_va != NULL) // start addr hints not yet supported
       return FAIL;
 
   // start by allocating a vma struct, fail if none free
   struct mmap_vma *vma;
   if ((vma = vma_alloc(f)) == NULL)
       return FAIL;
 
   // find our proc, fail if no proc?
   struct proc *p = myproc();
   if (p == NULL)
       return FAIL;
 
   vma->p = p;
   vma->f_offset = offset;
   vma->len = length;
   vma->prot = (prot & PROT_MASK); // prevent user from setting PTE flags other than R,W,X
   if (vma->prot == 0)
       vma->prot = PTE_R; // default readonly;
   vma->flags = flags;
 
   // find next available address
   //
   // From Figure 2.3 in the xv6-riscv book, the heap grows up...
   // ...all the way until it hits the trapframe. Using this...
   // ...information, we can just go down from the trapframe,...
   // ...growing downwards.
 
   uint64 top_va = MAXVA - (PGSIZE * 2); // top - trampoline - trapframe
   uint64 bottom_va = top_va;
   const uint64 len = PGROUNDUP(length); // round up, can't lose bytes
 
   // loop until we find enough pages
   while (top_va - bottom_va != len)
   {
       // find free top_va (check valid flag)
       pte_t *top_pte = walk(p->pagetable, top_va, 0);
       if ((*top_pte & PTE_V) != 0)
       {
           top_va -= PGSIZE;
           bottom_va = top_va;
           continue;
       }
 
       // keep moving bottom_va down until we reach the required length
       bottom_va -= PGSIZE;
 
       // have we intersected with heap?
       const uint64 brk = p->sz;
       if (bottom_va <= brk)
           return FAIL;
 
       // is bottom_va free? if not we reset the search
       pte_t *bottom_pte = walk(p->pagetable, bottom_va, 0);
       if ((*bottom_pte & PTE_V) != 0)
       {
           bottom_va -= PGSIZE;
           top_va = bottom_va;
       }
   }
 
   // we've found a valid va range
   vma->va = bottom_va;
 
   // dup file ref and set inode num
   filedup(f);
   ilock(f->ip);
   vma->f_inum = f->ip->inum;
   iunlock(f->ip);
 
   void *firstpg = NULL;
 
   // get private page
   if (flags & MAP_PRIVATE)
       firstpg = get_priv_firstpg(f, offset, length);
 
   // find a similar mapping, otherwise leave as NULL
   if (flags & MAP_SHARED)
       firstpg = find_shared_firstpg(f, vma);
 
   // leave R,W,X unset for lazy mmap
   if (mappages(p->pagetable, bottom_va, len, (uint64)firstpg, PTE_V | PTE_U) != 0)
       return FAIL;
 
   // set the first PTE to user perms, if firstpg is valid
   if (firstpg != NULL)
       (*walk(p->pagetable, bottom_va, 0)) |= vma->prot;
 
   // return the starting virtual address
   return bottom_va;
}
 
// frees a shared page, sync content to disk
// frees physical page if no other references to it
//
// va must be page aligned
static void free_shared_page(struct mmap_vma *vma, uint64 va)
{
   // we can use this opportunity to sync changes back to disk
   pte_t *pte = walk(vma->p->pagetable, va, 0);
   if ((*pte & PTE_DIRTY))
   { // only write back to disk if the page is dirty (has been written to)
       ilock(vma->f->ip);
       writei(vma->f->ip, 1, va, (va - vma->va) + vma->f_offset, PGSIZE);
       iunlock(vma->f->ip);
   }
 
   // now we check other similar mappings
   // starting to regret not using physical page ref counting
   // this looks a little slow...
   struct mmap_vma *o_vma;
   for (o_vma = vma_table.tbl; o_vma < vma_table.tbl + NMMAP; ++o_vma)
       if (o_vma->f && vma != o_vma && o_vma->f_inum == vma->f_inum && o_vma->f_offset == vma->f_offset && o_vma->len >= vma->len) // same file & offset
       {
           uint64 va_offset = va - vma->va;
           if ((*walk(o_vma->p->pagetable, o_vma->va + va_offset, 0) & PROT_MASK) != 0)
           { // do not free the physical page, there is another ref to it
               uvmunmap(vma->p->pagetable, va, 1, 0);
               return;
           }
       }
 
   // if we reach here, it means there are no other referenes
   // to the physical page, unmap the VMA and free the PA
   uvmunmap(vma->p->pagetable, va, 1, 1);
}
 
int mmap_unmap(uint64 va, uint length)
{
   if (va % PGSIZE != 0) // addr must be a multiple of PGSIZE
       return -1;
 
   // find mapping
   struct mmap_vma *vma;
   for (vma = vma_table.tbl; vma < vma_table.tbl + NMMAP; ++vma)
       if (vma->p == myproc() && va >= vma->va && va < (vma->va + vma->len))
       { // found mapping
           if (va != vma->va && PGROUNDUP((va + length)) != PGROUNDUP(vma->va + vma->len))
               return -1; // cannot punch a hole in mapping / end va out of range
 
           const uint pages = PGROUNDUP(length) / PGSIZE;
           uint pg;
           for (pg = 0; pg < pages; ++pg)
           {
               uint64 c_va = va + (PGSIZE * pg);
               pte_t *c_pte = walk(vma->p->pagetable, c_va, 0);
 
               if ((*c_pte & PROT_MASK) == 0)
               { // page not yet mapped anyway
                   uvmunmap(vma->p->pagetable, c_va, 1, 0);
                   continue;
               }
 
               if (vma->flags & MAP_SHARED)
               { // special free for shared pages
                   free_shared_page(vma, c_va);
                   continue;
               }
 
               // if we reach here, it's a private mapped page
               // just free the physical page and clear the PTE
               uvmunmap(vma->p->pagetable, c_va, 1, 1);
           }
 
           if (pages * PGSIZE == PGROUNDUP(vma->len))
           {
               vma_free(vma);
               return 0;
           }
 
           // regardless, we freed pages * PGSIZE bytes
           vma->len -= pages * PGSIZE;
           if (va == vma->va) // beginning freed, move start va
               vma->va += pages * PGSIZE;
 
           return 0;
       }
 
   return -1;
}
 
// force unmap all mappings for a process
void mmap_forceunmap(struct proc *p)
{
   struct mmap_vma *vma;
   for (vma = vma_table.tbl; vma < vma_table.tbl + NMMAP; ++vma)
       if (vma->p == p && vma->f)
           mmap_unmap(vma->va, vma->len);
}
 
static void vma_copy(struct mmap_vma *src, struct mmap_vma *dst)
{
   dst->f = src->f;
   dst->f_inum = src->f_inum;
   dst->f_offset = src->f_offset;
   dst->flags = src->flags;
   dst->len = src->len;
   dst->prot = src->prot;
   dst->va = src->va;
}
 
// copies mappings from p to np
int mmap_fork(struct proc *p, struct proc *np)
{
   struct mmap_vma *vma;
   for (vma = vma_table.tbl; vma < vma_table.tbl + NMMAP; ++vma)
       if (vma->p == p && vma->f)
       { // found a mapping belonging to old proc
           struct mmap_vma *n_vma = vma_alloc(vma->f);
           if (n_vma == NULL)
               return -1;
           n_vma->p = np;
 
           // copy VMA
           vma_copy(vma, n_vma);
 
           // new reference to file
           filedup(n_vma->f);
 
           // map our pages, leave all pages R,W,X unset for
           // lazy mmap for the new file as well :D
           if (mappages(np->pagetable, n_vma->va, n_vma->len, (uint64)0, PTE_V | PTE_U) != 0)
               return -1;
       }
   return 0;
}
