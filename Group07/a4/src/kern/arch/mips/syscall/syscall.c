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

#include <types.h>
#include <kern/errno.h>
#include <kern/syscall.h>
#include <lib.h>
#include <mips/trapframe.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <kern/wait.h> /* New include of wait macros for _exit */
#include <copyinout.h> /* A4 SETUP - new include for lseek */
/*
 * System call dispatcher.
 *
 * A pointer to the trapframe created during exception entry (in
 * exception.S) is passed in.
 *
 * The calling conventions for syscalls are as follows: Like ordinary
 * function calls, the first 4 32-bit arguments are passed in the 4
 * argument registers a0-a3. 64-bit arguments are passed in *aligned*
 * pairs of registers, that is, either a0/a1 or a2/a3. This means that
 * if the first argument is 32-bit and the second is 64-bit, a1 is
 * unused.
 *
 * This much is the same as the calling conventions for ordinary
 * function calls. In addition, the system call number is passed in
 * the v0 register.
 *
 * On successful return, the return value is passed back in the v0
 * register, or v0 and v1 if 64-bit. This is also like an ordinary
 * function call, and additionally the a3 register is also set to 0 to
 * indicate success.
 *
 * On an error return, the error code is passed back in the v0
 * register, and the a3 register is set to 1 to indicate failure.
 * (Userlevel code takes care of storing the error code in errno and
 * returning the value -1 from the actual userlevel syscall function.
 * See src/user/lib/libc/arch/mips/syscalls-mips.S and related files.)
 *
 * Upon syscall return the program counter stored in the trapframe
 * must be incremented by one instruction; otherwise the exception
 * return code will restart the "syscall" instruction and the system
 * call will repeat forever.
 *
 * If you run out of registers (which happens quickly with 64-bit
 * values) further arguments must be fetched from the user-level
 * stack, starting at sp+16 to skip over the slots for the
 * registerized values, with copyin().
 */
