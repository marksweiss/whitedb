/*
* $Id:  $
* $Version: $
*
* Copyright (c) Priit J�rv 2009
*
* This file is part of wgandalf
*
* Wgandalf is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
* 
* Wgandalf is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License
* along with Wgandalf.  If not, see <http://www.gnu.org/licenses/>.
*
*/

 /** @file dblock.c
 *  Concurrent access support for wgandalf memory database
 *
 */

/* ====== Includes =============== */

#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef _WIN32
#include "../config-w32.h"
#else
#include "../config.h"
#endif
#include "dballoc.h"
#include "dbdata.h" /* for CHECK */

/* ====== Private headers and defs ======== */

#include "dblock.h"

#ifndef QUEUED_LOCKS
#define WAFLAG 0x1  /* writer active flag */
#define RC_INCR 0x2  /* increment step for reader count */
#else
/* classes of locks. Class "none" is also possible, but
 * this is defined as 0x0 to simplify some atomic operations */
#define LOCKQ_READ 0x02
#define LOCKQ_WRITE 0x04
#endif

#define ASM32 1 /* XXX: handle using autotools etc */

#ifdef _WIN32
#define SPIN_COUNT 100000 /* break spin after this many cycles */
#define SLEEP_MSEC 1 /* minimum resolution is 1 millisecond
                      * Note: with queued locks, we could set it to 0
                      * with the known Sleep(0) effect, however this has
                      * potential scheduling priority issues. */
#else
#define SPIN_COUNT 500 /* shorter spins perform better with Linux */
#ifndef QUEUED_LOCKS
#define SLEEP_NSEC 500000 /* 500 microseconds */
#else
#define SLEEP_NSEC 1 /* just deschedule thread */
#endif
#endif

#ifdef _WIN32
/* XXX: quick hack for MSVC. Should probably find a cleaner solution */
#define inline __inline
#endif

/* ======= Private protos ================ */


inline void atomic_increment(volatile gint *ptr, gint incr);
inline void atomic_and(volatile gint *ptr, gint val);
inline gint fetch_and_add(volatile gint *ptr, gint incr);
inline gint fetch_and_store(volatile gint *ptr, gint val);
inline gint compare_and_swap(volatile gint *ptr, gint old, gint new);

#ifdef QUEUED_LOCKS
gint alloc_lock(void * db);
void free_lock(void * db, gint node);
gint deref_link(void *db, volatile gint *link);
#endif


/* ====== Functions ============== */


/* -------------- helper functions -------------- */

/*
 * System- and platform-dependent atomic operations
 * XXX: not all ops implemented as helpers. Not all places in code
 *      use helpers yet (but should eventually, to make porting
 *      and modification easier).
 */

/** Atomic increment. On x86 platform, this is internally
 *  the same as fetch_and_add().
 */

