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
#include <array.h>
#include <addrspace.h>
#include <vm.h>
#include <vmprivate.h>
#include <machine/coremap.h>

/*
 * vm_object operations.
 */


DEFARRAY_BYTYPE(lpage_array, struct lpage, /*noinline*/);

/*
 * vm_object_create: Allocate a new vm_object with nothing in it.
 * Returns: new vm_object on success, NULL on error.
 */
struct vm_object *
vm_object_create(size_t npages)
{
	struct vm_object *vmo;
	unsigned i;
	int result;

	result = swap_reserve(npages);
	if (result != 0) {
		return NULL;
	}

	vmo = kmalloc(sizeof(struct vm_object));
	if (vmo == NULL) {
		swap_unreserve(npages);
		return NULL;
	}

	vmo->vmo_lpages = lpage_array_create();
	if (vmo->vmo_lpages == NULL) {
		kfree(vmo);
		swap_unreserve(npages);
		return NULL;
	}

	vmo->vmo_base = 0xdeafbeef;		/* make sure these */
	vmo->vmo_lower_redzone = 0xdeafbeef;	/* get filled in later */

	/* add the requested number of zerofilled pages */
	result = lpage_array_setsize(vmo->vmo_lpages, npages);
	if (result) {
		lpage_array_destroy(vmo->vmo_lpages);
		kfree(vmo);
		swap_unreserve(npages);
		return NULL;
	}

	for (i=0; i<npages; i++) {
		lpage_array_set(vmo->vmo_lpages, i, NULL);
	}

	return vmo;
}

/*
 * vm_object_copy: clone a vm_object.
 *
 * Synchronization: None; lpage_copy does the hard stuff.
 */
int
vm_object_copy(struct vm_object *vmo, struct addrspace *newas,
	       struct vm_object **ret)
{
	struct vm_object *newvmo;

	struct lpage *newlp, *lp;
	unsigned j;
	int result;

	newvmo = vm_object_create(lpage_array_num(vmo->vmo_lpages));
	if (newvmo == NULL) {
		return ENOMEM;
	}

	newvmo->vmo_base = vmo->vmo_base;
	newvmo->vmo_lower_redzone = vmo->vmo_lower_redzone;

	for (j = 0; j < lpage_array_num(vmo->vmo_lpages); j++) {
		lp = lpage_array_get(vmo->vmo_lpages, j);
		newlp = lpage_array_get(newvmo->vmo_lpages, j);

		/* new guy should be initialized to all zerofill */
		KASSERT(newlp == NULL);

		if (lp == NULL) {
			/* old guy is zerofill too, don't do anything */
			continue;
		}

		result = lpage_copy(lp, &newlp);
		if (result) {
			goto fail;
		}
		lpage_array_set(newvmo->vmo_lpages, j, newlp);
	}

	*ret = newvmo;
	return 0;

 fail:
	vm_object_destroy(newas, newvmo);
	return result;
}

/*
 * vm_object_setsize: change the size of a vm_object.
 */
int
vm_object_setsize(struct addrspace *as, struct vm_object *vmo, unsigned npages)
{
	int result;
	unsigned i;
	struct lpage *lp;

	KASSERT(vmo != NULL);
	KASSERT(vmo->vmo_lpages != NULL);

	if (npages < lpage_array_num(vmo->vmo_lpages)) {
		for (i=npages; i<lpage_array_num(vmo->vmo_lpages); i++) {
			lp = lpage_array_get(vmo->vmo_lpages, i);
			if (lp != NULL) {
				KASSERT(as != NULL);
				/* remove any tlb entry for this mapping */
				mmu_unmap(as, vmo->vmo_base+PAGE_SIZE*i);
				lpage_destroy(lp);
			}
			else {
				swap_unreserve(1);
			}
		}
		result = lpage_array_setsize(vmo->vmo_lpages, npages);
		/* shrinking an array shouldn't fail */
		KASSERT(result==0);
	}
	else if (npages > lpage_array_num(vmo->vmo_lpages)) {
		int oldsize = lpage_array_num(vmo->vmo_lpages);
		unsigned newpages = npages - oldsize;

		result = swap_reserve(newpages);
		if (result) {
			return result;
		}

		result = lpage_array_setsize(vmo->vmo_lpages, npages);
		if (result) {
			swap_unreserve(newpages);
			return result;
		}
		for (i=oldsize; i<npages; i++) {
			lpage_array_set(vmo->vmo_lpages, i, NULL);
		}
	}
	return 0;
}

/*
 * vm_object_destroy: Deallocates a vm_object.
 *
 * Synchronization: none; assumes one thread uniquely owns the object.
 */
void 					
vm_object_destroy(struct addrspace *as, struct vm_object *vmo)
{
	int result;

	result = vm_object_setsize(as, vmo, 0);
	KASSERT(result==0);
	
	lpage_array_destroy(vmo->vmo_lpages);
	kfree(vmo);
}

