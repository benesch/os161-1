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
 * vmprivate.h - private VM system definitions.
 *
 * This file is not meant to be included by files that are not part of
 * the VM system. It contains definitions of VM-internal data structures
 * and functions.
 *
 * Everything in here is machine-independent. Machine-dependent stuff can
 * be found in <machine/coremap.h>.
 */

#ifndef _VMPRIVATE_H_
#define _VMPRIVATE_H_

#include <array.h>
#include <spinlock.h>
struct addrspace;

#include "opt-dumbvm.h"
#if !OPT_DUMBVM

/*
 * The VM system is designed around a coremap to keep track of
 * physical memory, and vm_objects and lpages within addrspace structs
 * to hold process-specific virtual-to-physical mappings.
 *
 * The set of vm_objects contained in each addrspace is loosely
 * equivalent to a sparse page-table.
 */

////////////////////////////////////////////////////////////
//
// lpage: logical page - per-virtual-page structure.
//

/*
 * Each distinct page handled by the VM system is assigned an lpage
 * structure. The lpage keeps track of where the page is in physical
 * memory (lp_paddr) and where it is kept on disk in the swapfile
 * (lp_swapaddr). If the page is not in RAM, lp_paddr is INVALID_PADDR.
 * If no swap has been allocated, lp_swapaddr is INVALID_SWAPADDR.
 *
 * It is assumed that the physical page size is at least 1k or so
 * (most MMUs use at least 4k), so the low bits of lp_paddr are used
 * to hold flags.
 *
 *     LPF_DIRTY    is set if the page has been modified.
 *     LPF_PINNED   is set if the page is in transit to/from disk.
 *
 * A vm_object contains an array of lpages, each of which corresponds
 * to a virtual page in the address space of a process.
 *
 * It is assumed in the solution set VM that lpages are never shared
 * between processes.
 */

struct lpage {
	volatile paddr_t lp_paddr;
	off_t lp_swapaddr;
	struct spinlock lp_spinlock;
};

/* lpage flags */
#define LPF_DIRTY		0x1
#define LPF_MASK		0x1	// mask for the above

#define LP_ISDIRTY(lp)		((lp)->lp_paddr & LPF_DIRTY)

#define LP_SET(am, bit)		((lp)->lp_paddr |= (bit))
#define LP_CLEAR(am, bit)	((lp)->lp_paddr &= ~(paddr_t)(bit))

/*
 * Functions in lpage.c
 *
 *    lpage_create - create a blank, non-materialized lpage structure.
 *    lpage_destroy - destroy an lpage
 *    lpage_lock/unlock - for exclusive access to an lpage
 *    lpage_lock_and_pin - also pin physical page (see lpage.c for details)
 *
 *    lpage_copy - clone an lpage, including the contents
 *    lpage_zerofill - materialize an lpage and zero-fill it
 *    lpage_fault - handle a fault on an lpage
 *    lpage_evict - evict an lpage
 */
struct lpage     *lpage_create(void);
void              lpage_destroy(struct lpage *lp);
void              lpage_lock(struct lpage *lp);
void              lpage_unlock(struct lpage *lp);
void              lpage_lock_and_pin(struct lpage *lp);

int	              lpage_copy(struct lpage *from, struct lpage **toret);
int               lpage_zerofill(struct lpage **lpret);
int               lpage_fault(struct lpage *lp, struct addrspace *,
			                  int faulttype, vaddr_t va);
void              lpage_evict(struct lpage *victim);

////////////////////////////////////////////////////////////
//
// vm_object - block of virtual memory
//

DECLARRAY_BYTYPE(lpage_array, struct lpage);

/*
 * vm_object - data structure associated with a mapped (that is, valid)
 * block of process virtual memory.
 *
 * Each vm object contains an array of lpages and a base address. It
 * also allows a redzone on the lower end in which other vm_objects are
 * not allowed to fall. This is used to implement a guard band under the
 * stack.
 */
struct vm_object {
	struct lpage_array *vmo_lpages;
	vaddr_t vmo_base;
	size_t vmo_lower_redzone;
};

/*
 * vm_object operations in vmobj.c:
 * 
 * vm_object_create:  allocates a blank vm_object with the requested
 *                    number of struct lpage's set for zero-fill.
 * vm_object_copy:    clone a vm_object, as at fork time.
 * vm_object_setsize: adjust the size of a vm_object (either up or down).
 * vm_object_destroy: frees all the mapping entries and swap space.
 *
 */
struct vm_object 	*vm_object_create(size_t npages);
int			        vm_object_copy(struct vm_object *vmo,
					               struct addrspace *newas,
					               struct vm_object **newvmo_ret);
int                 vm_object_setsize(struct addrspace *as,
					                  struct vm_object *vmo,
					                  unsigned newnpages);
void 			 vm_object_destroy(struct addrspace *as, 
					               struct vm_object *vmo);

////////////////////////////////////////////////////////////
//
// swap
//

/*
 * swapfile operations in swap.c:
 *
 * swap_bootstrap:   bootstraps the swapfile and completes VM-system 
 *                   bootstrap. Declared in vm.h.
 *
 * swap_shutdown:    closes the swapfile vnode. Declared in vm.h.
 * 
 * swap_alloc:       finds a free swap page and marks it as used.
 *                   A page should have been previously reserved.
 *
 * swap_free:        unmarks a swap page.
 *
 * swap_reserve:     reserve some swap pages for future allocation.
 *
 * swap_unreserve:   release some previously-reserved swap pages.
 *
 * swap_pagein:      Reads a page from the requested swap address 
 *                   into the requested physical page.
 *
 * swap_pageout:     Writes a page to the requested swap address 
 *                   from the requested physical page.
 */

off_t	 	swap_alloc(void);
void 		swap_free(off_t diskpage);

int		swap_reserve(unsigned long npages);
void		swap_unreserve(unsigned long npages);

void 		swap_pagein(paddr_t paddr, off_t swapaddr);
void 		swap_pageout(paddr_t paddr, off_t swapaddr);

/*
 * Special disk address:
 *   INVALID_SWAPADDR is an invalid swap address.
 */
#define INVALID_SWAPADDR	(0)

/*
 * Global lock for paging. Only one page can be in transit at a time
 * (at least under current circumstances) so we get this at a fairly
 * high level to try to improve paging decisions.
 */
extern struct lock *global_paging_lock;

////////////////////////////////////////////////////////////
//
// other bits
//

/* Print machine-dependent VM counters */
void vm_printmdstats(void);

#endif /* !OPT_DUMBVM */
#endif /* _VMPRIVATE_H_ */
