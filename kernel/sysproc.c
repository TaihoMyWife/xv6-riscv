#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "inttypes.h"
uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}
uint64
sys_sched_static(void)
{
  sched_statistics();
  printf("function over");
  return 0;
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
  argaddr(0, &p);
  //printf("uint64: %" PRIu64 "\n", p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
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

  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  //printf("ticks0:%u \n", ticks0);
  while(ticks - ticks0 < n){
    if(killed(myproc())){
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

  argint(0, &pid);
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
sys_info(void)
{
  int n;
  argint(0, &n); //get syscall arg
  return print_test(n);
  //return ;
}
uint64
sys_procinfo(void)
{
  uint64 p;
  argaddr(0, &p);
  //n = (struct pinfo*)p;
  //printf("[procinfo %d] ppid: %d, syscalls: %d, page usage: %d\n",
  //                      52, n->ppid, n->syscall_count, n->page_usage);
  //argaddr(0, &n); //get syscall arg
  //n = addr;
  proc_info(p);
  return 0;
}
