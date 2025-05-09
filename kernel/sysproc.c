#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  // 从寄存器a0获取系统调用的形参，放入proc结构体中的traceMask掩码
  argint(0, &(myproc()->traceMask)); //0：寄存器a0
  return 0;
}

uint64
sys_sysinfo()
{
  uint64 addrbuf; // 缓冲区，接受来自寄存器a0的参数信息，寄存器a0保存用户空间下的参数：一个指向struct sysinfo的指针(来自用户空间) 
  if(argaddr(0, &addrbuf)<0)
  {
    printf("argaddr failed to get arg from trapframe->a0\n");
    return -1;
  }
  struct sysinfo sinfo;
  sinfo.freemem = getRestMem();
  sinfo.nproc = getProcNum();
  
  /* sysinfo需要将一个struct sysinfo复制回用户空间 */
  
  // 使用 copyout，结合当前进程的页表，获得进程传进来的指针（逻辑地址）对应的物理地址
  // 然后将 &sinfo 中的数据复制到该指针所指位置，供用户进程使用
  if(copyout(myproc()->pagetable, addrbuf, (char *)&sinfo, sizeof(sinfo)) < 0)
    return -1;
  return 0;
}
