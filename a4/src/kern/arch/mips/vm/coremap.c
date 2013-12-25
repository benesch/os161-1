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
#include <kern/stat.h>
#include <kern/unistd.h>
#include <lib.h>
#include <uio.h>
#include <spinlock.h>
#include <wchan.h>
#include <cpu.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <vm.h>
#include <vmprivate.h>
#include <addrspace.h>
#include <machine/coremap.h>
#include <machine/tlb.h>
#include <vfs.h>
#include <vnode.h>

#include "opt-randpage.h"
#include "opt-randtlb.h"


/*
 * MIPS coremap/MMU-control implementation.
 *
 * The MIPS has a completely software-refilled TLB. It doesn't define
 * hardware-level pagetables. Thus, for simplicity, we won't use any.
 * (In real life, one might anyway, to allow faster TLB refill
 * handling.)
 *
 * We have one coremap_entry per page of physical RAM. This is absolute
 * overhead, so it's important to keep it small - if it's overweight
 * adding more memory won't help.
 */


/*
 * The coremap allocation functions make sure that there are at least 8
 * non-kernel pages available in memory.
 */
#define CM_MIN_SLACK		8


/*
 * Coremap entry structure.
 */

struct coremap_entry {
	struct lpage *cm_lpage;	/* logical page we hold, or NULL */

	volatile
	int cm_tlbix:7;		/* tlb index number, or -1 */
	unsigned cm_cpunum:5;	/* cpu number for cm_tlbix */

	unsigned cm_kernel:1,	/* true if kernel page */
		cm_notlast:1,	/* true not last in sequence of kernel pages */
		cm_allocated:1;	/* true if page in use (user or kernel) */
	volatile 
	unsigned cm_pinned:1;	/* true if page is busy */
};

#define COREMAP_TO_PADDR(i)	(((paddr_t)PAGE_SIZE)*((i)+base_coremap_page))
#define PADDR_TO_COREMAP(page)	(((page)/PAGE_SIZE) - base_coremap_page)

////////////////////////////////////////////////////////////
//
// Variables
//

static struct spinlock coremap_spinlock = SPINLOCK_INITIALIZER;

/*
 * Use one wchan for all page-pin waiting. There shouldn't be that
 * much of it or very many threads at once. Also use one wchan for all
 * TLB shootdown waiting. This is less justifiable - it maybe ought to
 * be per-CPU.
 */
static struct wchan *coremap_pinchan;
static struct wchan *coremap_shootchan;

static uint32_t num_coremap_entries;
static uint32_t num_coremap_kernel;	/* pages allocated to the kernel */
static uint32_t num_coremap_user;	/* pages allocated to user progs */
static uint32_t num_coremap_free;	/* pages not allocated at all */
static uint32_t base_coremap_page;
static struct coremap_entry *coremap;

static volatile uint32_t ct_shootdowns_sent;
static volatile uint32_t ct_shootdowns_done;
static volatile uint32_t ct_shootdown_interrupts;

////////////////////////////////////////////////////////////
//
// Per-CPU data

void
cpu_vm_machdep_init(struct cpu_vm_machdep *cvm)
{
	cvm->cvm_lastas = NULL;
	cvm->cvm_nexttlb = 0;
	cvm->cvm_tlbseqslot = 0;
}

void
cpu_vm_machdep_cleanup(struct cpu_vm_machdep *cvm)
{
	(void)cvm;
	/* nothing */
}

////////////////////////////////////////////////////////////
//
// Stats

void
vm_printmdstats(void)
{
	uint32_t ss, sd, si;

	spinlock_acquire(&coremap_spinlock);
	ss = ct_shootdowns_sent;
	sd = ct_shootdowns_done;
	si = ct_shootdown_interrupts;
	spinlock_release(&coremap_spinlock);

	kprintf("vm: shootdowns: %lu sent, %lu done (%lu interrupts)\n",
		(unsigned long) ss, (unsigned long) sd, (unsigned long) si);
}

////////////////////////////////////////////////////////////
//
// TLB handling

/*
 * tlb_replace - TLB replacement algorithm. Returns index of TLB entry
 * to replace.
 *
 * Synchronization: assumes we hold coremap_spinlock. Does not block.
 */
