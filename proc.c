 #include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

//add manabu 10/1
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "buf.h"


#define FAULT_SIZE 30
#define BROKEN_BYTE 8
#define RAND_SIZE 4090

struct ptable_t *ptable;

static struct proc *initproc;

int nextpid = 1;
int test_pte = 0;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

//add manaubu
extern pde_t *kpgdir;
char *plist[100];
int plist_index = 0;
char *test;
int test_plocal = 0;

extern char stack[KSTACKSIZE];

struct cons_lk {
  struct spinlock lock;
  int locking;
};

extern struct cons_lk *cons;
uint count_scheduler __attribute__((__section__(".must_writable"))) = 0;
struct context *verify_scheduler __attribute__((__section__(".must_writable")));;

struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};

extern struct log log;

extern char *uselist[4096];
extern uint ticks;

extern struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

extern struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  // Linked list of all buffers, through prev/next.
  // head.next is most recently used.
  struct buf head;
} bcache;


void
pinit(void)
{
  //add manabu 11/02  
  if ((ptable = (struct ptable_t *)kalloc(ALLOC_KGLOBAL)) == 0) {
    panic("kalloc: ptable");
  }
    memset(ptable, 0, PGSIZE); //set UNUSED 0
  //
    
  initlock(&ptable->lock, "ptable");  
}

// Must be ca1lled with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  uint flag;

  acquire(&ptable->lock);

  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable->lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->kgflag = 0;
  release(&ptable->lock);
 
  //Allocate kernel stack by kalloc(ALLOC_PLOCAL)

  if (p->pid > 2) {
    flag = ALLOC_PLOCAL;
  }
  else {
    flag = ALLOC_KGLOBAL;
  }
  
  if((p->kstack = kalloc(flag)) == 0){    
    p->state = UNUSED;
    return 0;
  }
  
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;

  if (p->pid > 2) {
    switchkvm();
    setptew(myproc()->pgdir, (char *)p->kstack, PGSIZE, 1);
    switchuvm(myproc());
  }
  
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
 
  return p;
}

//add function by manabu

int alloc_test_local(struct proc *p) {

  if ((p->tl = (struct test_local *)kalloc(ALLOC_PLOCAL)) == 0) {
    return -1;
  }
  
  return 0;
}


//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm(ALLOC_KGLOBAL)) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable->lock);

  p->state = RUNNABLE;

  release(&ptable->lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;    

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  if (np->pid > 2) {
    switchkvm();
    setpter(curproc->pgdir, (char *)np->kstack, PGSIZE);
    switchuvm(curproc);
  }
  
  
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable->lock);

  np->state = RUNNABLE;

  //add alloc_test
  /*
  if (alloc_test_local(np) < 0) {
    panic("alloc_test_local");
  }

  np->tl->pid = np->pid;
  np->tl->ppid = np->parent->pid;
  */  
  //    
  release(&ptable->lock);


  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable->lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

void
exit_plocal(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose_plocal(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  /* begin_op(); */
  /* iput(curproc->cwd); */
  /* end_op(); */
  curproc->cwd = 0;
  
  if (!holding(&ptable->lock)) {
    acquire(&ptable->lock);
  }


  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;  
  mycpu()->ncli = 1; //Force the lock depth to 1 because of process local data.  
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable->lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        //kfree((char *)p->tl);
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->kgflag = 0;        
        p->killed = 0;        
        p->state = UNUSED;
        release(&ptable->lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable->lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable->lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable->lock);
    for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;

      switchuvm(p);      
      p->state = RUNNING;      
      swtch(&(c->scheduler), p->context);
      if (count_scheduler == 0) {
        verify_scheduler = c->scheduler;
        count_scheduler = 1;
      }
      //Random Fault Injection
      //fault_injection(p);
  
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable->lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable->lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable->lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first __attribute__((__section__(".should_writable"))) = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable->lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable->lock){  //DOC: sleeplock0
    acquire(&ptable->lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable->lock){  //DOC: sleeplock2
    release(&ptable->lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable->lock);
  wakeup1(chan);
  release(&ptable->lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable->lock);
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable->lock);
      return 0;
    }
  }
  release(&ptable->lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->state == UNUSED)
       continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
