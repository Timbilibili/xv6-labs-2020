#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"


// 声明新函数原型
int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);

/*
 * the kernel's Global root page table.
 */
pagetable_t kernel_pagetable;
extern char etext[];      // kernel.ld sets this to end of kernel code.
extern char trampoline[]; // trampoline.S
void kvm_free_kernelpgtbl(pagetable_t);
pagetable_t kvminit_newpgtbl();
void kvmmap(pagetable_t, uint64, uint64, uint64, int);

// 将 src 页表的一部分页映射关系拷贝到 dst 页表中。
// 只拷贝页表项映射关系，不拷贝实际的物理页内存内容。
// 成功返回0，失败返回 -1
int kvmcopymapping(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint   flag;

  // PGROUNDUP: prevent re-mapping already mapped pages (eg. when doing growproc)
  for (i = PGROUNDDOWN(start); i < start + sz; i += PGSIZE)
  {
    if ((pte = walk(src, i, 0)) == 0)
    {
      panic("kvmcopymapping: pte should not exist");
    }
    if ((*pte & PTE_V) == 0) // 读取内容（*pte）与PTE_V相与无效时
    {
      panic("kvmcopymapping: page not present");
    }
    pa = PTE2PA(*pte); // 获得pte的物理地址pa

    // `& ~PTE_U` 表示将该页的权限设置为非用户页
    // 必须设置该权限，RISC-V 中内核是无法直接访问用户页的
    flag =  PTE_FLAGS(*pte) & ~PTE_U;

    if (mappages(dst, i, PGSIZE, pa, flag) != 0) // 将上文获得的pte的物理地址pa与va匹配
    {
      goto err;
    }
  }
  
  return 0;
err:
  uvmunmap(dst, PGROUNDUP(start), (i - PGROUNDUP(start)) / PGSIZE, 0);
  return -1;
}

// 与 uvmdealloc 功能类似，将程序内存从 oldsz 缩减到 newsz。但区别在于不释放实际内存
// 用于内核页表内程序内存映射与用户页表程序内存映射之间的同步
uint64 kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
  {
    return oldsz;
  }
  if (PGROUNDDOWN(newsz) < PGROUNDDOWN(oldsz))
  {
    int npages = (PGROUNDDOWN(oldsz) - PGROUNDDOWN(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDDOWN(newsz), npages, 0);
  }
  
  return newsz;
}

/**
 * @description: proc独立内核页表创建函数：
 * 内核需要依赖内核页表内一些固定的映射的存在才能正常工作，
 * 我们为自己创立的内核页表添加这些依赖
 * @return {*}
 */