static
uint32_t 
tlb_replace(void) 
{
	KASSERT(spinlock_do_i_hold(&coremap_spinlock));

#if OPT_RANDTLB
	/* random */
	return random() % NUM_TLB;
#else
	/* sequential */
	uint32_t slot = curcpu->c_vm.cvm_tlbseqslot;
	curcpu->c_vm.cvm_tlbseqslot = (slot + 1) % NUM_TLB;
	return slot;
#endif
}

/*
 * tlb_invalidate: marks a given tlb entry as invalid.
 *
 * Synchronization: assumes we hold coremap_spinlock. Does not block.
 */
static
void
tlb_invalidate(int tlbix)
{
	uint32_t elo, ehi;
	paddr_t pa;
	unsigned cmix;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));

	tlb_read(&ehi, &elo, tlbix);
	if (elo & TLBLO_VALID) {
		pa = elo & TLBLO_PPAGE;
		cmix = PADDR_TO_COREMAP(pa);
		KASSERT(cmix < num_coremap_entries);
		KASSERT(coremap[cmix].cm_tlbix == tlbix);
		KASSERT(coremap[cmix].cm_cpunum == curcpu->c_number);
		coremap[cmix].cm_tlbix = -1;
		coremap[cmix].cm_cpunum = 0;
		DEBUG(DB_TLB, "... pa 0x%05lx --> tlb --\n", 
			(unsigned long) COREMAP_TO_PADDR(cmix));
	}

	tlb_write(TLBHI_INVALID(tlbix), TLBLO_INVALID(), tlbix);
	DEBUG(DB_TLB, "... pa ------- <-- tlb %d\n", tlbix);
}

/*
 * tlb_clear: flushes the TLB by loading it with invalid entries.
 *
 * Synchronization: assumes we hold coremap_spinlock. Does not block.
 */
static
void
tlb_clear(void)
{
	int i;	

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));
	for (i=0; i<NUM_TLB; i++) {
		tlb_invalidate(i);
	}
	curcpu->c_vm.cvm_nexttlb = 0;
}

/*
 * Do one TLB shootdown.
 */
void
vm_tlbshootdown(const struct tlbshootdown *ts, int num)
{
	int i;
	int tlbix;
	unsigned where;

	spinlock_acquire(&coremap_spinlock);
	ct_shootdown_interrupts++;
	for (i=0; i<num; i++) {
		tlbix = ts[i].ts_tlbix;
		where = ts[i].ts_coremapindex;
		if (coremap[where].cm_tlbix == tlbix &&
		    coremap[where].cm_cpunum == curcpu->c_number) {
			tlb_invalidate(tlbix);
			ct_shootdowns_done++;
		}
	}
	wchan_wakeall(coremap_shootchan);
	spinlock_release(&coremap_spinlock);
}

/*
 * Shoot down everything.
 */
void
vm_tlbshootdown_all(void)
{
	spinlock_acquire(&coremap_spinlock);
	ct_shootdown_interrupts++;
	tlb_clear();
	ct_shootdowns_done += NUM_TLB;
	wchan_wakeall(coremap_shootchan);
	spinlock_release(&coremap_spinlock);
}

/*
 * Wait for shootdown to complete.
 */
static
void
tlb_shootwait(void)
{
	wchan_lock(coremap_shootchan);
	spinlock_release(&coremap_spinlock);
	wchan_sleep(coremap_shootchan);
	spinlock_acquire(&coremap_spinlock);
}

/*
 * tlb_unmap: Searches the TLB for a vaddr translation and invalidates
 * it if it exists.
 *
 * Synchronization: assumes we hold coremap_spinlock. Does not block. 
 */
static
void
tlb_unmap(vaddr_t va)
{
	int i;
	uint32_t elo = 0, ehi = 0;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));

	KASSERT(va < MIPS_KSEG0);

	i = tlb_probe(va & PAGE_FRAME,0);
	if (i < 0) {
		return;
	}
	
	tlb_read(&ehi, &elo, i);
	
	KASSERT(elo & TLBLO_VALID);
	
	DEBUG(DB_TLB, "invalidating tlb slot %d (va: 0x%x)\n", i, va); 
	
	tlb_invalidate(i);
}

/*
 * mipstlb_getslot: get a TLB slot for use, replacing an existing one if
 * necessary and peforming any at-replacement actions.
 */
static
int
mipstlb_getslot(void)
{
	int i;

	if (curcpu->c_vm.cvm_nexttlb < NUM_TLB) {
		return curcpu->c_vm.cvm_nexttlb++;
	}

	/* no space... need to evict */
	i = tlb_replace();
	tlb_invalidate(i);
	return i;
}