inline void atomic_increment(volatile gint *ptr, gint incr) {
#if defined(__GNUC__)
  __sync_fetch_and_add(ptr, incr);
#elif defined(_WIN32)
  _InterlockedExchangeAdd(ptr, incr);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/** Atomic AND operation.
 */

inline void atomic_and(volatile gint *ptr, gint val) {
#if defined(__GNUC__)
  __sync_fetch_and_and(ptr, val);
#elif defined(_WIN32)
  _InterlockedAnd(ptr, val);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/** Atomic OR operation.
 */

inline void atomic_or(volatile gint *ptr, gint val) {
#if defined(__GNUC__)
  __sync_fetch_and_or(ptr, val);
#elif defined(_WIN32)
  _InterlockedOr(ptr, val);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/** Fetch and (dec|inc)rement. Returns value before modification.
 */

inline gint fetch_and_add(volatile gint *ptr, gint incr) {
#if defined(__GNUC__)
  return __sync_fetch_and_add(ptr, incr);
#elif defined(_WIN32)
  return _InterlockedExchangeAdd(ptr, incr);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/** Atomic fetch and store. Swaps two values.
 */

inline gint fetch_and_store(volatile gint *ptr, gint val) {
  /* Despite the name, the GCC builtin should just
   * issue XCHG operation. There is no testing of
   * anything, just lock the bus and swap the values,
   * as per Intel's opcode reference.
   *
   * XXX: not available on all compiler targets :-(
   */
#if defined(__GNUC__)
  return __sync_lock_test_and_set(ptr, val);
#elif defined(_WIN32)
  return _InterlockedExchange(ptr, val);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/** Compare and swap. If value at ptr equals old, set it to
 *  new and return 1. Otherwise the function returns 0.
 */

inline gint compare_and_swap(volatile gint *ptr, gint old, gint new) {
#if defined(__GNUC__)
  return __sync_bool_compare_and_swap(ptr, old, new);
#elif defined(_WIN32)
  return (_InterlockedCompareExchange(ptr, new, old) == old);
#else
#error Atomic operations not implemented for this compiler
#endif
}

/* ----------- read and write transaction support ----------- */

/*
 * The following functions implement giant shared/exclusive
 * lock on the database. The rest of the db API is (currently)
 * implemented independently - therefore use of the locking routines
 * does not automatically guarantee isolation.
 *
 * Algorithms used for locking:
 *
 * 1. Simple reader-preference lock using a single global sync
 *    variable (described by Mellor-Crummey & Scott '92).
 * 2. Locally spinning queued locks (Mellor-Crummey & Scott '92). This
 *    algorithm is enabled by defining QUEUED_LOCKS.
 */

/** Start write transaction
 *   Current implementation: acquire database level exclusive lock
 *   Blocks until lock is acquired.
 */

gint wg_start_write(void * db) {
  int i;
#ifdef ASM32
  gint cond = 0;
#endif
#ifdef _WIN32
  int ts;
#else
  struct timespec ts;
#endif

#ifndef QUEUED_LOCKS
  volatile gint *gl;
#else
  gint lock, prev;
  lock_queue_node *lockp;
  db_memsegment_header* dbh;
#endif

#ifdef CHECK
  if (!dbcheck(db)) {
    fprintf(stderr,"Invalid database pointer in wg_start_write.\n");
    return 0;
  }
#endif  
  
#ifndef QUEUED_LOCKS
  gl = offsettoptr(db,
    ((db_memsegment_header *) db)->locks.global_lock);

  /* First attempt at getting the lock without spinning */
#if defined(__GNUC__) && defined (ASM32)
  __asm__ __volatile__(
    "movl $0, %%eax;\n\t"
    "lock cmpxchgl %1, %2;\n\t"
    "setzb %0\n"
    : "=m" (cond)
    : "q" (WAFLAG), "m" (*gl)
    : "eax", "memory" );
  if(cond)
    return 1;
#elif defined(__GNUC__)
  if(__sync_bool_compare_and_swap(gl, 0, WAFLAG))
    return 1;
#elif defined(_WIN32)
  if(_InterlockedCompareExchange(gl, WAFLAG, 0) == 0)
    return 1;
#else
#error Atomic operations not implemented for this compiler
#endif

#ifdef _WIN32
  ts = SLEEP_MSEC;
#else
  ts.tv_sec = 0;
  ts.tv_nsec = SLEEP_NSEC;
#endif

  /* Spin loop */
  for(;;) {
    for(i=0; i<SPIN_COUNT; i++) {
#if defined(__GNUC__) && defined (ASM32)
      __asm__ __volatile__(
        "pause;\n\t"
        "cmpl $0, %2;\n\t"
        "jne l1;\n\t"
        "movl $0, %%eax;\n\t"
        "lock cmpxchgl %1, %2;\n"
        "l1: setzb %0\n"
        : "=m" (cond)
        : "q" (WAFLAG), "m" (*gl)
        : "eax", "memory");
      if(cond)
        return 1;
#elif defined(__GNUC__)
      if(!(*gl) && __sync_bool_compare_and_swap(gl, 0, WAFLAG))
        return 1;
#elif defined(_WIN32)
      if(!(*gl) && _InterlockedCompareExchange(gl, WAFLAG, 0) == 0)
        return 1;
#else
#error Atomic operations not implemented for this compiler
#endif
    }
    
    /* Give up the CPU so the lock holder(s) can continue */
#ifdef _WIN32
    Sleep(ts);
    ts += SLEEP_MSEC;
#else
    nanosleep(&ts, NULL);
    ts.tv_nsec += SLEEP_NSEC;
#endif
  }

#else /* QUEUED_LOCKS */
  lock = alloc_lock(db);
  if(!lock) {
    fprintf(stderr,"Failed to allocate lock.\n");
    return 0;
  }

  dbh = (db_memsegment_header *) db;
  lockp = (lock_queue_node *) offsettoptr(db, lock);

  lockp->class = LOCKQ_WRITE;
  lockp->next = 0;
  lockp->state = 1; /* blocked, no successor */

  /* Put ourselves at the end of queue and check
   * if there is a predecessor node.
   */
  prev = fetch_and_store(&(dbh->locks.tail), lock);

  if(!prev) {
    /* No other locks in queue (note that this does not
     * explicitly mean there are no active readers. For
     * that we examine reader_count).
     */
    dbh->locks.next_writer = lock;
    if(!dbh->locks.reader_count &&\
      fetch_and_store(&(dbh->locks.next_writer), 0) == lock) {
      /* No readers, we're still the next writer */
      /* lockp->state &= ~1; */
      atomic_and(&(lockp->state), ~1); /* not blocked */
    }
  }
  else {
    lock_queue_node *prevp = (lock_queue_node *) offsettoptr(db, prev);

    /* There is something ahead of us in the queue, by
     * definition we must wait until all predecessors complete.
     * The unblocking will be done by either a lone writer
     * directly before us, or a random reader that manages to decrement
     * the reader count to 0 upon completion.
     */
      /* prevp->state |= LOCKQ_WRITE; */
     atomic_or(&(prevp->state), LOCKQ_WRITE);
     prevp->next = lock;
  }

  if(lockp->state & 1) {
    /* Spin-wait */
#ifdef _WIN32
    ts = SLEEP_MSEC;
#else
    ts.tv_sec = 0;
    ts.tv_nsec = SLEEP_NSEC;
#endif

    for(;;) {
      for(i=0; i<SPIN_COUNT; i++) {
#if defined(__GNUC__) && defined (ASM32)
        __asm__ __volatile__(
          "pause;\n\t"
          "movl %2, %%eax;\n\t"
          "andl %1, %%eax;\n\t"
          "setzb %0\n"
          : "=m" (cond)
          : "i" (1), "m" (lockp->state)
          : "eax");
        if(cond)
          return lock;
#else
        if(!(lockp->state & 1)) return lock;
#endif
      }

#ifdef _WIN32
      Sleep(ts);
      ts += SLEEP_MSEC;
#else
      nanosleep(&ts, NULL);
      ts.tv_nsec += SLEEP_NSEC;
#endif
    }
  }

  return lock;
#endif /* QUEUED_LOCKS */
  return 0; /* dummy */
}

/** End write transaction
 *   Current implementation: release database level exclusive lock
 */

gint wg_end_write(void * db, gint lock) {

#ifndef QUEUED_LOCKS
  volatile gint *gl;
#else
  lock_queue_node *lockp;
  db_memsegment_header* dbh;
#endif
  
#ifdef CHECK
  if (!dbcheck(db)) {
    fprintf(stderr,"Invalid database pointer in wg_end_write.\n");
    return 0;
  }
#endif  
  
#ifndef QUEUED_LOCKS
  gl = offsettoptr(db,
    ((db_memsegment_header *) db)->locks.global_lock);

  /* Clear the writer active flag */
#if defined(__GNUC__)
  __sync_fetch_and_and(gl, ~(WAFLAG));
#elif defined(_WIN32)
  _InterlockedAnd(gl, ~(WAFLAG));
#else
#error Atomic operations not implemented for this compiler
#endif

#else /* QUEUED_LOCKS */
  dbh = (db_memsegment_header *) db;
  lockp = (lock_queue_node *) offsettoptr(db, lock);

  /* Check for the successor. If we're the last node, reset
   * the queue completely (see comments in wg_end_read() for
   * a more detailed explanation of why this can be done).
   */
  if(lockp->next || !compare_and_swap(&(dbh->locks.tail), lock, 0)) {
    lock_queue_node *nextp;
    while(!lockp->next); /* Wait until the successor has updated
                          * this record. */
    nextp = (lock_queue_node *) offsettoptr(db, lockp->next);
    if(nextp->class & LOCKQ_READ)
      atomic_increment(&(dbh->locks.reader_count), 1);

    /* nextp->state &= ~1; */
    atomic_and(&(nextp->state), ~1); /* unblock successor */
  }

  free_lock(db, lock);
#endif /* QUEUED_LOCKS */

  return 1;
}

/** Start read transaction
 *   Current implementation: acquire database level shared lock
 *   Increments reader count, blocks until there are no active
 *   writers.
 */

gint wg_start_read(void * db) {
  int i;
#ifdef ASM32
  gint cond = 0;
#endif
#ifdef _WIN32
  int ts;
#else
  struct timespec ts;
#endif

#ifndef QUEUED_LOCKS
  volatile gint *gl;
#else
  gint lock, prev;
  lock_queue_node *lockp;
  db_memsegment_header* dbh;
#endif

#ifdef CHECK
  if (!dbcheck(db)) {
    fprintf(stderr,"Invalid database pointer in wg_start_read.\n");
    return 0;
  }
#endif  
  
#ifndef QUEUED_LOCKS
  gl = offsettoptr(db,
    ((db_memsegment_header *) db)->locks.global_lock);

  /* Increment reader count atomically */
#if defined(__GNUC__)
  __sync_fetch_and_add(gl, RC_INCR);
#elif defined(_WIN32)
  _InterlockedExchangeAdd(gl, RC_INCR);
#else
#error Atomic operations not implemented for this compiler
#endif

  /* Try getting the lock without pause */
#if defined(__GNUC__) && defined (ASM32)
  __asm__(
    "movl %2, %%eax;\n\t"
    "andl %1, %%eax;\n\t"
    "setzb %0\n"
    : "=m" (cond)
    : "i" (WAFLAG), "m" (*gl)
    : "eax");
  if(cond)
    return 1;
#else
  if(!((*gl) & WAFLAG)) return 1;
#endif

#ifdef _WIN32
  ts = SLEEP_MSEC;
#else
  ts.tv_sec = 0;
  ts.tv_nsec = SLEEP_NSEC;
#endif

  /* Spin loop */
  for(;;) {
    for(i=0; i<SPIN_COUNT; i++) {
#if defined(__GNUC__) && defined (ASM32)
      __asm__ __volatile__(
        "pause;\n\t"
        "movl %2, %%eax;\n\t"
        "andl %1, %%eax;\n\t"
        "setzb %0\n"
        : "=m" (cond)
        : "i" (WAFLAG), "m" (*gl)
        : "eax");
      if(cond)
        return 1;
#else
      if(!((*gl) & WAFLAG)) return 1;
#endif
    }

#ifdef _WIN32
    Sleep(ts);
    ts += SLEEP_MSEC;
#else
    nanosleep(&ts, NULL);
    ts.tv_nsec += SLEEP_NSEC;
#endif
  }

#else /* QUEUED_LOCKS */
  lock = alloc_lock(db);
  if(!lock) {
    fprintf(stderr,"Failed to allocate lock.\n");
    return 0;
  }

  dbh = (db_memsegment_header *) db;
  lockp = (lock_queue_node *) offsettoptr(db, lock);

  lockp->class = LOCKQ_READ;
  lockp->next = 0;
  lockp->state = 1; /* blocked, no successor */

  /* Put ourselves at the end of queue and check
   * if there is a predecessor node.
   */
  prev = fetch_and_store(&(dbh->locks.tail), lock);

  if(!prev) {
    /* No other locks, increment reader count and return */
    atomic_increment(&(dbh->locks.reader_count), 1);
    /* lockp->state &= ~1; */
    atomic_and(&(lockp->state), ~1); /* not blocked */
  }
  else {
    lock_queue_node *prevp = (lock_queue_node *) offsettoptr(db, prev);

    /* There is a previous lock. Depending on it's type
     * and state we may need to spin-wait (this happens if
     * there is an active writer somewhere).
     */
    if(prevp->class & LOCKQ_WRITE ||\
      compare_and_swap(&(prevp->state), 1, 1|(LOCKQ_READ))) {

      /* Predecessor is a writer or a blocked reader. Spin-wait;
       * the predecessor will unblock us and increment the reader count */
      prevp->next = lock;
      if(lockp->state & 1) {
        /* Spin-wait */
#ifdef _WIN32
        ts = SLEEP_MSEC;
#else
        ts.tv_sec = 0;
        ts.tv_nsec = SLEEP_NSEC;
#endif

        for(;;) {
          for(i=0; i<SPIN_COUNT; i++) {
#if defined(__GNUC__) && defined (ASM32)
            __asm__ __volatile__(
              "pause;\n\t"
              "movl %2, %%eax;\n\t"
              "andl %1, %%eax;\n\t"
              "setzb %0\n"
              : "=m" (cond)
              : "i" (1), "m" (lockp->state)
              : "eax");
            if(cond)
              goto rd_lock_cont;
#else
            if(!(lockp->state & 1)) goto rd_lock_cont;
#endif
          }

#ifdef _WIN32
          Sleep(ts);
          ts += SLEEP_MSEC;
#else
          nanosleep(&ts, NULL);
          ts.tv_nsec += SLEEP_NSEC;
#endif
        }
      }
    }
    else {
      /* Predecessor is a reader, we can continue */
      atomic_increment(&(dbh->locks.reader_count), 1);
      prevp->next = lock;
      /* lockp->state &= ~1; */
      atomic_and(&(lockp->state), ~1); /* not blocked */
    }
  }

rd_lock_cont:
  /* Now check if this lock has a successor. If it's a reader
   * we know it's currently blocked since this lock was
   * blocked too up to now. So we need to unblock the successor.
   */
  if(lockp->state & LOCKQ_READ) {
    lock_queue_node *nextp;

    while(!lockp->next); /* wait until structure is updated */
    atomic_increment(&(dbh->locks.reader_count), 1);
    nextp = (lock_queue_node *) offsettoptr(db, lockp->next);
    /* nextp->state &= ~1; */
    atomic_and(&(nextp->state), ~1); /* unblock successor */
  }
  
  return lock;
#endif /* QUEUED_LOCKS */
  return 0; /* dummy */
}

/** End read transaction
 *   Current implementation: release database level shared lock
 */

gint wg_end_read(void * db, gint lock) {

#ifndef QUEUED_LOCKS
  volatile gint *gl;
#else
  lock_queue_node *lockp;
  db_memsegment_header* dbh;
#endif
  
#ifdef CHECK
  if (!dbcheck(db)) {
    fprintf(stderr,"Invalid database pointer in wg_end_read.\n");
    return 0;
  }
#endif  
  
#ifndef QUEUED_LOCKS
  gl = offsettoptr(db,
    ((db_memsegment_header *) db)->locks.global_lock);

  /* Decrement reader count */
#if defined(__GNUC__)
  __sync_fetch_and_add(gl, -RC_INCR);
#elif defined(_WIN32)
  _InterlockedExchangeAdd(gl, -RC_INCR);
#else
#error Atomic operations not implemented for this compiler
#endif

#else /* QUEUED_LOCKS */
  dbh = (db_memsegment_header *) db;
  lockp = (lock_queue_node *) offsettoptr(db, lock);

  /* Check if the successor is a waiting writer (predecessors
   * cannot be waiting readers with fair queueing).
   *
   * If there are active readers, their presence is also
   * known via reader_count. This is why we can set the value
   * of tail to none (0) if our reader is the last one in queue.
   * This is important from memory management point of view - 
   * basically the contents of the rest of the reader locks is
   * now irrelevant for future locks and we can "cut" the queue.
   *
   * The other important point is that we are interested in cases where
   * the CAS operation *fails*, indicating that a successor has appeared.
   */
  if(lockp->next || !compare_and_swap(&(dbh->locks.tail), lock, 0)) {

    while(!lockp->next); /* Wait until the successor has updated
                          * this record, meaning no further locks
                          * are interested in reading our state. This
                          * record can now be freed without checking
                          * reference count.
                          */
    if(lockp->state & LOCKQ_WRITE)
      dbh->locks.next_writer = lockp->next;
  }
  if(fetch_and_add(&(dbh->locks.reader_count), -1) == 1) {

    /* No more readers. If there is a writer in line, unblock it */
    gint w = fetch_and_store(&(dbh->locks.next_writer), 0);
    if(w) {
        lock_queue_node *wp = (lock_queue_node *) offsettoptr(db, w);
        /* wp->state &= ~1; */
        atomic_and(&(wp->state), ~1); /* unblock writer */
    }
  }
  free_lock(db, lock);
#endif /* QUEUED_LOCKS */

  return 1;
}

/* ---------- memory management for queued locks ---------- */

/*
 * Queued locks algorithm assumes allocating memory cells
 * for each lock. These cells need to be memory-aligned to
 * allow spinlocks run locally, but more importantly, allocation
 * and freeing of the cells has to be implemented in a lock-free
 * manner.
 *
 * The method used in the initial implementation is freelist
 * with reference counts (generally described by Valois '95,
 * actual code is based on examples from
 * http://www.non-blocking.com/Eng/services-technologies_non-blocking-lock-free.htm)
 *
 * XXX: code untested currently
 * XXX: Mellor-Crummey & Scott algorithm possibly does not need
 *      refcounts. If so, they should be #ifdef-ed out, but
 *      kept for possible future expansion.
 */

#ifdef QUEUED_LOCKS

/** Initialize memory cells.
 *   Not parallel-safe, so should be run during database init.
 */

gint init_lock_queue(void * db) {
  gint i, chunk_wall;
  db_memsegment_header* dbh;
  lock_queue_node *tmp;

#ifdef CHECK
  if (!dbcheck(db)) {
    fprintf(stderr,"Invalid database pointer in init_lock_queue.\n");
    return -1;
  }
#endif  

  dbh = (db_memsegment_header *) db;
  chunk_wall = dbh->locks.storage + dbh->locks.max_nodes*SYN_VAR_PADDING;

  for(i=dbh->locks.storage; i<chunk_wall; ) {
    tmp = (lock_queue_node *) offsettoptr(db, i);
    tmp->refcount = 1;
    i+=SYN_VAR_PADDING;
    tmp->next_cell = i; /* offset of next cell */
  }
  tmp->next_cell=0; /* last node */

  /* top of the stack points to first cell in chunk */
  dbh->locks.freelist = dbh->locks.storage;
  return 0;
}

/** Allocate memory cell for a lock.
 *   Used internally only, so we assume the passed db pointer
 *   is already validated.
 *
 *   Returns offset to allocated cell.
 */

gint alloc_lock(void * db) {
  db_memsegment_header* dbh = (db_memsegment_header *) db;
  lock_queue_node *tmp;

  for(;;) {
    gint t = dbh->locks.freelist;
    if(!t)
      return 0; /* end of chain :-( */
    tmp = (lock_queue_node *) offsettoptr(db, t);

#if defined(__GNUC__)
    __sync_fetch_and_add(&(tmp->refcount), 2);

    if(__sync_bool_compare_and_swap(&(dbh->locks.freelist),
      t, tmp->next_cell)) {
      __sync_fetch_and_add(&(tmp->refcount), -1); /* clear lsb */
      return t;
    }
#elif defined(_WIN32)
    _InterlockedExchangeAdd(&(tmp->refcount), 2);

    if(_InterlockedCompareExchange(&(dbh->locks.freelist),
      tmp->next_cell, t) == t) {
      _InterlockedExchangeAdd(&(tmp->refcount), -1);  /* clear lsb */
      return t;
    }
#else
#error Atomic operations not implemented for this compiler
#endif

    free_lock(db, t);
  }

  return 0; /* dummy */
}

/** Release memory cell for a lock.
 *   Used internally only.
 */

void free_lock(void * db, gint node) {
  db_memsegment_header* dbh = (db_memsegment_header *) db;
  lock_queue_node *tmp;
  volatile gint t;

  tmp = (lock_queue_node *) offsettoptr(db, node);

  /* Clear reference */
#if defined(__GNUC__)
  __sync_fetch_and_add(&(tmp->refcount), -2);
#elif defined(_WIN32)
  _InterlockedExchangeAdd(&(tmp->refcount), -2);
#else
#error Atomic operations not implemented for this compiler
#endif

  /* Try to set lsb */
#if defined(__GNUC__)
  if(__sync_bool_compare_and_swap(&(tmp->refcount), 0, 1)) {
#elif defined(_WIN32)
  if(_InterlockedCompareExchange(&(tmp->refcount), 1, 0) == 0) {
#else
#error Atomic operations not implemented for this compiler
#endif

/* XXX:
    if(tmp->next_cell) free_lock(db, tmp->next_cell);
*/
    do {
      t = dbh->locks.freelist;
      tmp->next_cell = t;
#if defined(__GNUC__)
    } while (!__sync_bool_compare_and_swap(&(dbh->locks.freelist),
      t, node));
#elif defined(_WIN32)
    } while (_InterlockedCompareExchange(&(dbh->locks.freelist),
      node, t) != t);
#else
#error Atomic operations not implemented for this compiler
#endif
  }
}

/** De-reference (release pointer to) a link.
 *   Used internally only.
 */

gint deref_link(void *db, volatile gint *link) {
  lock_queue_node *tmp;
  volatile gint t;

  for(;;) {
    t = *link;
    if(t == 0) return 0;
    tmp = (lock_queue_node *) offsettoptr(db, t);

#if defined(__GNUC__)
    __sync_fetch_and_add(&(tmp->refcount), 2);
#elif defined(_WIN32)
    _InterlockedExchangeAdd(&(tmp->refcount), 2);
#else
#error Atomic operations not implemented for this compiler
#endif

    if(t == *link) return t;
    free_lock(db, t);
  }
}

#endif /* QUEUED_LOCKS */