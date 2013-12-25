/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process ID management.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/wait.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <clock.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <pid.h>
#include <signal.h>

/*
 * Structure for holding PID and return data for a thread.
 *
 * If pi_ppid is INVALID_PID, the parent has gone away and will not be
 * waiting. If pi_ppid is INVALID_PID and pi_exited is true, the
 * structure can be freed.
 */
struct pidinfo {
	pid_t pi_pid;			// process id of this thread
	pid_t pi_ppid;			// process id of parent thread
	volatile bool pi_exited;	// true if thread has exited
	int pi_exitstatus;		// status (only valid if exited)
	struct cv *pi_cv;		// use to wait for thread exit
    struct cv *pi_signal_cv; // use to wait for SIGCONT
    int waitingthreads;     // number of threads waiting
    volatile bool detached; // true if thread is detached
    /* if thread is signaled to be killed, set to terminating signal */
    int sigkill;
    volatile bool sigstop;  // true if thread is signaled to sleep
    volatile bool sigcont;  // true if thread is signaled to continue
};


/*
 * Global pid and exit data.
 *
 * The process table is an el-cheapo hash table. It's indexed by
 * (pid % PROCS_MAX), and only allows one process per slot. If a
 * new pid allocation would cause a hash collision, we just don't
 * use that pid.
 */
static struct lock *pidlock;		// lock for global exit data
static struct pidinfo *pidinfo[PROCS_MAX]; // actual pid info
static pid_t nextpid;			// next candidate pid
static int nprocs;			// number of allocated pids

/* Define signals that are valid. */
static int implemented_signals [8] = { SIGHUP, SIGINT, SIGKILL,
    SIGTERM, SIGSTOP, SIGCONT, SIGWINCH, SIGINFO };

/*
 * Create a pidinfo structure for the specified pid.
 */
static
struct pidinfo *
pidinfo_create(pid_t pid, pid_t ppid)
{
	struct pidinfo *pi;

	KASSERT(pid != INVALID_PID);

	pi = kmalloc(sizeof(struct pidinfo));
	if (pi==NULL) {
		return NULL;
	}

	pi->pi_cv = cv_create("pidinfo cv");
	if (pi->pi_cv == NULL) {
		kfree(pi);
		return NULL;
	}
    
    /* Initialize pi_signal_cv. */
    pi->pi_signal_cv = cv_create("pidinfo signal cv");
	if (pi->pi_signal_cv == NULL) {
		kfree(pi);
		return NULL;
	}

	pi->pi_pid = pid;
	pi->pi_ppid = ppid;
	pi->pi_exited = false;
	pi->pi_exitstatus = 0xbaad;  /* Recognizably invalid value */
    pi->detached = false;
    pi->sigkill = false;
    pi->sigstop = false;
    pi->sigcont = false;
    pi->waitingthreads = 0;

	return pi;
}

/*
 * Clean up a pidinfo structure.
 */
static
void
pidinfo_destroy(struct pidinfo *pi)
{
	KASSERT(pi->pi_exited == true);
	KASSERT(pi->pi_ppid == INVALID_PID);
	cv_destroy(pi->pi_cv);
	cv_destroy(pi->pi_signal_cv);
	kfree(pi);
}

////////////////////////////////////////////////////////////

/*
 * pid_bootstrap: initialize.
 */
void
pid_bootstrap(void)
{
	int i;

	pidlock = lock_create("pidlock");
	if (pidlock == NULL) {
		panic("Out of memory creating pid lock\n");
	}

	/* not really necessary - should start zeroed */
	for (i=0; i<PROCS_MAX; i++) {
		pidinfo[i] = NULL;
	}

	pidinfo[BOOTUP_PID] = pidinfo_create(BOOTUP_PID, INVALID_PID);
	if (pidinfo[BOOTUP_PID]==NULL) {
		panic("Out of memory creating bootup pid data\n");
	}

	nextpid = PID_MIN;
	nprocs = 1;
}

/*
 * pi_get: look up a pidinfo in the process table.
 */
static
struct pidinfo *
pi_get(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(pid>=0);
	KASSERT(pid != INVALID_PID);
	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	if (pi==NULL) {
		return NULL;
	}
	if (pi->pi_pid != pid) {
		return NULL;
	}
	return pi;
}