////////////////////////////////////////////////////////////
//
// Page replacement code
//

/*
 * To evict a page, it must be non-kernel and non-pinned.
 *
 * page_replace() takes no arguments and returns an index into the
 * coremap (for the selected victim page).
 */

#if OPT_RANDPAGE

/*
 * Random page replacement.
 *
 * Repeatedly generates a random index into the coremap until the 
 * selected page is not pinned and does not belong to the kernel.
 */
static
uint32_t 
page_replace(void)
{
    // Complete this function.
	return 0;
}

#else /* not OPT_RANDPAGE */

/*
 * Sequential page replacement.
 *
 * Selects pages to be evicted from the coremap sequentially. Skips
 * pages that are pinned or that belong to the kernel.
 */

static
uint32_t
page_replace(void)
{
	// Complete this function.
	return 0;
}

#endif /* OPT_RANDPAGE */


////////////////////////////////////////////////////////////
//
// Setup/initialization
// 

/*
 * coremap_bootstrap
 *
 * Because of the way ram.c works, after ram_getsize() is called,
 * ram_stealmem() cannot be called any longer. This means we cannot
 * call kmalloc between calling ram_getsize and setting things up
 * properly so that kmalloc can use the coremap logic for allocation.
 *
 * What this in turn means is that we cannot use kmalloc to allocate
 * the coremap; we steal space for it ourselves by hand.
 *
 * The coremap does not manage the kernel load image itself; the kernel
 * load image is assumed to be fixed for all eternity.
 *
 * Synchronization: none; runs early in boot.
 */

void
coremap_bootstrap(void)
{
	uint32_t i;
	paddr_t first, last;
	uint32_t npages, coremapsize;

	ram_getsize(&first, &last);

	/* The way ram.c works, these should be page-aligned */
	KASSERT((first & PAGE_FRAME) == first);
	KASSERT((last & PAGE_FRAME) == last);

	npages = (last - first) / PAGE_SIZE;

	DEBUG(DB_VM, "coremap: first: 0x%x, last 0x%x: %u pages\n",
	      first, last, npages);

	/*
	 * The coremap contains one coremap_entry per page.  Because
	 * of the allocation constraints here, it must be rounded up
	 * to a whole number of pages.
	 * 
	 * Note that while we don't need space for coremap entries for
	 * the coremap pages, and could save a few slots that way, the
	 * operating regime of OS/161 is such that we're unlikely to
	 * need more than one page for the coremap, and certainly not
	 * more than two. So for simplicity (and robustness) we'll
	 * avoid the relaxation computations necessary to optimize the
	 * coremap size.
	 */
	coremapsize = npages * sizeof(struct coremap_entry);
	coremapsize = ROUNDUP(coremapsize, PAGE_SIZE);
	KASSERT((coremapsize & PAGE_FRAME) == coremapsize);

	/*
	 * Steal pages for the coremap.
	 */
	coremap = (struct coremap_entry *) PADDR_TO_KVADDR(first);
	first += coremapsize;

	if (first >= last) {
		/* This cannot happen unless coremap_entry gets really huge */
		panic("vm: coremap took up all of physical memory?\n");
	}

	/*
	 * Now, set things up to reflect the range of memory we're
	 * managing. Note that we skip the pages the coremap is using.
	 */
	base_coremap_page = first / PAGE_SIZE;
	num_coremap_entries = (last / PAGE_SIZE) - base_coremap_page;
	num_coremap_kernel = 0;
	num_coremap_user = 0;
	num_coremap_free = num_coremap_entries;

	KASSERT(num_coremap_entries + (coremapsize/PAGE_SIZE) == npages);

	/*
	 * Initialize the coremap entries.
	 */
	for (i=0; i < num_coremap_entries; i++) {
		coremap[i].cm_kernel = 0;
		coremap[i].cm_notlast = 0;
		coremap[i].cm_allocated = 0;
		coremap[i].cm_pinned = 0;
		coremap[i].cm_tlbix = -1;
		coremap[i].cm_cpunum = 0;
		coremap[i].cm_lpage = NULL;
	}

	coremap_pinchan = wchan_create("vmpin");
	coremap_shootchan = wchan_create("tlbshoot");
	if (coremap_pinchan == NULL || coremap_shootchan == NULL) {
		panic("Failed allocating coremap wchans\n");
	}
}	

////////////////////////////////////////////////////////////
//
// Memory allocation
//

