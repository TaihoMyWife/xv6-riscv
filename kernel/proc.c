#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "inttypes.h"
//#include "random.h"
struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;
struct run {
  struct run *next;
};
int nextpid = 1;
struct spinlock pid_lock;
extern struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;
extern void forkret(void);
static void freeproc(struct proc *p);
extern int syscall_count;
extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;
int random_at_most(int max);
// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

unsigned short lfsr = 0xACE1u;
unsigned short bit;
unsigned short rand(){
  bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1;
  return lfsr = (lfsr >> 1) | (bit << 15);
}
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->syscall_count=0;
  p->state = USED;
  p->tickets = 10;
  p->ticks = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->syscall_count = 0;
  p->state = UNUSED;
  p->ticks = 0;
  p->tickets =0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate(); //apply for a new page
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;
  p->syscall_count = 0;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  #if defined(LOTTERY)
  np->tickets = 10;
  np->ticks = 0;
  #endif
  #if defined(STRIDE)
  np->tickets = 10000;
  np->stride = 10000/10000;
  np->pass = p->pass;
  np->ticks = 0;
  #endif
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);
        
	// check that this is a child proess, not simply a thread
        if (p->pgdir == p->parent->pgdir)
        continue;

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  /*
  static _Bool have_seeded = 0;
  const int seed = 1323;
	if(!have_seeded)
	{
		srand(seed);
		have_seeded = 1;
	}
 */
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
 #if defined(LOTTERY)
    int total_tickets = 0;
    int currentTicketCount = 0;
    int lottery = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      //acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // get all runable process to get total ticktets
        // to release its lock and then reacquire it.
        total_tickets += p->tickets;
      }
    }
    lottery = rand()%total_tickets;
    total_tickets = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // get all runable process to get total ticktets 
        // to release its lock and then reacquire it.
	total_tickets += p->tickets;
      if(total_tickets>=lottery){
	  //printf("the result of lottery pid is %d p tickets is %d\n",p->pid,p->tickets);
	  p->state = RUNNING;
	  p->ticks++;
          c->proc = p;
          currentTicketCount++;
          swtch(&c->context, &p->context);
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
	  release(&p->lock);
	  break;
        }
      }
      release(&p->lock);
    }
#endif
#if defined(STRIDE)
    uint min_pass = 999999;
    int pid = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state == RUNNABLE) {
	  if(p->pass < min_pass){
	    min_pass=p->pass;
	    pid=p->pid;
	  }
      }
    }
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE && p->pid == pid) {
	  p->state = RUNNING;
          p->ticks++;
	  p->pass+=p->stride;
          c->proc = p;
          //currentTicketCount++;
          swtch(&c->context, &p->context);
          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc = 0;
          release(&p->lock);
          break;
        }
      release(&p->lock);
      }
      //release(&p->lock);
#endif
  }
  
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int print_test(int n)
{
  struct proc *p; int k=64;
  if(n==0){
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state == UNUSED) {
        k--;
      }
    }
#ifdef DEBUG
  printf("current processes: %d\n", k);
#endif
  return k;
  }
  if(n==1){
#ifdef DEBUG
	  printf("current syscalls: %d\n", syscall_count);
#endif
	  return syscall_count;
  }
  if(n==2){
	  int sum =0;
	  struct run *r;
          r = kmem.freelist;
          while(r){
          	r = r->next;
		sum++;
	  }
#ifdef DEBUG
	  printf("current free mempage: %d\n", sum);
#endif
	  return sum;
  }
  return -1;
}
int proc_info(uint64 addr){
	struct proc *p = myproc();
        struct pinfo pf;
	//printf("uint64 FUNC: %" PRIu64 "\n", addr);
	pf.ppid = p->parent->pid;	
	pf.syscall_count = p->syscall_count;
#if defined(LOTTERY)
	printf("current syscalllottry  mempage size: %d\n", p->sz);
#endif

#ifdef DEBUG
	printf("current syscall 123  mempage size: %d\n", p->sz);
#endif
	pf.page_usage = (p->sz+PGSIZE-1)/PGSIZE;
	if(copyout(p->pagetable, addr, (char *)&pf, sizeof(pf)) < 0)
        	return -1;
	return 0;
}
#if defined(LOTTERY)
int sched_statistics(void)
{
  struct proc *p;
 // struct cpu *c = mycpu();
 // c->proc = 0;
    // Avoid deadlock by ensuring that devices can interrupt.
    
    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state!=UNUSED) {
	 //acquire(&p->lock);
         printf("%d(%s): tickets: %d, ticks:%d \n", p->pid, p->name,p->tickets,p->ticks);
	 //release(&p->lock);
      }
      
      //acquire(&p->lock);
     /* if(p->state != UNUSED){
	//uint k=ticks++;
        printf("%d(%s): tickets: xxx, ticks: \n", p->pid, p->name);
      }*/
    }
  return 0;
}
#endif