/*
 * pi_put: insert a new pidinfo in the process table. The right slot
 * must be empty.
 */
static
void
pi_put(pid_t pid, struct pidinfo *pi)
{
	KASSERT(lock_do_i_hold(pidlock));

	KASSERT(pid != INVALID_PID);

	KASSERT(pidinfo[pid % PROCS_MAX] == NULL);
	pidinfo[pid % PROCS_MAX] = pi;
	nprocs++;
}

/*
 * pi_drop: remove a pidinfo structure from the process table and free
 * it. It should reflect a process that has already exited and been
 * waited for.
 */
static
void
pi_drop(pid_t pid)
{
	struct pidinfo *pi;

	KASSERT(lock_do_i_hold(pidlock));

	pi = pidinfo[pid % PROCS_MAX];
	KASSERT(pi != NULL);
	KASSERT(pi->pi_pid == pid);

	pidinfo_destroy(pi);
	pidinfo[pid % PROCS_MAX] = NULL;
	nprocs--;

	DEBUG(DB_THREADS, "Dropped pidinfo for %d\n", pid);
}

////////////////////////////////////////////////////////////

/*
 * Helper function for pid_alloc.
 */
static
void
inc_nextpid(void)
{
	KASSERT(lock_do_i_hold(pidlock));

	nextpid++;
	if (nextpid > PID_MAX) {
		nextpid = PID_MIN;
	}
}

/*
 * Check whether ppid of a thread of given pid is the same as
 * the current thread's pid.
 * Returns -1 if such pid does not exist, 1 if comparison is true,
 * 0 otherwise.
 */
int
check_ppid(pid_t pid)
{
    struct pidinfo *my_pi;
    int retval;
    
    lock_acquire(pidlock);
    my_pi = pi_get(pid);
    
    if (my_pi == NULL) {
        return -1;
    } else if (my_pi->pi_ppid == curthread->t_pid) {
        retval = 1;
    } else {
        retval = 0;
    }
    
    lock_release(pidlock);
    return retval;
}

/*
 * pid_alloc: allocate a process id.
 */
int
pid_alloc(pid_t *retval)
{
	struct pidinfo *pi;
	pid_t pid;
	int count;

	KASSERT(curthread->t_pid != INVALID_PID);

	/* lock the table */
	lock_acquire(pidlock);

	if (nprocs == PROCS_MAX) {
		lock_release(pidlock);
		return EAGAIN;
	}

	/*
	 * The above test guarantees that this loop terminates, unless
	 * our nprocs count is off. Even so, assert we aren't looping
	 * forever.
	 */
	count = 0;
	while (pidinfo[nextpid % PROCS_MAX] != NULL) {

		/* avoid various boundary cases by allowing extra loops */
		KASSERT(count < PROCS_MAX*2+5);
		count++;

		inc_nextpid();
	}

	pid = nextpid;

	pi = pidinfo_create(pid, curthread->t_pid);
	if (pi==NULL) {
		lock_release(pidlock);
		return ENOMEM;
	}

	pi_put(pid, pi);

	inc_nextpid();

	lock_release(pidlock);

	*retval = pid;
	return 0;
}

/*
 * pid_unalloc - unallocate a process id (allocated with pid_alloc) that
 * hasn't run yet.
 */
void
pid_unalloc(pid_t theirpid)
{
	struct pidinfo *them;

	KASSERT(theirpid >= PID_MIN && theirpid <= PID_MAX);

	lock_acquire(pidlock);

	them = pi_get(theirpid);
	KASSERT(them != NULL);
	KASSERT(them->pi_exited == false);
	KASSERT(them->pi_ppid == curthread->t_pid);

	/* keep pidinfo_destroy from complaining */
	them->pi_exitstatus = 0xdead;
	them->pi_exited = true;
	them->pi_ppid = INVALID_PID;

	pi_drop(theirpid);

	lock_release(pidlock);
}

/*
 * pid_detach - disavows interest in the child thread's exit status, so 
 * it can be freed as soon as it exits. May only be called by the
 * parent thread. Returns 0 if the thread childpid is successfully placed
 * in the detached state.
 */