static
int
piggish_kernel(int proposed_kernel_pages)
{
	uint32_t nkp;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));

	nkp = num_coremap_kernel + proposed_kernel_pages ;
	if (nkp >= num_coremap_entries - CM_MIN_SLACK) {
		return 1;
	}
	return 0;
}

static
void
do_evict(int where)
{
	struct lpage *lp;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));
	KASSERT(curthread != NULL && !curthread->t_in_interrupt);
	KASSERT(lock_do_i_hold(global_paging_lock));

	KASSERT(coremap[where].cm_pinned==0);
	KASSERT(coremap[where].cm_allocated);
	KASSERT(coremap[where].cm_kernel==0);

	lp = coremap[where].cm_lpage;
	KASSERT(lp != NULL);

	/*
	 * Pin it now, so it doesn't get e.g. paged out by someone
	 * else while we're waiting for TLB shootdown.
	 */
	coremap[where].cm_pinned = 1;

	if (coremap[where].cm_tlbix >= 0) {
		if (coremap[where].cm_cpunum != curcpu->c_number) {
			/* yay, TLB shootdown */
			struct tlbshootdown ts;
			ts.ts_tlbix = coremap[where].cm_tlbix;
			ts.ts_coremapindex = where;
			ct_shootdowns_sent++;
			ipi_tlbshootdown(coremap[where].cm_cpunum, &ts);
			while (coremap[where].cm_tlbix != -1) {
				tlb_shootwait();
			}
			KASSERT(coremap[where].cm_tlbix == -1);
			KASSERT(coremap[where].cm_cpunum == 0);
			KASSERT(coremap[where].cm_lpage == lp);
		}
		else {
			tlb_invalidate(coremap[where].cm_tlbix);
			coremap[where].cm_tlbix = -1;
			coremap[where].cm_cpunum = 0;
		}
		DEBUG(DB_TLB, "... pa 0x%05lx --> tlb --\n", 
		      (unsigned long) COREMAP_TO_PADDR(where));
	}

	/* properly we ought to lock the lpage to test this */
	KASSERT(COREMAP_TO_PADDR(where) == (lp->lp_paddr & PAGE_FRAME));

	/* release the coremap spinlock in case we need to swap out */
	spinlock_release(&coremap_spinlock);

	lpage_evict(lp);

	spinlock_acquire(&coremap_spinlock);

	/* because the page is pinned these shouldn't have changed */
	KASSERT(coremap[where].cm_allocated == 1);
	KASSERT(coremap[where].cm_lpage == lp);
	KASSERT(coremap[where].cm_pinned == 1);

	coremap[where].cm_allocated = 0;
	coremap[where].cm_lpage = NULL;
	coremap[where].cm_pinned = 0;

	num_coremap_user--;
	num_coremap_free++;
	KASSERT(num_coremap_kernel+num_coremap_user+num_coremap_free
	       == num_coremap_entries);

	wchan_wakeall(coremap_pinchan);
}

static
int
do_page_replace(void)
{
	int where;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));
	KASSERT(lock_do_i_hold(global_paging_lock));

	where = page_replace();

	KASSERT(coremap[where].cm_pinned==0);
	KASSERT(coremap[where].cm_kernel==0);

	if (coremap[where].cm_allocated) {
		KASSERT(coremap[where].cm_lpage != NULL);
		KASSERT(curthread != NULL && !curthread->t_in_interrupt);
		do_evict(where);
	}

	return where;
}

static
void
mark_pages_allocated(int start, int npages, int dopin, int iskern)
{
	int i;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));
	for (i=start; i<start+npages; i++) {
		KASSERT(coremap[i].cm_pinned==0);
		KASSERT(coremap[i].cm_allocated==0);
		KASSERT(coremap[i].cm_kernel==0);
		KASSERT(coremap[i].cm_lpage==NULL);
		KASSERT(coremap[i].cm_tlbix<0);
		KASSERT(coremap[i].cm_cpunum == 0);

		if (dopin) {
			coremap[i].cm_pinned = 1;
		}
		coremap[i].cm_allocated = 1;
		if (iskern) {
			coremap[i].cm_kernel = 1;
		}

		if (i < start+npages-1) {
			coremap[i].cm_notlast = 1;
		}
	}
	if (iskern) {
		num_coremap_kernel += npages;
	}
	else {
		num_coremap_user += npages;
	}
	num_coremap_free -= npages;
	KASSERT(num_coremap_kernel+num_coremap_user+num_coremap_free
	       == num_coremap_entries);
}

