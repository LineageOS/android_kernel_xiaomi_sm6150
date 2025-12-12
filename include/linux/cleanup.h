/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scope-based Resource Management (cleanup.h) - Backport for kernel 4.14
 *
 * Original by Peter Zijlstra <peterz@infradead.org> for kernel 6.5+
 * Backported for kernel 4.14 compatibility.
 *
 * This provides automatic resource cleanup using GCC's __attribute__((cleanup))
 * which calls a specified function when a variable goes out of scope.
 */
#ifndef _LINUX_CLEANUP_H
#define _LINUX_CLEANUP_H

#include <linux/compiler.h>

/*
 * DEFINE_FREE(name, type, free):
 *	simple helper macro that defines the required wrapper for a __free()
 *	based cleanup function. @free is an expression using '_T' to access
 *	the variable. e.g.:
 *
 *	DEFINE_FREE(kfree, void *, if (_T) kfree(_T))
 *
 *	defines an internal function to be used with __free(kfree):
 *
 *	void *p __free(kfree) = kmalloc(...);
 */
#define DEFINE_FREE(_name, _type, _free) \
	static inline void __free_##_name(void *p) { _type _T = *(_type *)p; _free; }

/*
 * __free(name): Designate a variable to be cleaned up by @name's free function.
 *
 * When the variable goes out of scope, the cleanup function defined by
 * DEFINE_FREE(@name, ...) will be called.
 */
#define __free(_name)	__attribute__((__cleanup__(__free_##_name)))

/*
 * no_free_ptr(p): Prevents __free() from freeing @p.
 *
 * Returns @p and clears the variable to NULL, preventing automatic cleanup.
 * Use this when transferring ownership of a resource.
 */
#define no_free_ptr(p) \
	({ __auto_type __ptr = (p); (p) = NULL; __ptr; })

/*
 * return_ptr(p): Return @p from a function while preventing __free cleanup.
 *
 * Equivalent to: return no_free_ptr(p);
 */
#define return_ptr(p)	return no_free_ptr(p)

/*
 * DEFINE_CLASS(name, type, exit, init, init_args...):
 *	Helper to define a class @name with constructor and destructor.
 *
 *	@type: type of the class variable
 *	@exit: cleanup expression using 'this' to access the class var
 *	@init: initialization expression
 *	@init_args: argument types for the constructor
 *
 *	Usage example:
 *	  DEFINE_CLASS(fdget, struct fd, fdput(this), fdget(fd), int fd)
 *
 *	  CLASS(fdget, f)(fd);
 *	  // f is auto-cleaned via fdput() on scope exit
 */
#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)		\
typedef _type class_##_name##_t;					\
static inline void class_##_name##_destructor(_type *p)			\
{ _type this = *p; _exit; }						\
static inline _type class_##_name##_constructor(_init_args)		\
{ _type t = _init; return t; }

/*
 * EXTEND_CLASS(name, ext, init, init_args...):
 *	Extend class @name with @ext extension, using a new constructor.
 */