int
pid_detach(pid_t childpid)
{
    int err = 0;
    
    /* Check if childpid is valid. */
    if (childpid == INVALID_PID || childpid == BOOTUP_PID)
        return EINVAL;
    
    /* Obtain process information of childs thread. */
    struct pidinfo *pinfo;
    lock_acquire(pidlock);
    pinfo = pi_get(childpid);
    
    /* No thread associated with childpid could be found. */
    if (pinfo == NULL) {
        err = ESRCH;
        goto out;
    }
    
    /* Check the childpid for the following:
     * (1) The caller is not the parent of childpid
     * (2) childpid has been joined at another thread
     * (3) childpid is already in the detached state */
    if (pinfo->pi_ppid != curthread->t_pid ||
        pinfo->waitingthreads > 0 ||
        pinfo->detached) {
        err = EINVAL;
        goto out;
    }
    
    /* If thread already exited, drop associated pidinfo.
     * Otherwise, detach childpid by setting pi_ppid. */
    if (pinfo->pi_exited) {
        pi_drop(childpid);
    } else {
        pinfo->detached = true;
    }
        
out:
    lock_release(pidlock);
	return err;
}

/*
 * pid_exit 
 *  - sets the exit status of this thread (i.e. curthread). 
 *  - disowns children. 
 *  - if dodetach is true, children are also detached. 
 *  - wakes any thread waiting for the curthread to exit. 
 *  - frees the PID and exit status if the curthread has been detached. 
 *  - must be called only if the thread has had a pid assigned.
 */
void
pid_exit(int status, bool dodetach)
{
	struct pidinfo *my_pi;

	lock_acquire(pidlock);
	my_pi = pi_get(curthread->t_pid);
	KASSERT(my_pi != NULL && my_pi->pi_pid == curthread->t_pid);
    
    // Update the exit status of the current thread
    my_pi->pi_exited = true;
    my_pi->pi_exitstatus = status;
    
    // Disown children.
    pid_t pidindex = PID_MIN;
	while (pidindex <= PID_MAX) {
        struct pidinfo *pinfo = pi_get(pidindex);
        if (pinfo != NULL && pinfo->pi_ppid == my_pi->pi_pid) {
            pinfo->pi_ppid = INVALID_PID;
            if (dodetach)
                pinfo->detached = true;
        }
		pidindex++;
	}
    
    /* If current thread has been detached, discard the pid struct. */
    if (my_pi->detached) {
        my_pi->pi_ppid = INVALID_PID;
        pi_drop(my_pi->pi_pid);
    } else {
        // Wakes any thread waiting for current thread.
        cv_broadcast(my_pi->pi_cv, pidlock);
    }
    
	lock_release(pidlock);
}

/*
 * pid_join - Store the exit status of the thread associated with
 * targetpid (in the status argument) as soon as it is available. 
 * If the thread has not yet exited, curthread waits unless the flag 
 * WNOHANG is sent. Return the pid of the joined thread on success or
 * a negative error code otherwise.
 */
int
pid_join(pid_t targetpid, int *status, int flags)
{	
    int result = 0;
    
    /* Check if targetpid is valid. */
    if (targetpid == INVALID_PID || targetpid == BOOTUP_PID)
        return -1 * EINVAL;
    
    /* Check if targetpid is caller. */
    if (targetpid == curthread->t_pid)
        return -1 * EDEADLK;
    
    /* Obtain process information of target thread. */
    struct pidinfo *pinfo;
    lock_acquire(pidlock);
    pinfo = pi_get(targetpid);
    
    /* No thread associated with targetpid could be found. */
    if (pinfo == NULL) {
        result = -1 * ESRCH;
        goto out;
    }
    
    /* Check if target thread has been detached. */
    if (pinfo->detached) {
        result = -1 * EINVAL;
        goto out;
    }
    
    if (!pinfo->pi_exited) {
        /* Target thread has not exited. */
        if (flags == WNOHANG)
            goto out;
        else {
            /* Put thread to sleep. */
            pinfo->waitingthreads++;
            DEBUG(DB_THREADS, "Parent waiting for %d\n", targetpid);
            cv_wait(pinfo->pi_cv, pidlock);
            pinfo->waitingthreads--;
        }
    }
    
    /* Store the exit status of the target thread in the caller */
    if (status != NULL)
    	*status = pinfo->pi_exitstatus;

    result = targetpid;
    
    /* Clean up pidinfo if there is no other thread waiting. */
    if (pinfo->waitingthreads == 0) {
        pinfo->pi_ppid = INVALID_PID;
        pi_drop(targetpid);
    }
    
out:
    lock_release(pidlock);
	return result;
}