/*
 * coremap_alloc_one_page
 *
 * Allocate one page of memory, mark it pinned if requested, and
 * return its paddr. The page is marked a kernel page iff the lp
 * argument is NULL.
 */
static
paddr_t
coremap_alloc_one_page(struct lpage *lp, int dopin)
{
	int candidate, i, iskern;

	iskern = (lp == NULL);

	/*
	 * Hold this while allocating to reduce starvation of multipage
	 * allocations. (But we can't if we're in an interrupt, or if
	 * we're still very early in boot.)
	 */
	if (curthread != NULL && !curthread->t_in_interrupt) {
		lock_acquire(global_paging_lock);
	}

	spinlock_acquire(&coremap_spinlock);

	/*
	 * Don't allow the kernel to eat everything.
	 */
	if (iskern && piggish_kernel(1)) {
		coremap_print_short();
		spinlock_release(&coremap_spinlock);
		if (curthread != NULL && !curthread->t_in_interrupt) {
			lock_release(global_paging_lock);
		}
		kprintf("alloc_kpages: kernel heap full getting 1 page\n");
		return INVALID_PADDR;
	}

	/*
	 * For single-page allocations, start at the top end of memory. We
	 * will do multi-page allocations at the bottom end in the hope of
	 * reducing long-term fragmentation. But it probably won't help
	 * much if the system gets busy.
	 */

	candidate = -1;

	if (num_coremap_free > 0) {
		/* There's a free page. Find it. */

		for (i = num_coremap_entries-1; i>=0; i--) {
			if (coremap[i].cm_pinned || coremap[i].cm_allocated) {
				continue;
			}
			KASSERT(coremap[i].cm_kernel==0);
			KASSERT(coremap[i].cm_lpage==NULL);
			candidate = i;
			break;
		}
	}

	if (candidate < 0 && curthread != NULL && !curthread->t_in_interrupt) {
		KASSERT(num_coremap_free==0);
		candidate = do_page_replace();
	}

	if (candidate < 0) {
		spinlock_release(&coremap_spinlock);
		/* we don't hold global_paging_lock; don't unlock it */
		return INVALID_PADDR;
	}

	/* At this point we should have an ok page. */
	mark_pages_allocated(candidate, 1 /* npages */, dopin, iskern);
	coremap[candidate].cm_lpage = lp;

	// free pages should not be in the TLB
	KASSERT(coremap[candidate].cm_tlbix < 0);
	KASSERT(coremap[candidate].cm_cpunum == 0);

	spinlock_release(&coremap_spinlock);
	if (curthread != NULL && !curthread->t_in_interrupt) {
		lock_release(global_paging_lock);
	}

	return COREMAP_TO_PADDR(candidate);
}

