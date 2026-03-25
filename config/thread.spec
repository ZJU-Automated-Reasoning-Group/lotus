# Thread API Configuration File (Goblint-style)
# Line format: FunctionName ThreadPrimitive [optional arg indices]
#
# Primitives: TD_FORK, TD_JOIN, TD_DETACH, TD_EXIT, TD_CANCEL,
#   TD_ACQUIRE, TD_TRY_ACQUIRE, TD_RELEASE,
#   TD_COND_WAIT, TD_COND_SIGNAL, TD_COND_BROADCAST,
#   TD_MUTEX_INI, TD_MUTEX_DESTROY, TD_CONDVAR_INI, TD_CONDVAR_DESTROY,
#   TD_BAR_INIT, TD_BAR_WAIT,
#   TD_KERNEL_SPIN_LOCK, TD_KERNEL_SPIN_UNLOCK,
#   TD_KERNEL_MUTEX_LOCK, TD_KERNEL_MUTEX_UNLOCK
#
# Optional argument indices (0-based) override defaults for custom APIs:
#   TD_FORK: thread_arg start_routine_arg user_arg   (default: 0 2 3, pthread_create)
#   TD_JOIN: thread_arg ret_arg                     (default: 0 1, pthread_join)
# Example: kthread_run has (thread, fn, arg) at 0,1,2 so use "kthread_run TD_FORK 0 1 2"

# Linux Kernel
mutex_lock TD_KERNEL_MUTEX_LOCK
mutex_unlock TD_KERNEL_MUTEX_UNLOCK
spin_lock TD_KERNEL_SPIN_LOCK
spin_unlock TD_KERNEL_SPIN_UNLOCK
kthread_run TD_FORK 0 1 2
kthread_stop TD_JOIN 0 1
schedule TD_DUMMY

# GNU OpenMP runtime
GOMP_parallel TD_FORK 99 0 1
GOMP_parallel_start TD_FORK 99 0 1