/*
 * pid_setsignal - Set the signal for a thread with given pid.
 * Store a return value of 0 to retval if no errors occur and -1
 * otherwise. Return the error code if an error occurs and 0 otherwise.
 */
int
pid_setsignal(pid_t targetpid, int signal, int *retval)
{
    int err = 0;
    
    /* Obtain process information of target thread. */
    struct pidinfo *pinfo;
    lock_acquire(pidlock);
    pinfo = pi_get(targetpid);
    
    /* No thread associated with targetpid could be found. */
    if (pinfo == NULL) {
        err = ESRCH;
        goto out;
    }
    
    /* Check if given signal is valid and implemented. */
    if (signal <= 0 || signal > 31) {
        /* Signal is invalid. */
        err = EINVAL;
        goto out;
    } else {
        volatile bool implemented = false;
        int size = sizeof(implemented_signals) / sizeof(int *);
        int i;
        for (i = 0; i < size; i++) {
            if (implemented_signals[i] == signal) {
                implemented = true;
                break;
            }
        }
        
        if (!implemented) {
            /* Signal is not implemented. */
            err = EUNIMP;
            goto out;
        }
    }

    switch (signal) {
        case SIGHUP:
        case SIGINT:
        case SIGKILL:
        case SIGTERM:
        	/* For signals that terminates a process, set the pid's
        	 * sigkill flag to the specified signal. */
            pinfo->sigkill = signal;
            break;
        case SIGSTOP:
            pinfo->sigstop = true;
            break;
        case SIGCONT:
        	/* Wake up the target thread if the thread was
        	 * put to sleep with SIGSTOP */
            if (pinfo->sigstop) {
                cv_signal(pinfo->pi_signal_cv, pidlock);
            }    
            break;
        default:
            break;
	}
    
out:
	if (err > 0) {
    	/* An error occurred. */
    	*retval = -1;
    } else {
    	*retval = 0;
    }

    lock_release(pidlock);
    return err;
}

/* Handle the signal for a thread with given pid. */
void
pid_handlesignal(pid_t pid)
{
	/* Obtain process information of target thread. */
	struct pidinfo *pinfo;
	lock_acquire(pidlock);
	pinfo = pi_get(pid);
    /* Check if the thread associated with pid was found */
    KASSERT(pinfo != NULL);

    /* Handle signal. */    
    if (pinfo->sigkill > 0) {
    	/* Order the thread to exit */
        lock_release(pidlock);
        thread_exit(_MKWAIT_SIG(pinfo->sigkill));
    } else if (pinfo->sigstop) {
        /* Order the thread to sleep, and set the sigstop
         * flag to false when woken up */
        cv_wait(pinfo->pi_signal_cv, pidlock);
        pinfo->sigstop = false;
    }

	lock_release(pidlock);
}

/*
 * Handle system call waitpid - the current process waits for the target
 * process targetpid to exit, and stores the exit status in the pointer status.
 * Store a return value of 0 to retval if no error occurs and -1 otherwise.
 * Return the error code if an error occurs and 0 otherwise.
 */
int
sys_waitpid(pid_t targetpid, int *status, int options, int *retval){
    int err;
    
    /* Check if flag is valid. */
    if (options != 0 && options != WNOHANG) {
        err = -1 * EINVAL;
        goto out;
    }
    
    /* Check if the current thread is waiting on one of its
     * own children. */
    int ischild = check_ppid(targetpid);
    if (ischild == -1) {
        /* targetpid process does not exist. */
        err = -1 * ESRCH;
        goto out;
    } else if (ischild == 0) {
        /* Current thread is not the parent of target thread. */
        err = -1 * ECHILD;
        goto out;
    }    
    
    err = pid_join(targetpid, status, options);

out:
    if (err < 0) {
        /* An error occurred. */
        err = -err;
        *retval = -1;
    } else {
        *retval = err;
        err = 0;
    }

    return err;
}
