/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2009
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
 * Test code for coremap page allocation.
 */
#include <types.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <test.h>
#include <vm.h>
#include <machine/coremap.h>

/*
 * Test alloc_kpages; allocate NPAGES pages NTRIES times, freeing
 * somewhat later.
 *
 * The total of NPAGES * NTRIES is intended to exceed the size of
 * available memory.
 *
 * coremapstress does the same thing, but from NTHREADS different
 * threads at once.
 */

#define NTRIES   1200
#define NPAGES    3
#define NTHREADS  8

static
void
coremapthread(void *sm, unsigned long num)
{
	struct semaphore *sem = sm;
	uint32_t page;
	uint32_t oldpage = 0;
	uint32_t oldpage2 = 0;
	int i;

	for (i=0; i<NTRIES; i++) {
		page = alloc_kpages(NPAGES);
		if (page==0) {
			if (sem) {
				kprintf("thread %lu: alloc_kpages failed\n",
					num);
				V(sem);
				return;
			}
			kprintf("alloc_kpages failed; test failed.\n");
			return;
		}
		if (oldpage2) {
			free_kpages(oldpage2);
		}
		oldpage2 = oldpage;
		oldpage = page;
	}
	if (oldpage2) {
		free_kpages(oldpage2);
	}
	if (oldpage) {
		free_kpages(oldpage);
	}
	if (sem) {
		V(sem);
	}
}

int
coremaptest(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kprintf("Starting kcoremap test...\n");
	coremapthread(NULL, 0);
	kprintf("kcoremap test done\n");

	return 0;
}

int
coremapstress(int nargs, char **args)
{
	struct semaphore *sem;
	int i, err;

	(void)nargs;
	(void)args;

	sem = sem_create("coremapstress", 0);
	if (sem == NULL) {
		panic("coremapstress: sem_create failed\n");
	}

	kprintf("Starting kcoremap stress test...\n");

	for (i=0; i<NTHREADS; i++) {
		err = thread_fork("coremapstress", coremapthread, sem, i,
				  NULL);
		if (err) {
			panic("coremapstress: thread_fork failed (%d)\n", err);
		}
	}

	for (i=0; i<NTHREADS; i++) {
		P(sem);
	}

	sem_destroy(sem);
	kprintf("kcoremap stress test done\n");

	return 0;
}
