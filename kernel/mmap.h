#define MAP_SHARED  (1L << 0)
#define MAP_PRIVATE (1L << 1)
 
#define PROT_READ   (1L << 1)
#define PROT_WRITE  (1L << 2)
#define PROT_EXEC   (1L << 3)
 
struct mmap_vma
{
   struct proc *p;          // process owning this VMA
   uint64 va;              // virtual addr
   uint64 len;             // vma length
   int prot;               // page prot flags
   int flags;              // mapping flags
   struct file *f;         // file
   uint f_inum;            // file inode num
   uint f_offset;          // file offset
};