#if defined(STRIDE)
int sched_statistics(void)
{
  struct proc *p;
 // struct cpu *c = mycpu();
 // c->proc = 0;
    // Avoid deadlock by ensuring that devices can interrupt.

    for(p = proc; p < &proc[NPROC]; p++) {
      if(p->state!=UNUSED) {
         //acquire(&p->lock);
         printf("%d(%s): tickets: %d, ticks:%d pass:%d stride:%d\n", p->pid, p->name,p->tickets,p->ticks,p->pass,p->stride);
         //release(&p->lock);
      }

      //acquire(&p->lock);
     /* if(p->state != UNUSED){
        //uint k=ticks++;
        printf("%d(%s): tickets: xxx, ticks: \n", p->pid, p->name);
      }*/
    }
  return 0;
}
#endif

#if defined(LOTTERY)
int sched_tickets(int tickets)
{
  if(tickets < 0|| tickets>10000){
    printf("invalid tickets");
    return -1;
  }
  struct proc *p = myproc();
  acquire(&p->lock);
  p->tickets = tickets;
  release(&p->lock);
#ifdef DEBUG
  printf("tickets changed to : xxx, ticks: \n", p->pid, p->name);
#endif
  return 0;
}	
#endif
#if defined(STRIDE)
int sched_tickets(int tickets)
{
  if(tickets < 0|| tickets>10000){
    printf("invalid tickets");
    return -1;
  }
  struct proc *p = myproc();
  acquire(&p->lock);
  p->tickets = tickets;
  p->stride = 10000/tickets;
  release(&p->lock);
#ifdef DEBUG
  printf("tickets changed to : xxx, ticks: \n", p->pid, p->name);
#endif
  return 0;
}
#endif

int clone(void* stack) {
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  // check if stack is not null
  if (stack == 0){
    return -1;
  }
  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  // Use same page tables and process state from parent
  np->pgdir = p->pgdir;
  np->sz = p->sz;
  np->parent = p;
  *np->trapframe = *p->trapframe;

  // Clear %eax so that clone returns 0 in the child.
  //np->trapframe->eax = 0;

  // Create stack and call copyout to ensure page table entries reflect
  // the changes in memory
  int ustack[3];
  //ustack[0] = 0xFFFFFFFF;
  //ustack[1] = (uint)arg1;
  //ustack[2] = (uint)arg2;
  uint usp = (uint)stack + PGSIZE - sizeof(ustack);
  if (copyout(np->pgdir, usp, ustack, sizeof(ustack)) < 0)
    return -1;

  // Setup new user stack and  and eip
  np->trapframe->sp = (uint)usp;
  //np->trapframe->eip = (uint)fcn;

  // TOOD: indicate process has another thread

  // Save address of stack so that is can be freed later
  np->ustack = stack;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

int
join(void** stack)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;

      // check that this is a thread, not simply a child process
      if (p->pgdir != p->parent->pgdir)
        continue;

      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;

        // copy wait() but don't call kfree and freevm which frees page tables and all
        // physical memory pages that is shared by threads.
        //  kfree(p->kstack);
        //  p->kstack = 0;

        //freevm(p->pgdir);
        stack = p->ustack;
        p->ustack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

