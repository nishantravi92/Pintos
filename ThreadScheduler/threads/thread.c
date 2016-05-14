#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
*    Used to detect stack overflow.  See the big comment at the top
*       of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
*    that are ready to run but not actually running. */
static struct list ready_list;
/* List of all processes.  Processes are added to this list
*    when they are first scheduled and removed when they exit. */
static struct list all_list;
/* List of sleepy threads. These threads are added to this list to sleep */ 
static struct list sleepy_list;    
/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;
/* Lock used by allocate_tid(). */
static struct lock tid_lock;
/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
{
void *eip;                  /* Return address. */
thread_func *function;      /* Function to call. */
void *aux;                  /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
*  If true, use multi-level feedback queue scheduler.
*  Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
bool thread_set_self;
int32_t load_average;
int32_t ready_threads;
static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void wake_sleepy_threads(void);
static bool order_by_time (const struct list_elem *a,
const struct list_elem *b,void *aux);
static void priority_scheduler(void);
static void bsd_scheduler(void);
static void threads_calculate_priority(struct thread *t, void *aux); 
static void threads_calculate_recent_cpu(struct thread *t, void *aux);
static void threads_calculate_load_avg(void);
/* Initializes the threading system by transforming the code
*  that's currently running into a thread.  This can't work in
*  general and it is possible in this case only because loader.S
*  was careful to put the bottom of the stack at a page boundary.
*  Also initializes the run queue and the tid lock.
*  After calling this function, be sure to initialize the page
*  allocator before trying to create any threads with
*  thread_create().
*  It is not safe to call thread_current() until this function
*  finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);
  temp_lock = NULL; 
  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleepy_list);
  thread_set_self = true;
  load_average = 0;
/* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  initial_thread->recent_cpu = 0;
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
*  Also creates the idle thread. */
void
thread_start (void) 
{
/* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

/* Start preemptive thread scheduling. */
  intr_enable ();

/* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
*    Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();
/* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

if(thread_mlfqs)
{
   int32_t f = 1 << 14;
  if(strcmp(t->name, "idle") != 0)
    t->recent_cpu = t->recent_cpu + f;
  if( (timer_ticks() %  TIMER_FREQ) == 0 )
  {
    threads_calculate_load_avg();
    thread_foreach(threads_calculate_recent_cpu, NULL);
  }
}
/* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
  {
    if(thread_mlfqs)
      thread_foreach (threads_calculate_priority, NULL);     
    intr_yield_on_return ();
  }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
  idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
*  PRIORITY, which executes FUNCTION passing AUX as the argument,
*  and adds it to the ready queue.  Returns the thread identifier
*  for the new thread, or TID_ERROR if creation fails.
*  If thread_start() has been called, then the new thread may be
*  scheduled before thread_create() returns.  It could even exit
*  before thread_create() returns.  Contrariwise, the original
*  thread may run for any amount of time before the new thread is
*  scheduled.  Use a semaphore or some other form of
*  synchronization if you need to ensure ordering.
*  The code provided sets the new thread's `priority' member to
*  PRIORITY, but no actual priority scheduling is implemented.
*  Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

ASSERT (function != NULL);

/* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

/* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();
  t->recent_cpu = thread_current()->recent_cpu;
/* Prepare thread for first run by initializing its stack.
*  Do this atomically so intermediate values for the 'stack' 
*  member cannot be observed. */
  old_level = intr_disable ();

/* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

/* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

/* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

/* Add to run queue. */
  thread_unblock (t);
  if(!thread_mlfqs)
  if(t->priority > thread_current()->priority)
    thread_yield();                              //Yields if higher priority thread has been put on queue
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
*  again until awoken by thread_unblock().
*  This function must be called with interrupts turned off.  It
*  is usually a better idea to use one of the synchronization
*  primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
* This is an error if T is not blocked.  (Use thread_yield() to
* make the running thread ready.)
* This function does not preempt the running thread.  This can
* be important: if the caller had disabled interrupts itself,
* it may expect that it can atomically unblock a thread and
* update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
*  This is running_thread() plus a couple of sanity checks.
*  See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();

/* Make sure T is really a thread.
*  If either of these assertions fire, then your thread may
*  have overflowed its stack.  Each thread has less than 4 kB
*  of stack, so a few big automatic arrays or moderate
*  recursion can cause stack overflow. */
ASSERT (is_thread (t));
ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
*  returns to the caller. */
void
thread_exit (void) 
{
ASSERT (!intr_context ());

#ifdef USERPROG
process_exit ();
#endif

/* Remove thread from all threads list, set our status to dying,
*  and schedule another process.  That process will destroy us
*  when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
 *  may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
  list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
*  This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
    e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. If new priority is lower than the present and if a thread has itself
 * tried to set its own priority, but its priority is higher than the new priority and the current thread is holding a lock
 * that needs to be released, then  sets member pre_priority to new value and then yields  */
   void
  thread_set_priority (int new_priority)
 {
   enum intr_level old_level = intr_disable();
   if(thread_set_self)
   { // If a thread is trying to set its own priority but is holding a lock needed by a higher priority thread, only changes member pre_priority 
     if(!list_empty(&thread_current()->donaters_list) && thread_current()->priority > new_priority)   // else both are changed
       thread_current()->pre_priority = new_priority;
     else
     {
       thread_current()->priority = new_priority;
       thread_current()->pre_priority = new_priority;
     }
    }
   else
   {
     thread_current()->priority = new_priority;
     thread_set_self = true;                         //Toggled and set back to true when called from thread_reset_priority
   }
  intr_set_level (old_level);
  thread_yield(); 
}
/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  thread_current()->niceness = nice;
  threads_calculate_priority(thread_current(), NULL);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->niceness;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
int32_t f = 1 << 14;
int32_t load = load_average*100;
if(load == 0)
   return 0;
load = load > 0 ? (load+f/2)/f: (load -f/2)/f;   
 return load;  
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  int32_t f = 1 << 14;
  int32_t recent = thread_current()->recent_cpu*100;
  if(recent == 0)
    return 0;
  recent = recent > 0 ? (recent+f/2)/f:(recent-f/2)/f;  
  return recent;
}

/* Idle thread.  Executes when no other thread is ready to run.
*  The idle thread is initially put on the ready list by
*  thread_start().  It will be scheduled once initially, at which
*  point it initializes idle_thread, "up"s the semaphore passed
*  to it to enable thread_start() to continue, and immediately
*  blocks.  After that, the idle thread never appears in the
*  ready list.  It is returned by next_thread_to_run() as a
*   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
/* Let someone else run. */
      intr_disable ();
      thread_block ();

/* Re-enable interrupts and wait for the next one.
*  The `sti' instruction disables interrupts until the
*  completion of the next instruction, so these two
*  instructions are executed atomically.  This atomicity is
*  important; otherwise, an interrupt could be handled
*  between re-enabling interrupts and waiting for the next
*  one to occur, wasting as much as one clock tick worth of
*  time.
*  See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
*  7.11.1 "HLT Instruction". */
asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

/* Copy the CPU's stack pointer into `esp', and then round that
*  down to the start of a page.  Because `struct thread' is
*  always at the beginning of a page and the stack pointer is
*  somewhere in the middle, this locates the curent thread. */
asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
*  NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
ASSERT (t != NULL);
ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  t->sleepy_ticks=0;
  list_init(&t->donaters_list);
  t->pre_priority = priority;
  t->wait_for_lock =NULL; 
  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
*  returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
/* Stack data is always allocated in word-size units. */
ASSERT (is_thread (t));
ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
*  return a thread from the run queue, unless the run queue is
*  empty.  (If the running thread can continue running, then it
*  will be in the run queue.)  If the run queue is empty, return
*  idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
*  tables, and, if the previous thread is dying, destroying it.
*  At this function's invocation, we just switched from thread
*  PREV, the new thread is already running, and interrupts are
*  still disabled.  This function is normally invoked by
*  thread_schedule() as its final action before returning, but
*  the first time a thread is scheduled it is called by
*  switch_entry() (see switch.S).
*  It's not safe to call printf() until the thread switch is
*  complete.  In practice that means that printf()s should be
*  added at the end of the function.
*  After this function and its caller returns, the thread switch
*  is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

ASSERT (intr_get_level () == INTR_OFF);

/* Mark us as running. */
  cur->status = THREAD_RUNNING;

/* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
/* Activate the new address space. */
  process_activate ();
#endif

/* If the thread we switched from is dying, destroy its struct
* thread.  This must happen late so that thread_exit() doesn't
* pull out the rug under itself.  (We don't free
* initial_thread because its memory was not obtained via
* palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
  {
    ASSERT (prev != cur);
    palloc_free_page (prev);
  }
}

/* Schedules a new process.  At entry, interrupts must be off and
*  the running process's state must have been changed from
*  running to some other state.  This function finds another
*  thread to run and switches to it.
*  It's not safe to call printf() until thread_schedule_tail()
*  has completed. */
static void
schedule (void) 
{
  wake_sleepy_threads();
  if(thread_mlfqs)
    bsd_scheduler();
  else
    priority_scheduler();
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;
  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

static void priority_scheduler()
{
  list_sort(&ready_list, order_by_priority, NULL); 
}
static void bsd_scheduler()
{
  list_sort(&ready_list, order_by_priority, NULL);
}
/* Donates priority to thread that is holding the lock. Works in a way that first it checks if the lock holders priority is greater
 * and if so then donates until the next thread that holds the lock has a higher priority or no one is holdig the lock.After that sets
 * the lock and thread members and adds self to priority donaters list */ 
void thread_donate_priority(struct lock *lock)
 {
   struct thread *t_holder = lock->holder;
   struct thread *holder = NULL ; 
   while(t_holder != NULL)
    {
      if(t_holder->priority < thread_current()->priority)
      {
        holder = t_holder;
        holder->priority = thread_current()->priority;  //Keep on bumping up priority till no lock holders exist or higher priority lock holder is 
        if(t_holder->wait_for_lock != NULL)             // encountered which holds lock 
          t_holder = t_holder->wait_for_lock->holder;   // go to next holder of lock
        else t_holder = NULL;
      }
     else break;
    }
   if(holder != NULL)                                  //If there is a holder whose priority is lower than thread_current() then .. 
   {  
     holder->priority = thread_current()->priority;
     thread_current()->wait_for_lock = lock;
     lock->wait_for_higher = true;
     list_insert_ordered(&lock->holder->donaters_list, &thread_current()->lock_elem, order_by_priority, NULL); 
   } 
 }
/* This basically checks to see if the total time of any thread put to sleep
* is greater or equal to total timer ticks till now and if true,
* removes the thread from sleep list and unblocks it */     
static void wake_sleepy_threads(void)
{
  struct list_elem *entry;
  struct thread *thread_woken;
  entry = list_begin(&sleepy_list);
  thread_woken = list_entry(entry, struct thread, elem);
  while(entry != list_end(&sleepy_list))
  {
   if(timer_ticks() >= thread_woken->sleepy_ticks)
   {
     entry = list_remove(entry);      
     thread_unblock(thread_woken);
     thread_woken = list_entry(entry, struct thread , elem);
   }
   else break;
  }
}
/* Function used to order according to priority */    
bool order_by_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *t1 = list_entry(a, struct thread, elem);
  struct thread *t2 = list_entry(b, struct thread, elem);
 if(t1->priority > t2->priority)
    return true;
   return false;
}
/* Orders according to time = timer_ticks()+sleep_time in non decreasing order. */
static bool order_by_time(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct thread *t1 = list_entry(a, struct thread, elem);
  struct thread *t2 = list_entry(b, struct thread, elem);
  if(t1->sleepy_ticks < t2->sleepy_ticks)
    return true;
  return false;
}
/* Inserts the thread into sleeping list and then blocks */  
void thread_put_to_sleep(int64_t ticks) 
{
  thread_current()->sleepy_ticks = timer_ticks() + ticks;
  list_insert_ordered(&sleepy_list,&thread_current()-> elem, order_by_time, NULL);
  thread_block();
}
/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}
/* Once thread has released lock, returns thread to original priority if there are no other donaters whose priority is higher than current i
 * thread. After sema_up, checks to see if that thread is not having a lock for which another higher priority thread is waiting to acquire it.
 * ie The blocked thread may have its priority bumped up while it is blocked */
/* Called after a thread has been put back on to ready list in sema_up() */
void thread_reset_priority(struct lock *lock)
{
       struct list_elem *entry;
       struct thread *t;
       struct thread *t1;
       thread_set_self = false;
       if(!list_empty(&ready_list))
       {
         entry = list_back(&ready_list);
         t = list_entry(entry, struct thread, elem);
         if(!list_empty(&t->donaters_list))  //For nesting, if new thread put on the block has a lock that another higher priority lock is waiting on,
        {                                    // then bump up the priority as this thread needs to release the lock it is holding 
           entry = list_front(&t->donaters_list);
           t1 = list_entry(entry, struct thread, lock_elem);
           if(t1->priority > t->priority)
             t->priority = t1->priority;
         }
       }
       entry = list_begin(&thread_current()->donaters_list);
       bool flag = false;
      //Remove same waiting locks from donaters list
      while(entry != list_end(&thread_current()->donaters_list))
      {
        t = list_entry(entry, struct thread, lock_elem);
        if(t->wait_for_lock == lock ) {
           entry = list_remove(entry);
      }
      // For case of thread holding multiple locks, donates priority if priority is lower than thread wanting the lock    
        else
        {
          if(t->priority > thread_current()->pre_priority)
          {
            thread_current()->priority = t->priority;
            flag = true;
          }
        entry = list_next(entry);
        }
      }
    lock = NULL;
    // If thread has no other donaters remaining, bring it down to original priority, else bump it up again
    if(!flag)
      thread_set_priority(thread_current()->pre_priority);
    else thread_set_priority(thread_current()->priority);
}
/* Calculates the priority of every thread at every fourth clock tick */
void threads_calculate_priority(struct thread *t, void *aux UNUSED)
{
 int32_t f = 1 << 14;
 if(strcmp(t->name, "idle") != 0)
 {
   int32_t recent = t->recent_cpu/4;
   recent = recent/f;
   int32_t nice = t->niceness*2;
   t->priority = PRI_MAX - recent - nice;   
 }
}
/*Calculates recent CPU values of all the threads every second */ 
void threads_calculate_recent_cpu(struct thread *t, void *aux UNUSED)
{
  int32_t f = 1 << 14;
  int32_t load_avg1 = load_average*2;
  int32_t load_avg2 = load_average*2 +f;
  int32_t load = ((int64_t) load_avg1) * f / load_avg2; 
  t->recent_cpu = ((int64_t) load) * t->recent_cpu / f  + t->niceness*f;  
}
/*Calculates recent load average every second */
static void threads_calculate_load_avg(void)
{
 ready_threads = list_size(&ready_list);
 if(strcmp(thread_current()->name, "idle"))
   ready_threads++;
 int32_t f = 1 << 14;
 int32_t divide1 = 59*f;
 int32_t divide2 = 60*f;
 int32_t div59by60 = ((int64_t) divide1) * f / divide2;
 int32_t load = ((int64_t) div59by60) * load_average / f;  

 int32_t ready = ready_threads*f;
 int32_t ready1 = ((int64_t) ready) * f / divide2;  
 load_average = load + ready1;
}
/* Offset of `stack' member within `struct thread'.
Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