static
paddr_t
coremap_alloc_multipages(unsigned npages)
{
	int base, bestbase;
	int badness, bestbadness;
	int evicted;
	unsigned i;

	KASSERT(npages>1);

	/*
	 * Get this early and hold it during the allocation so nobody else
	 * can start paging while we're trying to page out the victims in
	 * the allocation range.
	 */

	if (curthread != NULL && !curthread->t_in_interrupt) {
		lock_acquire(global_paging_lock);
	}

	spinlock_acquire(&coremap_spinlock);

	if (piggish_kernel(npages)) {
		coremap_print_short();
		spinlock_release(&coremap_spinlock);
		if (curthread != NULL && !curthread->t_in_interrupt) {
			lock_release(global_paging_lock);
		}
		kprintf("alloc_kpages: kernel heap full getting %u pages\n",
			npages);
		return INVALID_PADDR;
	}

	/*
	 * Look for the best block of this length.
	 * "badness" counts how many evictions we need to do.
	 * Find the block where it's smallest.
	 */

	do {
		bestbase = -1;
		bestbadness = npages*2;
		base = -1;
		badness = 0;
		for (i=0; i<num_coremap_entries; i++) {
			if (coremap[i].cm_pinned || coremap[i].cm_kernel) {
				base = -1;
				badness = 0;
				continue;
			}
			
			if (coremap[i].cm_allocated) {
				KASSERT(coremap[i].cm_lpage != NULL);
				/*
				 * We should do badness += 2 if page
				 * needs cleaning, but we don't know
				 * that here for now. Also, we shouldn't
				 * prefer clean pages when there isn't a
				 * pageout thread, as we'll end up always
				 * replacing code and never data, which
				 * doesn't work well. FUTURE.
				 */
				badness++;
			}
			
			if (base < 0) {
				base = i;
			}
			else if (i - base >= npages-1) {
				if (badness < bestbadness) {
					bestbase = base;
					bestbadness = badness;
				}

				/* Keep trying (offset upwards by one) */
				if (coremap[base].cm_allocated) {
					badness--;
				}
				base++;
			}
		}

		if (bestbase < 0) {
			/* no good */
			spinlock_release(&coremap_spinlock);
			if (curthread != NULL && !curthread->t_in_interrupt) {
				lock_release(global_paging_lock);
			}
			return INVALID_PADDR;
		}

		/*
		 * If any pages need evicting, evict them and try the
		 * whole schmear again. Because we are holding
		 * global_paging_lock, nobody else *ought* to allocate
		 * or pin these pages until we're done. But the
		 * contract with global_paging_lock is that it's
		 * advisory -- so tolerate and retry if/in case
		 * something changes while we're paging.
		 */

		evicted = 0;
		for (i=bestbase; i<bestbase+npages; i++) {
			if (coremap[i].cm_pinned || coremap[i].cm_kernel) {
				/* Whoops... retry */
				KASSERT(evicted==1);
				break;
			}
			if (coremap[i].cm_allocated) {
				if (curthread == NULL ||
				    curthread->t_in_interrupt) {
					/* Can't evict here */
					spinlock_release(&coremap_spinlock);
					/* don't need to unlock */
					return INVALID_PADDR;
				}
				do_evict(i);
				evicted = 1;
			}
		}
	} while (evicted);

	mark_pages_allocated(bestbase, npages, 
			     0 /* dopin -- not needed for kernel pages */,
			     1 /* kernel */);
				     
	spinlock_release(&coremap_spinlock);
	if (curthread != NULL && !curthread->t_in_interrupt) {
		lock_release(global_paging_lock);
	}
	return COREMAP_TO_PADDR(bestbase);
}

/*
 * coremap_allocuser
 *
 * Allocate a page for a user-level process, to hold the passed-in
 * logical page.
 *
 * Synchronization: takes coremap_spinlock.
 * May block to swap pages out.
 */
paddr_t
coremap_allocuser(struct lpage *lp)
{
	KASSERT(!curthread->t_in_interrupt);
	return coremap_alloc_one_page(lp, 1 /* dopin */);
}

/*
 * coremap_free 
 *
 * Deallocates the passed paddr and any subsequent pages allocated in
 * the same block. Cross-checks the iskern flag against the flags
 * maintained in the coremap entry.
 *
 * Synchronization: takes coremap_spinlock. Does not block.
 */
void
coremap_free(paddr_t page, bool iskern)
{
	uint32_t i, ppn;

	ppn = PADDR_TO_COREMAP(page);	
	
	spinlock_acquire(&coremap_spinlock);

	KASSERT(ppn<num_coremap_entries);

	for (i = ppn; i < num_coremap_entries; i++) {
		if (!coremap[i].cm_allocated) {
			panic("coremap_free: freeing free page (pa 0x%x)\n",
			      COREMAP_TO_PADDR(i));
		}

		/*
		 * Pages should be pinned when they're freed, because
		 * otherwise there's a nice big race condition. However,
		 * this doesn't apply to kernel pages.
		 */
		KASSERT(iskern || coremap[i].cm_pinned);

		/* flush any live mapping */
		if (coremap[i].cm_tlbix >= 0) {
			/* should only release one's own pages */
			KASSERT(coremap[i].cm_cpunum == curcpu->c_number);

			tlb_invalidate(coremap[i].cm_tlbix);
			coremap[i].cm_tlbix = -1;
			coremap[i].cm_cpunum = 0;

			DEBUG(DB_TLB, "... pa 0x%05lx --> tlb --\n", 
				(unsigned long) COREMAP_TO_PADDR(i));
		}

		DEBUG(DB_VM,"coremap_free: freeing pa 0x%x\n",
		      COREMAP_TO_PADDR(i));

		/* now we can actually deallocate the page */

		coremap[i].cm_allocated = 0;
		if (coremap[i].cm_kernel) {
			KASSERT(coremap[i].cm_lpage == NULL);
			num_coremap_kernel--;
			KASSERT(iskern);
			coremap[i].cm_kernel = 0;
		}
		else {
			KASSERT(coremap[i].cm_lpage != NULL);
			num_coremap_user--;
			KASSERT(!iskern);
		}
		num_coremap_free++;

		coremap[i].cm_lpage = NULL;

		if (!coremap[i].cm_notlast) {
			break;
		}

		coremap[i].cm_notlast = 0;
	}

	spinlock_release(&coremap_spinlock);
}