cps(void)
{
 
  struct proc *p;

  sti();
  acquire(&ptable->lock);    

  cprintf("pid \t ppid \t name \t tl->pid \t tl->ppid \t state \t\n");  
  for (p = ptable->proc; p < &ptable->proc[NPROC]; p++) {
    if (p->state == SLEEPING)
      cprintf("%d \t %d \t %s \t %d \t %d \t SLEEPING \t \n", p->pid, p->parent->pid, p->name, p->tl->pid, p->tl->ppid);
    else if (p->state == RUNNING)
      cprintf("%d \t %d \t %s \t %d \t %d \t RUNNING \t \n", p->pid, p->parent->pid, p->name, p->tl->pid, p->tl->ppid);
    else if (p->state == RUNNABLE) {
      cprintf("%d \t %d \t %s \t %d \t %d \t RUNNABLE \t \n", p->pid, p->parent->pid, p->name, p->tl->pid, p->tl->ppid);
      
    }
    else if (p->state == ZOMBIE) {
      cprintf("%d \t %d \t %s \t %d \t %d \t ZOMBIE \t \n", p->pid, p->parent->pid, p->name, p->tl->pid, p->tl->ppid);
      
    }    
  }  
  release(&ptable->lock);

  return 22;
}

int
plocal(void)
{

  //Protection violation test for file structure secured by sh process
  /*
  int i = 0;
  for (i = 0; i < 100; i++) {
    if (plist[i] != 0) {
      cprintf("DEBUG: plocal plist[%d] %x\n", i, plist[i]);
      //write : occur page falt
      *plist[i] = 0;
      //((struct file *)plist[i])->ref = 0;
      //((struct proc *)plist[i])->context->esi = 0;      
    }    
  } 
  */

  //test under consideration
  /*
  char *c1;
  struct proc *p = myproc();

  cprintf("DEBUG: process %s\n", p->name);
  c1 = (char *)kalloc(ALLOC_PLOCAL);

  cprintf("DEBUG: ALLOC_PLOCAL c1 addr %x\n", c1);
  //c2 = c1 + PGSIZE + PGSIZE + PGSIZE; //sh kerne stack memory 
  //cprintf("DEBUG: ALLOC_PLOCAL c2 addr %x\n", c2);
  *c1 = 0;
p  */  

  //protection violation test for kernel stack of other process
  /*
  struct proc *p;
  for (p = ptable->proc; p < &ptable->proc[NPROC]; p++) {
    if (p->state == SLEEPING) {
      p->context->eip = 0;
    }
  }
  */

  /*
  switchkvm();
  setptew(p->pgdir, c2, PGSIZE, 1);
  switchuvm(p);
  */

  
  /* test = kalloc(ALLOC_KGLOBAL); */
  /* cprintf("DEBUG: plocal test: %d\n", *test); */
  /* kfree((char *)test); */

  /* for (int i = 0; i < KSTACKSIZE; i++) { */
  /*   cprintf("%d %p\n", i, stack[i]); */
  /* } */  
  cprintf("scheduler :%p\n", cpus->scheduler);
  

  test_plocal = 1;
  return 23;  
}

int
plist_init(void)
{
  int i;
  cprintf("plist init!\n");
  for (i = 0; i < 100; i++) {
    plist[i] = 0;
  }
  return 24;
}


int
strcmp(const char *p, const char *q)
{
  while(*p && *p == *q)
    p++, q++;
  return (uchar)*p - (uchar)*q;
}

char*
strcpy(char *s, char *t)
{
  char *os;

  os = s;
  while((*s++ = *t++) != 0)
    ;
  return os;
}

int plocal_insert(char *p)
{ 

  //cprintf("DEBUG: plocal_insert &plist[%d] %x\n", i, &plist[i]);
  //cprintf("DEBUG: plocal_insert plist_index %d\n", plist_index);
  plist[plist_index] = p;
  cprintf("DEBUG: plocal insert index %d\n", plist_index);
  plist_index++;
  
  return 0;  
}

