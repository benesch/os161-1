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
#include <kern/fcntl.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <bitmap.h>
#include <synch.h>
#include <thread.h>
#include <current.h>
#include <mainbus.h>
#include <addrspace.h>
#include <vm.h>
#include <vmprivate.h>
#include <machine/coremap.h>
#include <vfs.h>
#include <vnode.h>

/*
 * swap.c - swapfile management and operations.
 */

static const char swapfilename[] = "lhd0raw:";

/*
 * Data.
 */
static struct bitmap *swapmap;	// swap allocation map
static struct lock *swaplock;	// synchronizes swapmap and counters

/*
 * A "reserved" page is one for which no swap page has actually
 * been allocated but for which we are committed to being able to
 * provide swap. This happens when zero-fill pages are allocated.
 */
static unsigned long swap_total_pages;
static unsigned long swap_free_pages;
static unsigned long swap_reserved_pages;

static struct vnode *swapstore;	// swap file

/*
 * Only one page can be in transit to/from disk at once (at least under
 * present circumstances.) While the disk device will queue up multiple
 * concurrent requests correctly, there's no benefit to doing so. So we
 * enforce mutual exclusion at a higher level. This reduces the number
 * of pages marked in transit at any one time and thus (hopefully) makes
 * paging more intelligent and multipage allocation less likely to starve.
 *
 * This lock signals "intent to page" and should be construed as
 * advisory.
 */

struct lock *global_paging_lock;


/*
 * swap_bootstrap: Initializes swap information and finishes
 * bootstrapping the VM so that processes can use it.
 *
 * Synchronization: none (runs during boot before anyone else uses VM)
 */
void
swap_bootstrap(void)
{
	int rv;
	struct stat st;
	char path[sizeof(swapfilename)];
	off_t minsize;
	size_t pmemsize;

	pmemsize = mainbus_ramsize();

	strcpy(path, swapfilename);
	rv = vfs_open(path, O_RDWR, 0, &swapstore);
	if (rv) {
		kprintf("swap: Error %d opening swapfile %s\n", rv, 
			swapfilename);
		kprintf("swap: Please create swapfile/swapdisk.\n");
		panic("swap: Unable to continue.\n");
	}

	minsize = pmemsize*20;

	VOP_STAT(swapstore, &st);
	if (st.st_size < minsize) {
		kprintf("swap: swapfile %s is only %lu bytes.\n", swapfilename,
			(unsigned long) st.st_size);
		kprintf("swap: with %lu bytes of physical memory it should "
			"be at least\n", (unsigned long) pmemsize);
		kprintf("      %lu bytes (%lu blocks), perhaps larger.\n", 
			(unsigned long) minsize, 
			(unsigned long) minsize / 512);
		kprintf("swap: Because we conservatively reserve swap, a "
			"large amount may be\n");
		kprintf("      needed to run large workloads.\n");
		kprintf("swap: Please extend it.\n");
		panic("swap: Unable to continue.\n");
	}

	kprintf("swap: swapping to %s (%lu bytes; %lu pages)\n", swapfilename,
		(unsigned long) st.st_size, 
		(unsigned long) st.st_size / PAGE_SIZE);

	swap_total_pages = st.st_size / PAGE_SIZE;
	swap_free_pages = swap_total_pages;
	swap_reserved_pages = 0;

	swapmap = bitmap_create(st.st_size/PAGE_SIZE);
	DEBUG(DB_VM, "creating swap map with %lld entries\n",
			st.st_size/PAGE_SIZE);
	if (swapmap == NULL) {
		panic("swap: No memory for swap bitmap\n");
	}

	swaplock = lock_create("swaplock");
	if (swaplock == NULL) {
		panic("swap: No memory for swap lock\n");
	}

	/* mark the first page of swap used so we can check for errors */
	bitmap_mark(swapmap, 0);
	swap_free_pages--;
}

/*
 * swap_shutdown
 *
 * Destroys data structures and closes the swap vnode.
 */
void
swap_shutdown(void)
{
	lock_destroy(swaplock);
	bitmap_destroy(swapmap);
	vfs_close(swapstore);
}

/*
 * swap_alloc: allocates a page in the swapfile.
 * The page should have already been reserved with swap_reserve.
 *
 * Synchronization: uses swaplock.
 */