/*
 * alloc_kpages
 *
 * Allocate some kernel-space virtual pages.
 * This is the interface kmalloc uses to get pages for its use.
 *
 * Synchronization: takes coremap_spinlock.
 * May block to swap pages out.
 */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	if (npages > 1) {
		pa = coremap_alloc_multipages(npages);
	}
	else {
		pa = coremap_alloc_one_page(NULL, 0 /* dopin */);
	}
	if (pa==INVALID_PADDR) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

/*
 * free_kpages
 *
 * Free pages allocated with alloc_kpages.
 * Synchronization: takes coremap_spinlock. Does not block.
 */
void 
free_kpages(vaddr_t addr)
{
	coremap_free(KVADDR_TO_PADDR(addr), true /* iskern */);
}

////////////////////////////////////////////////////////////

/*
 * coremap_print_short: diagnostic dump of coremap to console.
 *
 * synchronization: assumes we hold coremap_spinlock. Does not block.
 * (printing will happen in polling mode)
 */
#define NCOLS 64
void					
coremap_print_short(void)
{
	uint32_t i, atbol=1;

	KASSERT(spinlock_do_i_hold(&coremap_spinlock));
		
	kprintf("Coremap: %u entries, %uk/%uu/%uf\n",
		num_coremap_entries,
		num_coremap_kernel, num_coremap_user, num_coremap_free);

	for (i=0; i<num_coremap_entries; i++) {
		if (atbol) {
			kprintf("0x%x: ", COREMAP_TO_PADDR(i));
			atbol=0;
		}
		if (coremap[i].cm_kernel && coremap[i].cm_notlast) {
			kprintf("k");
		}
		else if (coremap[i].cm_kernel) {
			kprintf("K");
		}
		else if (coremap[i].cm_allocated && coremap[i].cm_pinned) {
			kprintf("&");
		}
		else if (coremap[i].cm_allocated) {
			kprintf("*");
		}
		else {
			kprintf(".");
		}
		if (i%NCOLS==NCOLS-1) {
			kprintf("\n");
			atbol=1;
		}
	}
	if (!atbol) {
		kprintf("\n");
	}
}
#undef NCOLS

/*
 * coremap_pinwait: wait for a pinned page to unpin.
 */
static
void
coremap_pinwait(void)
{
	wchan_lock(coremap_pinchan);
	spinlock_release(&coremap_spinlock);
	wchan_sleep(coremap_pinchan);
	spinlock_acquire(&coremap_spinlock);
}

/*
 * coremap_pin: mark page pinned for manipulation of contents.
 *
 * Synchronization: takes coremap_spinlock. Blocks if page is already pinned.
 */
void
coremap_pin(paddr_t paddr)
{
	unsigned ix;

	ix = PADDR_TO_COREMAP(paddr);
	KASSERT(ix<num_coremap_entries);

	spinlock_acquire(&coremap_spinlock);
	while (coremap[ix].cm_pinned) {
		coremap_pinwait();
	}
	coremap[ix].cm_pinned = 1;
	spinlock_release(&coremap_spinlock);
}

/*
 * coremap_pageispinned: checks if page is marked pinned.
 *
 * Synchronization: does *not* take coremap_spinlock - we are reading
 * a single bit and that had *better* be atomic, or the processor is
 * in deep trouble.
 */
int
coremap_pageispinned(paddr_t paddr)
{
	int rv;
	unsigned ix;

	ix = PADDR_TO_COREMAP(paddr);
	KASSERT(ix<num_coremap_entries);

	/* Do this fast and loose without the spinlock. */
	rv = coremap[ix].cm_pinned != 0;

	return rv;
}

/*
 * coremap_unpin: unpin a page that was pinned with coremap_pin or
 * coremap_allocuser.
 *
 * Synchronization: takes coremap_spinlock. Does not block.
 */
void
coremap_unpin(paddr_t paddr)
{
	unsigned ix;

	ix = PADDR_TO_COREMAP(paddr);
	KASSERT(ix<num_coremap_entries);

	spinlock_acquire(&coremap_spinlock);
	KASSERT(coremap[ix].cm_pinned);
	coremap[ix].cm_pinned = 0;
	wchan_wakeall(coremap_pinchan);
	spinlock_release(&coremap_spinlock);
}