#define EXTEND_CLASS(_name, ext, _init, _init_args...)			\
typedef class_##_name##_t class_##_name##ext##_t;			\
static inline void class_##_name##ext##_destructor(class_##_name##_t *p)\
{ class_##_name##_destructor(p); }					\
static inline class_##_name##_t class_##_name##ext##_constructor(_init_args) \
{ class_##_name##_t t = _init; return t; }

/*
 * CLASS(name, var): Declare an auto-cleanup class variable.
 *
 * Creates a variable @var of type class_@name_t that will be automatically
 * cleaned up when it goes out of scope. Initialize with constructor args:
 *
 *   CLASS(fdget, f)(fd);
 *   // equivalent to: class_fdget_t f __cleanup(...) = class_fdget_constructor(fd);
 */
#define CLASS(_name, var)						\
	class_##_name##_t var __attribute__((__cleanup__(class_##_name##_destructor))) = \
		class_##_name##_constructor

/*
 * The following section provides guard macros for locks.
 * Guard types wrap lock/unlock in a class, so locks are automatically
 * released when leaving scope.
 */

/*
 * __guard_ptr(name): Get pointer suitable for checking guard validity.
 * Used by scoped_guard() to check if lock was acquired.
 */
#define __guard_ptr(_name) class_##_name##_lock_ptr

/*
 * DEFINE_GUARD(name, type, lock, unlock):
 *	Convenience macro to define a lock guard class.
 *
 *	@name: guard name for use with guard() and scoped_guard()
 *	@type: lock type (e.g., spinlock_t *)
 *	@lock: locking expression using 'this'
 *	@unlock: unlocking expression using 'this'
 *
 *	Example:
 *	  DEFINE_GUARD(spinlock, spinlock_t *, spin_lock(_T), spin_unlock(_T))
 */
#define DEFINE_GUARD(_name, _type, _lock, _unlock) \
	DEFINE_CLASS(_name, _type, if (_T) { _unlock; }, ({ _lock; _T; }), _type _T) \
	static inline void * class_##_name##_lock_ptr(class_##_name##_t *_T) \
	{ return *_T; }

/*
 * DEFINE_GUARD_COND(name, ext, cond):
 *	Define a conditional variant of guard @name with extension @ext.
 *	@cond should be a locking expression that may fail (returns NULL/false on failure).
 */
#define DEFINE_GUARD_COND(_name, _ext, _cond) \
	EXTEND_CLASS(_name, _ext, ({ _cond; }), class_##_name##_t _T) \
	static inline void * class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
	{ return *_T; }

/*
 * guard(name): Create an anonymous lock guard for the current scope.
 *
 * The lock will be automatically released when the scope exits.
 *
 * Example:
 *   guard(spinlock)(&lock);
 *   // lock is held from here...
 *   // ...until scope exit
 */
#define guard(_name) \
	CLASS(_name, __UNIQUE_ID(guard))

/*
 * scoped_guard(name, args...):
 *	Macro to create a scoped guard for a following statement/block.
 *
 *	The lock is acquired before entering the block and automatically
 *	released after the block completes.
 *
 *	Example:
 *	  scoped_guard(spinlock, &lock) {
 *	      // lock is held here
 *	      do_something();
 *	  }
 *	  // lock is released here
 *
 *	Can also be used for single statements:
 *	  scoped_guard(spinlock, &lock)
 *	      do_something();
 */
#define scoped_guard(_name, args...)					\
	for (CLASS(_name, scope)(args),					\
	     *done = NULL; __guard_ptr(_name)(&scope) && !done; done = (void *)1)

/*
 * scoped_cond_guard(name, fail, args...):
 *	Conditional scoped guard - allows handling lock acquisition failure.
 *
 *	If the lock cannot be acquired (trylock fails), execute @fail.
 *	Otherwise, execute the following block with lock held.
 *
 *	Example:
 *	  scoped_cond_guard(spinlock_trylock, return -EBUSY, &lock) {
 *	      do_something();
 *	  }
 */
#define scoped_cond_guard(_name, _fail, args...)			\
	for (CLASS(_name, scope)(args),					\
	     *done = NULL; !done; done = (void *)1)			\
		if (!__guard_ptr(_name)(&scope)) _fail;			\
		else

/*
 * Lock guard macro helpers for building typed lock guards.
 */

/* For locks without arguments (like raw spinlocks held by CPU) */
#define DEFINE_LOCK_GUARD_0(_name, _lock, _unlock)			\
	DEFINE_CLASS(_name, int, _unlock, ({ _lock; 0; }))		\
	static inline void *class_##_name##_lock_ptr(int *_T) { return (void *)1; }

/* For typed locks - standard pattern */
#define DEFINE_LOCK_GUARD_1(_name, _type, _lock, _unlock, ...)		\
typedef struct {							\
	_type *lock;							\
	__VA_ARGS__							\
} class_##_name##_t;							\
									\
static inline void class_##_name##_destructor(class_##_name##_t *_T)	\
{									\
	if (_T->lock) { _unlock; }					\
}									\
									\
static inline class_##_name##_t class_##_name##_constructor(_type *l)	\
{									\
	class_##_name##_t _t = { .lock = l };				\
	_lock;								\
	return _t;							\
}									\
									\
static inline void *class_##_name##_lock_ptr(class_##_name##_t *_T)	\
{ return _T->lock; }

/* For conditional/try locks */
#define DEFINE_LOCK_GUARD_1_COND(_name, _ext, _cond)			\
static inline class_##_name##_t class_##_name##_ext##_constructor(_type *l) \
{									\
	class_##_name##_t _t = { .lock = l };				\
	if (!(_cond))							\
		_t.lock = NULL;						\
	return _t;							\
}									\
									\
static inline void *class_##_name##_ext##_lock_ptr(class_##_name##_t *_T) \
{ return _T->lock; }

/*
 * Predefined guard types for common kernel locks.
 * These are compatible with kernel 4.14 locking primitives.
 */

#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/rwlock.h>

/* Spinlock guards */
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t,
	spin_lock(_T->lock),
	spin_unlock(_T->lock))

DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t,
	spin_lock_irq(_T->lock),
	spin_unlock_irq(_T->lock))

DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t,
	spin_lock_irqsave(_T->lock, _T->flags),
	spin_unlock_irqrestore(_T->lock, _T->flags),
	unsigned long flags;)