int verify_kglobal(struct proc *p) {
  int i = 0;
  struct proc *tmp_p;
  //stack
  /* for (i = 0; i < KSTACKSIZE; i++) { */
  /*   cprintf("stack[%d] %p\n", i, stack[i]); */
  /*   if (stack[i] < 0) */
  /*     return -1; */
  /* }   */
  
  //cpus
  if (mycpu()->scheduler != verify_scheduler)
    return -1;
  if (mycpu()->ts.ss0 != (SEG_KDATA << 3))
    return -1;
  if (mycpu()->ts.esp0 != ((uint)p->kstack + KSTACKSIZE))
    return -1;
  if (mycpu()->ts.iomb != (ushort)0xFFFF)
    return -1;  

  if (mycpu()->gdt[SEG_TSS].lim_15_0 != (sizeof(mycpu()->ts)-1))
    return -1;
  if (mycpu()->gdt[SEG_TSS].base_15_0 != ((uint)&mycpu()->ts & 0xffff))
    return -1;
  if (mycpu()->gdt[SEG_TSS].base_23_16 != (((uint)&mycpu()->ts >> 16 ) & 0xff))
    return -1;
  if (mycpu()->gdt[SEG_TSS].type != STS_T32B)
    return -1;
  if (mycpu()->gdt[SEG_TSS].s != 0)
    return -1;
  if (mycpu()->gdt[SEG_TSS].dpl != 0)
    return -1;
  if (mycpu()->gdt[SEG_TSS].p != 1)
    return -1;
  if (mycpu()->gdt[SEG_TSS].lim_19_16 != ((uint)(sizeof(mycpu()->ts)-1) >> 16))
    return -1;
  if (mycpu()->gdt[SEG_TSS].avl != 0)
    return -1;
  if (mycpu()->gdt[SEG_TSS].rsv1 != 0)
    return -1;
  if (mycpu()->gdt[SEG_TSS].db != 1)
    return -1;
  if (mycpu()->gdt[SEG_TSS].g != 0)
    return -1; 
  if ((mycpu()->gdt[SEG_TSS].base_31_24 != ((uint)(&mycpu()->ts) >> 24)))
    return -1;

  if (mycpu()->ncli < 0 || mycpu()->ncli > NPROC) {
    return -1;    
  }

  if (mycpu()->intena < 0)
    return -1;

  if (mycpu()->proc != p)
    return -1;

  //cons
  if (cons->locking != 0 && cons->locking != 1) {
    return -1;
  }

  if (cons->lock.locked != 0 && cons->lock.locked != 1) {
    return -1;
  }
    
  //ptable
  if (ptable->lock.locked != 0 && ptable->lock.locked != 1) {
    return -1;
  }

  for (tmp_p = ptable->proc; tmp_p < &ptable->proc[NPROC]; tmp_p++) {
    if (tmp_p->state == RUNNING) {
      if (tmp_p->sz == 0)
        return -1;
      if (!((char *)tmp_p->pgdir >= get_kglobal_addr() && (char *)tmp_p->pgdir < get_kplocal_addr())) 
        return -1;
      if (!(tmp_p->kstack >= get_kplocal_addr() && tmp_p->kstack < get_devspace_addr()))
        return -1;
      if (tmp_p->pid < 1)
        return -1;
      /* if (tmp_p->parent  0) */
      /*   return -1; */
      if (tmp_p->tf == 0)
        return -1;
      if (tmp_p->context == 0)
        return -1;
      /* if (tmp_p->chan != 0) */
      /*   return -1; */
      if (tmp_p->killed != 0 && tmp_p->killed != 1)
        return -1;
      for (i = 0; i < NOFILE; i++) {
        if (tmp_p->ofile[i] != 0 && !((char *)tmp_p->ofile[i] >= get_kplocal_addr() &&
                                      (char *)tmp_p->ofile[i] < get_devspace_addr()))
            return -1;
      }
      if (tmp_p->cwd == 0)
        return -1;
    }
  }

  
  //log
  if (log.outstanding < 0)
    return -1;
  if (log.committing !=0 && log.committing != 1)
    return -1;
  if (log.lh.n < 0)
    return -1;

  //icache
  if (icache.lock.locked != 0 && icache.lock.locked != 1)
    return -1;

  //bcache
  if (bcache.lock.locked != 0 && icache.lock.locked != 1)
    return -1;
  
  return 0;
}

void fault_injection(struct proc *p) {
  int num, rand;
  char *destroy;
  
  if (p->pid > 2 && (p->parent->state == SLEEPING)) {
    num = ticks % FAULT_SIZE - 1;
    rand = ticks % RAND_SIZE - (BROKEN_BYTE * 8);
    destroy = uselist[num];
    if (destroy == 0) {
      for (num++; num < FAULT_SIZE; num++) {
        if ((destroy = uselist[num]) != 0) {
          //cprintf("Fault userlist num: %d\n", num);
          memset(destroy+rand, 0, BROKEN_BYTE);
          break;
        }
        if (num == (FAULT_SIZE - 1)) {
          num = -1;
        }
      }
    }
    else {
      //cprintf("Fault userlist num: %d\n", num);
      memset(destroy+rand, 0, BROKEN_BYTE);
    }
  }  
}

