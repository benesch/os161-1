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

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vmprivate.h>
#include <machine/coremap.h>
#include <mainbus.h>

#include "opt-randpage.h"
#include "opt-randtlb.h"


/*
 * Machine-dependent VM stuff that isn't directly coremap-related.
 */


/*
 * vm_bootstrap
 *
 * Begin VM system initialization.  Creates the coremap, which allows
 * kmalloc to be called.
 * 
 * Synchronization: none. Runs at boot.
 */
void
vm_bootstrap(void)
{

#if OPT_RANDPAGE
	kprintf("vm: Page replacement: random\n");
#else
	kprintf("vm: Page replacement: sequential\n");
#endif

#if OPT_RANDTLB
	kprintf("vm: TLB replacement: random\n");
#else
	kprintf("vm: TLB replacement: sequential\n");
#endif

	coremap_bootstrap();

	global_paging_lock = lock_create("global_paging_lock");
}

/*
 * vm_fault: TLB fault handler. Hands off to the current thread's
 * address space.
 *
 * Synchronization: none.
 */
int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	struct addrspace *as;

	faultaddress &= PAGE_FRAME;
	KASSERT(faultaddress < MIPS_KSEG0);

	as = curthread->t_addrspace;
	if (as == NULL) {
		return EFAULT;
	}

	return as_fault(as, faulttype, faultaddress);
}