/*
 * coremap_zero_page: zero out a memory page. Page should be pinned.
 *
 * Synchronization: none. Does not block.
 */

void
coremap_zero_page(paddr_t paddr)
{
	vaddr_t va;

	KASSERT(coremap_pageispinned(paddr));

	va = PADDR_TO_KVADDR(paddr);
	bzero((char *)va, PAGE_SIZE);
}

/*
 * coremap_copy_page: copy a memory page. Both pages should be pinned.
 *
 * Synchronization: none. Does not block.
 * Note: must not take coremap_spinlock.
 */

void
coremap_copy_page(paddr_t oldpaddr, paddr_t newpaddr)
{
	vaddr_t oldva, newva;

	KASSERT(oldpaddr != newpaddr);
	KASSERT(coremap_pageispinned(oldpaddr));
	KASSERT(coremap_pageispinned(newpaddr));

	oldva = PADDR_TO_KVADDR(oldpaddr);
	newva = PADDR_TO_KVADDR(newpaddr);
	memcpy((char *)newva, (char *)oldva, PAGE_SIZE);
}

////////////////////////////////////////////////////////////

/*
 * Hardware page-table interface
 */

/*
 * mmu_setas: Set current address space in MMU.
 *
 * Synchronization: takes coremap_spinlock. Does not block.
 */
void
mmu_setas(struct addrspace *as)
{
	spinlock_acquire(&coremap_spinlock);
	if (as != curcpu->c_vm.cvm_lastas) {
		curcpu->c_vm.cvm_lastas = as;
		tlb_clear();
	}
	spinlock_release(&coremap_spinlock);
}

/*
 * mmu_unmap: Remove a translation from the MMU.
 *
 * Synchronization: takes coremap_spinlock. Does not block.
 */
void
mmu_unmap(struct addrspace *as, vaddr_t va)
{
	spinlock_acquire(&coremap_spinlock);
	if (as == curcpu->c_vm.cvm_lastas) {
		tlb_unmap(va);
	}
	spinlock_release(&coremap_spinlock);
}

/*
 * mmu_map: Enter a translation into the MMU. (This is the end result
 * of fault handling.)
 *
 * Synchronization: Takes coremap_spinlock. Does not block.
 */
void
mmu_map(struct addrspace *as, vaddr_t va, paddr_t pa, int writable)
{
	int tlbix;
	uint32_t ehi, elo;
	unsigned cmix;
	
	KASSERT(pa/PAGE_SIZE >= base_coremap_page);
	KASSERT(pa/PAGE_SIZE - base_coremap_page < num_coremap_entries);
	
	spinlock_acquire(&coremap_spinlock);

	KASSERT(as == curcpu->c_vm.cvm_lastas);

	cmix = PADDR_TO_COREMAP(pa);
	KASSERT(cmix < num_coremap_entries);

	/* Page must be pinned. */
	KASSERT(coremap[cmix].cm_pinned);

	tlbix = tlb_probe(va, 0);
	if (tlbix < 0) {
		KASSERT(coremap[cmix].cm_tlbix == -1);
		KASSERT(coremap[cmix].cm_cpunum == 0);
		tlbix = mipstlb_getslot();
		KASSERT(tlbix>=0 && tlbix<NUM_TLB);
		coremap[cmix].cm_tlbix = tlbix;
		coremap[cmix].cm_cpunum = curcpu->c_number;
		DEBUG(DB_TLB, "... pa 0x%05lx <-> tlb %d\n", 
			(unsigned long) COREMAP_TO_PADDR(cmix), tlbix);
	}
	else {
		KASSERT(tlbix>=0 && tlbix<NUM_TLB);
		KASSERT(coremap[cmix].cm_tlbix == tlbix);
		KASSERT(coremap[cmix].cm_cpunum == curcpu->c_number);
	}

	ehi = va & TLBHI_VPAGE;
	elo = (pa & TLBLO_PPAGE) | TLBLO_VALID;
	if (writable) {
		elo |= TLBLO_DIRTY;
	}

	tlb_write(ehi, elo, tlbix);

	/* Unpin the page. */
	coremap[cmix].cm_pinned = 0;
	wchan_wakeall(coremap_pinchan);

	spinlock_release(&coremap_spinlock);
}