void
syscall(struct trapframe *tf)
{
	int callno;
	int32_t retval;
	int err;
	/* BEGIN A4 SETUP */
	/* lseek uses a 64-bit argument, and has a 64-bit return type,
	 * which needs special handling.
	 */
	int whence;
	off_t pos;
	off_t retval64 = 0;
	/* END A4 SETUP */

	KASSERT(curthread != NULL);
	KASSERT(curthread->t_curspl == 0);
	KASSERT(curthread->t_iplhigh_count == 0);

	callno = tf->tf_v0;

	/*
	 * Initialize retval to 0. Many of the system calls don't
	 * really return a value, just 0 for success and -1 on
	 * error. Since retval is the value returned on success,
	 * initialize it to 0 by default; thus it's not necessary to
	 * deal with it except for calls that return other values, 
	 * like write.
	 */

	retval = 0;

	switch (callno) {
	    case SYS_reboot:
		    err = sys_reboot(tf->tf_a0);
		    break;

	    case SYS___time:
		    err = sys___time((userptr_t)tf->tf_a0,
				     (userptr_t)tf->tf_a1);
		    break;

            /* ASST1: These implementations of read and write only work for
             * console I/O (stdin, stdout and stderr file descriptors)
             */
            case SYS_read:
                err = sys_read(tf->tf_a0, (userptr_t)tf->tf_a1, tf->tf_a2,
                               &retval);
                break;

            case SYS_write:
                err = sys_write(tf->tf_a0, (userptr_t)tf->tf_a1, tf->tf_a2,
                                &retval);
                break;

	    /* process calls */
	
	    case SYS__exit:
		    DEBUG(DB_SYSCALL, "thread %d exiting with code %d\n",
			  curthread->t_pid, tf->tf_a0);
		    thread_exit(_MKWAIT_EXIT(tf->tf_a0));
		    panic("Returning from exit\n");

            case SYS_fork:
		    err = sys_fork(tf, &retval);
		    break;

            /* ASST1 - You need to fill in the code for each of these cases */
            case SYS_getpid:
            case SYS_waitpid:
            case SYS_kill:
		kprintf("Unimplemented A2 syscall %d\n", callno);
		err = ENOSYS;
		break;

	    /* Even more system calls will go here */

	    /* BEGIN A4 SETUP */
           
            /* Note: SYS_read and SYS_write are above, from A1 starter code.*/
		 
	    case SYS_open:
		err = sys_open((userptr_t)tf->tf_a0, tf->tf_a1, tf->tf_a2, 
			       &retval);
		break;
	    case SYS_close:
		err = sys_close(tf->tf_a0);
		break;
	    case SYS_dup2:
		err = sys_dup2(tf->tf_a0, tf->tf_a1, &retval);
		break;
	    case SYS_lseek:
		    /* Ouch ... off_t is 64-bit, so need a2/a3 register
		     * pair to get the "pos" argument and need to get 
		     * last argument "whence" off the user stack with 
		     * copyout.
		     */
		pos = ((off_t)tf->tf_a2 << 32) | tf->tf_a3;
		err = copyin((userptr_t)(tf->tf_sp+16), &whence, sizeof(int));
		if (err) {
			break;
		}
		err = sys_lseek(tf->tf_a0, pos, whence, &retval64);
		break;
	    case SYS_mkdir:
		 err = sys_mkdir((userptr_t)tf->tf_a0, tf->tf_a1);
		 break;
	    case SYS_rmdir:
		 err = sys_rmdir((userptr_t)tf->tf_a0);
		 break;
	    case SYS_chdir:
		err = sys_chdir((userptr_t)tf->tf_a0);
		break;
	    case SYS___getcwd:
		err = sys___getcwd((userptr_t)tf->tf_a0, tf->tf_a1, &retval);
		break;
	    case SYS_fstat:
		err = sys_fstat(tf->tf_a0, (userptr_t)tf->tf_a1);
		break;
	    case SYS_getdirentry:
		err = sys_getdirentry(tf->tf_a0, (userptr_t)tf->tf_a1, 
				      tf->tf_a2, &retval);
		break;
	    
	    /* END A4 SETUP */
 
	    default:
		kprintf("Unknown syscall %d\n", callno);
		err = ENOSYS;
		break;
	}


	if (err) {
		/*
		 * Return the error code. This gets converted at
		 * userlevel to a return value of -1 and the error
		 * code in errno.
		 */
		tf->tf_v0 = err;
		tf->tf_a3 = 1;      /* signal an error */
	}
	else {
		/* Success. */
		/* BEGIN A4 SETUP */
		/* lseek needs to return a 64-bit result in v0 and v1 */
		if (retval64 != 0) {
			tf->tf_v0 = (int)(retval64 >> 32); /* high bits */
			tf->tf_v1 = (int)(retval64 & 0xffffffff); /* low bits */
		} else {
			tf->tf_v0 = retval;
		}
		/* END A4 SETUP */
		tf->tf_a3 = 0;      /* signal no error */
	}
	
	/*
	 * Now, advance the program counter, to avoid restarting
	 * the syscall over and over again.
	 */
	
	tf->tf_epc += 4;

	/* Make sure the syscall code didn't forget to lower spl */
	KASSERT(curthread->t_curspl == 0);
	/* ...or leak any spinlocks */
	KASSERT(curthread->t_iplhigh_count == 0);
}

/*
 * Enter user mode for a newly forked process.
 *
 * This function is provided as a reminder. You need to write
 * both it and the code that calls it.
 *
 * Thus, you can trash it and do things another way if you prefer.
 */
void
enter_forked_process(void *data1, unsigned long unused)
{
	struct trapframe local_tf;
	(void)unused;

	/* Copy the trapframe passed in onto the current thread's stack */
	local_tf = *(struct trapframe *)data1;

	/* And free the kernel memory from the trapframe passed in */
	kfree(data1);

	/*
	 * Advance the program counter, to avoid restarting
	 * the syscall over and over again in the child.
	 */
	local_tf.tf_epc += 4;
	local_tf.tf_a3 = 0; /* Success if the child gets here */
	local_tf.tf_v0 = 0; /* return 0 to the child */

	mips_usermode(&local_tf);
}