/* Raw spinlock guards */
DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t,
	raw_spin_lock(_T->lock),
	raw_spin_unlock(_T->lock))

DEFINE_LOCK_GUARD_1(raw_spinlock_irq, raw_spinlock_t,
	raw_spin_lock_irq(_T->lock),
	raw_spin_unlock_irq(_T->lock))

DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
	raw_spin_lock_irqsave(_T->lock, _T->flags),
	raw_spin_unlock_irqrestore(_T->lock, _T->flags),
	unsigned long flags;)

/* Mutex guard */
DEFINE_LOCK_GUARD_1(mutex, struct mutex,
	mutex_lock(_T->lock),
	mutex_unlock(_T->lock))

/* RW semaphore guards */
DEFINE_LOCK_GUARD_1(rwsem_read, struct rw_semaphore,
	down_read(_T->lock),
	up_read(_T->lock))

DEFINE_LOCK_GUARD_1(rwsem_write, struct rw_semaphore,
	down_write(_T->lock),
	up_write(_T->lock))

/* RW lock guards */
DEFINE_LOCK_GUARD_1(read_lock, rwlock_t,
	read_lock(_T->lock),
	read_unlock(_T->lock))

DEFINE_LOCK_GUARD_1(write_lock, rwlock_t,
	write_lock(_T->lock),
	write_unlock(_T->lock))

DEFINE_LOCK_GUARD_1(read_lock_irq, rwlock_t,
	read_lock_irq(_T->lock),
	read_unlock_irq(_T->lock))

DEFINE_LOCK_GUARD_1(write_lock_irq, rwlock_t,
	write_lock_irq(_T->lock),
	write_unlock_irq(_T->lock))

DEFINE_LOCK_GUARD_1(read_lock_irqsave, rwlock_t,
	read_lock_irqsave(_T->lock, _T->flags),
	read_unlock_irqrestore(_T->lock, _T->flags),
	unsigned long flags;)

DEFINE_LOCK_GUARD_1(write_lock_irqsave, rwlock_t,
	write_lock_irqsave(_T->lock, _T->flags),
	write_unlock_irqrestore(_T->lock, _T->flags),
	unsigned long flags;)

/*
 * RCU guard - lightweight version for read-side critical sections
 */
#include <linux/rcupdate.h>

DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())

#endif /* _LINUX_CLEANUP_H */
