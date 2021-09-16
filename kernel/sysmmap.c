#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
 
uint64 sys_mmap(void)
{
   // we really be maxing out max syscall args
   int fd, prot, flags, off, len;
   uint64 start_va;
 
   if (argint(0, &fd) < 0 ||
       argint(1, &off) < 0 ||
       argint(2, &len) < 0 ||
       argaddr(3, &start_va) < 0 ||
       argint(4, &prot) < 0 ||
       argint(5, &flags) < 0)
       return -1; // same as 0xffffffff
 
   return mmap_map(myproc()->ofile[fd], off, len, start_va, prot, flags);
}
 
uint64 sys_munmap(void)
{
   uint64 va;
   int len;
 
   if (argaddr(0, &va) < 0 || argint(1, &len) < 0)
       return -1;
  
   // writing to fs, must do it in a transaction
   // or else kernel panic :shrug:
   begin_op();
   int retval = mmap_unmap(va, len);
   end_op();
   return retval;
}