void kvm_map_pagetable(pagetable_t pgtbl)
{

  // uart registers
  kvmmap(pgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(pgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  //kvmmap(pgtbl, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(pgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(pgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(pgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

/**
 * @description: 创建新的内核页表，并添加固定依赖
 * @return 新创建的proc独立内核页表
 */
pagetable_t kvminit_newpgtbl()
{
  pagetable_t pgtbl = (pagetable_t)kalloc(); // 创建物理页表
  memset(pgtbl, 0, PGSIZE);
  kvm_map_pagetable(pgtbl); // 为自己创建的pgtbl添加固定依赖，否则页表无法正常使用

  return pgtbl; // 返回新创建的内核页表
}

/*
 * create a direct-map page table for the kernel.全局的内核页表
  init 本意为给内核页表添加固定依赖
 */
void kvminit()
{
  // 仍然需要有全局的内核页表，用于内核 boot 过程，以及无进程在运行时使用。
  kernel_pagetable = kvminit_newpgtbl(); // kernel_pagetable = pgtbl内核页表
  kvmmap(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_W | PTE_R);
}

/**
 * @description:递归释放一个内核页表中的所有 mapping，但是不释放其指向的物理页
 * @param {pagetable_t} pagetable
 * @return {*}
 */
void kvm_free_kernelpgtbl(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    uint64 child = PTE2PA(pte);
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0)
    { // 如果该页表项指向更低一级的页表
      // 递归释放低一级页表及其页表项
      kvm_free_kernelpgtbl((pagetable_t)child);
      pagetable[i] = 0;
    }
  }
  kfree((void *)pagetable); // 释放当前级别页表所占用空间
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
// after xv6 reload the satp, reflush the TLB
// 使用此函数前：（必须）reload satp，reflush the TLB
void kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// 在页表pagetable中，返回符合va的pte地址
//
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
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--)
  {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V)
    {
      pagetable = (pagetable_t)PTE2PA(*pte);
    }
    else
    {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
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

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
// 将逻辑地址 va 映射到物理地址 pa（添加第一个参数 pgtbl）
void kvmmap(pagetable_t pgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(pgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
// 将内核逻辑地址 va 转为物理地址 pa 并return pa
uint64
kvmpa(pagetable_t pgtbl, uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(pgtbl, va, 0);
  if (pte == 0)
    panic("kvmpa");
  if ((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa + off;
}

// 在页表pagetable下，在va处创建PTEs，然后将物理地址pa与逻辑地址va匹配起来，成功return 0 否则 -1
//
// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for (;;)
  {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// 释放 pagetable 页表下，从va开始的n个页面（npages）的mapping，释放物理内存
//
// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE)
  {
    if ((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if ((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if (PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if (do_free)
    {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
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
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if (sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W | PTE_R | PTE_X | PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE)
  {
    mem = kalloc();
    if (mem == 0)
    {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W | PTE_X | PTE_R | PTE_U) != 0)
    {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// 将用户页表 pagetable 取消分配，缩减进程内存从 oldsz 缩减至 newsz
//
// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz))
  {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// 释放物理内存页，且所有的页分配mapping需提前被释放
//
// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  // 递归释放每个PTEs的物理内存
  // 横向遍历
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) // &：按bit位与操作
    {
      // this PTE points to a lower-level page table.
      // 进入递归，纵向遍历
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      // 递归结束，遍历512个PTE置零
      pagetable[i] = 0;
    }
    else if (pte & PTE_V)
    {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

/**
 * @description: lxk'fun to print the stuff of pagetable
 * @param {pagetable_t} pagetable
 * @param {int} depth 控制递归深度，也就是往下递归页表的深度
 * @return {*}
 */
void pgtblprint(pagetable_t pagetable, int depth)
{
  // 512个PTEs：9个bit位控制页表pagetable中PTE的地址，2 ^ 9 = 512 PTEs
  for (int i = 0; i < 512; i++)
  {
    pte_t pte = pagetable[i];
    if (pte & PTE_V) // PTE（页表项）有效时
    {
      // 按格式打印
      printf("..");
      // depth控制递归深度，也就是“ ..”的数量，depth为 1 时，“ ..”数量为 1
      for (int j = 0; j < depth; j++)
      {
        printf(" ..");
      }
      printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));

      // 如果该节点不是叶节点，递归打印其子节点。
      if ((pte & (PTE_R | PTE_W | PTE_X)) == 0)
      {
        // this PTE points to a lower-level page table.
        // 读取PTE内容，即是下一个页表地址
        // child 指向下一级的页表
        pagetable_t child = PTE2PA(pte);
        pgtblprint((pagetable_t)child, depth + 1);
      }
    }
  }
  return 0;
}
// 打印 pagetable 内容
int vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  return pgtblprint(pagetable, 0);
}

// Free user memory pages,
// then free page-table pages.
void uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE)
  {
    if ((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if ((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0)
    {
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
void uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while (len > 0)
  {
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table--user pagetable.
// Return 0 on success, -1 on error.
// srcva在pagetable页表中寻址，利用walkaddr寻址到该srcva对应的物理地址pa0
int copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  // uint64 n, va0, pa0;

  // while (len > 0)
  // {
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0);
  //   if (pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if (n > len)
  //     n = len;
  //   memmove(dst, (void *)(pa0 + (srcva - va0)), n); // 这里操作的是真实物理内存pa

  //   len -= n;
  //   dst += n;
  //   srcva = va0 + PGSIZE;
  // }
  // return 0;

  return copyin_new(pagetable, dst, srcva, len);

}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table--user page,
// until a '\0', or max.
// Return 0 on success, -1 on error.
// srcva在pagetable页表中寻址，利用walkaddr寻址到该srcva对应的物理地址pa0
int copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  // uint64 n, va0, pa0;
  // int got_null = 0;

  // while (got_null == 0 && max > 0)
  // {
  //   va0 = PGROUNDDOWN(srcva);
  //   pa0 = walkaddr(pagetable, va0); // 找到srccva对应的physical address(pa0)
  //   if (pa0 == 0)
  //     return -1;
  //   n = PGSIZE - (srcva - va0);
  //   if (n > max)
  //     n = max; // 以max为准，max限制copyinstr的最大字符数

  //   char *p = (char *)(pa0 + (srcva - va0)); // 字符指针p指向地址(pa0 + (srcva - va0))
  //   while (n > 0)
  //   {
  //     if (*p == '\0') // *p:访问p指针指向的地址(pa0 + (srcva - va0))，读取该地址的内容
  //     {
  //       *dst = '\0'; // 判断遇到了终止符浩'\0'
  //       got_null = 1;
  //       break;
  //     }
  //     else
  //     {
  //       *dst = *p;
  //     }
  //     --n;
  //     --max;
  //     p++;
  //     dst++;
  //   }

  //   srcva = va0 + PGSIZE;
  // }
  // if (got_null)
  // {
  //   return 0;
  // }
  // else
  // {
  //   return -1;
  // }

  return copyinstr_new(pagetable, dst, srcva, max);

}

// 将 src 页表的一部分页映射关系拷贝到 dst 页表中。
// 只拷贝页表项，不拷贝实际的物理页内存。
// 成功返回0，失败返回 -1
// 将源页表(src)中从地址start开始的sz字节映射复制到目标页表(dst)
int kvmcopymappings(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz)
{
  pte_t *pte;    // walk返回的是pte的地址，所以采用pte * 指针
  uint64 pa, va; // va虚拟地址，即是对齐后的start
  uint64 flags;  // PTE的标志位

  // 1.页对齐处理：从PGROUNDDOwn(start)开始处理，避免重复映射已存在的页
  // 2.for循环遍历页面处理，虚拟地址 i 每次递增一个PGSIZE，递增一个页面
  for (va = PGROUNDDOWN(start); va < start + sz; va += PGSIZE)
  {
    // walk返回目标src页表内关于 va（i） 的PTE地址
    if ((pte = walk(src, va, 0)) == 0)
    {
      panic("kvmcopymapping: pte should exist");
    }
    if ((*pte & PTE_V) == 0)
    {
      panic("kvmcopymapping: page not present");
    }
    // 读取PTE地址，并转化为物理地址PA
    pa = PTE2PA(*pte);

    // 3.权限调整：移除用户可访问标志(PTE_U)，设置内核模式权限。
    // `& ~PTE_U` 表示将该页的权限设置为非用户页
    // 必须设置该权限，RISC-V 中内核是无法直接访问用户页的。
    flags = PTE_FLAGS(*pte) & ~PTE_U; // pte的标志位与PTE_U的相反 相与，则表明不是PTE_U

    // 4.va映射pa：在目标页表dst，将va（i）映射到pa上，标志位为flags，每次映射一个页面大小PGSIZE
    if (mappages(dst, va, PGSIZE, pa, flags) != 0)
    {
      // 5.错误回滚
      goto err;
    }
  }

  return 0;

err: // 如遇到映射错误则进行回滚，将dst页表内已经映射的部分进行撤回，
     // 从对齐的start地址开始一直到for循环中+=PGSIZE递增了好几次PGSIZE的va为止，
     // 这期间的地址除以PGSIZE得到要撤回的几个页，也就是npage，进行撤回
  uvmunmap(dst, PGROUNDDOWN(start), (va - PGROUNDDOWN(start) / PGSIZE), 0);

  return -1;
}