off_t
swap_alloc(void)
{
	uint32_t rv, index;
	
	lock_acquire(swaplock);

	KASSERT(swap_free_pages <= swap_total_pages);
	KASSERT(swap_reserved_pages <= swap_free_pages);

	KASSERT(swap_reserved_pages>0);
	KASSERT(swap_free_pages>0);

	rv = bitmap_alloc(swapmap, &index);
	/* If this blows up, our counters are wrong */
	KASSERT(rv == 0);

	swap_reserved_pages--;
	swap_free_pages--;

	lock_release(swaplock);

	return index*PAGE_SIZE;
}

/*
 * swap_free: marks a page in the swapfile as unused.
 *
 * Synchronization: uses swaplock.
 */
void
swap_free(off_t swapaddr)
{
	uint32_t index;

	KASSERT(swapaddr != INVALID_SWAPADDR);
	KASSERT(swapaddr % PAGE_SIZE == 0);

	index = swapaddr / PAGE_SIZE;

	lock_acquire(swaplock);

	KASSERT(swap_free_pages < swap_total_pages);
	KASSERT(swap_reserved_pages <= swap_free_pages);

	KASSERT(bitmap_isset(swapmap, index));
	bitmap_unmark(swapmap, index);
	swap_free_pages++;

	lock_release(swaplock);
}

/*
 * swap_reserve/unreserve: reserve some pages for future allocation, or
 * release such pages.
 *
 * Synchronization: uses swaplock.
 */
int
swap_reserve(unsigned long npages)
{
	lock_acquire(swaplock);

	KASSERT(swap_free_pages <= swap_total_pages);
	KASSERT(swap_reserved_pages <= swap_free_pages);

	if (swap_free_pages - swap_reserved_pages < npages) {
		lock_release(swaplock);
		return ENOMEM;
	}

	swap_reserved_pages += npages;

	KASSERT(swap_free_pages <= swap_total_pages);
	KASSERT(swap_reserved_pages <= swap_free_pages);

	lock_release(swaplock);
	return 0;
}

void
swap_unreserve(unsigned long npages)
{
	lock_acquire(swaplock);

	KASSERT(swap_free_pages <= swap_total_pages);
	KASSERT(swap_reserved_pages <= swap_free_pages);

	KASSERT(npages <= swap_reserved_pages);
	swap_reserved_pages -= npages;

	lock_release(swaplock);
}

/*
 * swap_io: Does one swap I/O. Panics on failure.
 *
 * Synchronization: none specifically. The physical page should be
 * marked "pinned" (locked) so it won't be touched by other people.
 */
static
void
swap_io(paddr_t pa, off_t swapaddr, enum uio_rw rw)
{
	struct iovec iov;
	struct uio u;
	vaddr_t va;
	int result;

	KASSERT(lock_do_i_hold(global_paging_lock));

	KASSERT(pa != INVALID_PADDR);
	KASSERT(swapaddr % PAGE_SIZE == 0);
	KASSERT(coremap_pageispinned(pa));
	KASSERT(bitmap_isset(swapmap, swapaddr / PAGE_SIZE));

	va = coremap_map_swap_page(pa);

	uio_kinit(&iov, &u, (char *)va, PAGE_SIZE, swapaddr, rw);
	if (rw==UIO_READ) {
		result = VOP_READ(swapstore, &u);
	}
	else {
		result = VOP_WRITE(swapstore, &u);
	}

	coremap_unmap_swap_page(va, pa);

	if (result==EIO) {
		panic("swap: EIO on swapfile (offset %ld)\n",
		      (long)swapaddr);
	}
	else if (result==EINVAL) {
		panic("swap: EINVAL from swapfile (offset %ld)\n",
		      (long)swapaddr);
	}
	else if (result) {
		panic("swap: Error %d from swapfile (offset %ld)\n",
		      result, (long)swapaddr);
	}
}

/*
 * swap_pagein: load one page from swap into physical memory.
 * Synchronization: none here. See swap_io().
 */
void
swap_pagein(paddr_t pa, off_t swapaddr)
{
	swap_io(pa, swapaddr, UIO_READ);
}


/* 
 * swap_pageout: write one page from physical memory into swap.
 * Synchronization: none here. See swap_io().
 */
void
swap_pageout(paddr_t pa, off_t swapaddr)
{
	swap_io(pa, swapaddr, UIO_WRITE);
}
