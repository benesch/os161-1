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
 * Process ID managment.
 */

#ifndef _PID_H_
#define _PID_H_


#define INVALID_PID	0	/* nothing has this pid */
#define BOOTUP_PID	1	/* first thread has this pid */

/*
 * Initialize pid management.
 */
void pid_bootstrap(void);

/*
 * Get a pid for a new thread.
 */
int pid_alloc(pid_t *retval);

/*
 * Undo pid_alloc (may blow up if the target has ever run)
 */
void pid_unalloc(pid_t targetpid);

/*
 * Detach the thread associated with a pid. 
 */
int pid_detach(pid_t childpid);

/*
 * Set the exit status of the current thread to status.  Wake any 
 * threads waiting to read this status.
 */
void pid_exit(int status, bool dodetach);

/*
 * Return the exit status of the thread associated with targetpid as
 * soon as it is available.
 */
int pid_join(pid_t targetpid, int *status, int flags);


#endif /* _PID_H_ */
