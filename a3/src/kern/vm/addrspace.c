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
#include <kern/unistd.h>
#include <limits.h>
#include <lib.h>
#include <array.h>
#include <uio.h>
#include <thread.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vmprivate.h>
#include <machine/coremap.h>   /* for mmu_setas() */
#include <vnode.h>
#include <vfs.h>
#include <syscall.h>


/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 */



DEFARRAY_BYTYPE(vm_object_array, struct vm_object, /*noinline*/);

/*
 * as_create - create an address space structure.
 * Synchronization: none.
 */
struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->as_objects = vm_object_array_create();
	if (as->as_objects == NULL) {
		kfree(as);
		return NULL;
	}

	return as;
}

/*
 * as_copy: duplicate an address space. Creates a new address space and
 * copies each vm_object in the source address space into the new one.
 * Implements the VM system part of fork().
 *
 * Synchronization: none.
 */
int
as_copy(struct addrspace *as, struct addrspace **ret)
{
	struct addrspace *newas;
	struct vm_object *vmo, *newvmo;
	unsigned i;
	int result;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	/*
	 * We assume that as belongs to curthread, and furthermore that
	 * it's not shared with any other threads. (The latter restriction
	 * is easily lifted; the former is not.)
	 *
	 * We assume that nothing is going to modify the source address
	 * space except for the usual page evictions by other processes.
	 */

	KASSERT(as == curthread->t_addrspace);


	/* copy the vmos */
	for (i = 0; i < vm_object_array_num(as->as_objects); i++) {
		vmo = vm_object_array_get(as->as_objects, i);

		result = vm_object_copy(vmo, newas, &newvmo);
		if (result) {
			goto fail;
		}

		result = vm_object_array_add(newas->as_objects, newvmo, NULL);
		if (result) {
			vm_object_destroy(newas, newvmo);
			goto fail;
		}
	}
	
	*ret = newas;
	return 0;

fail:
	as_destroy(newas);
	return result;
}

/*
 * as_fault: fault handling. Handle a fault on an address space, of
 * specified type, at specified address.
 *
 * Synchronization: none. We assume the address space is not shared,
 * so we don't lock it.
 */
int
as_fault(struct addrspace *as, int faulttype, vaddr_t va)
{
	struct vm_object *faultobj = NULL;
	struct lpage *lp;
	vaddr_t bot=0, top;
	unsigned i, index;
	int result;

	/* Find the vm_object concerned */
	for (i=0; i<vm_object_array_num(as->as_objects); i++) {
		struct vm_object *vmo;

		vmo = vm_object_array_get(as->as_objects, i);
		bot = vmo->vmo_base;
		top = bot + PAGE_SIZE * lpage_array_num(vmo->vmo_lpages);
		if (va >= bot && va < top) {
			faultobj = vmo;
			break;
		}
	}

	if (faultobj == NULL) {
		DEBUG(DB_VM, "vm_fault: EFAULT: va=0x%x\n", va);
		return EFAULT;
	}

	/* Now get the logical page */
	index = (va - bot) / PAGE_SIZE;
	lp = lpage_array_get(faultobj->vmo_lpages, index);

	if (lp == NULL) {
		/* zerofill page */
		result = lpage_zerofill(&lp);
		if (result) {
			kprintf("vm: zerofill fault at 0x%x failed\n", va);
			return result;
		}
		lpage_array_set(faultobj->vmo_lpages, index, lp);
	}
	
	return lpage_fault(lp, as, faulttype, va);
}

/*
 * as_destroy: wipe out an address space by destroying its components.
 * Synchronization: none.
 */
void
as_destroy(struct addrspace *as)
{
	struct vm_object *vmo;
	unsigned i;

	for (i = 0; i < vm_object_array_num(as->as_objects); i++) {
		vmo = vm_object_array_get(as->as_objects, i);
		vm_object_destroy(as, vmo);
	}

	vm_object_array_setsize(as->as_objects, 0);
	vm_object_array_destroy(as->as_objects);
	kfree(as);
}

/*
 * as_activate: load specified address space into the MMU as the
 * current address space. Called from context switch and also during
 * execv().
 *
 * Synchronization: none.
 */
void
as_activate(struct addrspace *as)
{
	KASSERT(as==NULL || as==curthread->t_addrspace);
	mmu_setas(as);
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored.
 *
 * Does not allow overlapping regions.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 size_t lower_redzone,
		 int readable, int writeable, int executable)
{
	struct vm_object *vmo;
	unsigned i;
	int result;
	vaddr_t check_vaddr;	/* vaddr to use for overlap check */

	(void)readable;
	(void)writeable;	// XXX
	(void)executable;

	/* align base address */
	vaddr &= PAGE_FRAME;

	/* redzone must be aligned */
	KASSERT((lower_redzone & PAGE_FRAME) == lower_redzone);

	/* redzone must fit */
	KASSERT(vaddr >= lower_redzone);
	check_vaddr = vaddr - lower_redzone;

	/* size may not be */
	sz = ROUNDUP(sz, PAGE_SIZE);

	/*
	 * Check for overlaps.
	 */
	for (i = 0; i < vm_object_array_num(as->as_objects); i++) {
		vaddr_t bot, top;
		
		vmo = vm_object_array_get(as->as_objects, i);
		KASSERT(vmo != NULL);
		bot = vmo->vmo_base;
		top = bot + PAGE_SIZE * lpage_array_num(vmo->vmo_lpages);

		/* Check guard band, if any */
		KASSERT(bot >= vmo->vmo_lower_redzone);
		bot = bot - vmo->vmo_lower_redzone;

		if (check_vaddr+sz > bot && check_vaddr < top) {
			/* overlap */
			return EINVAL;
		}
	}


	/* Create a new vmo. All pages are marked zerofilled. */
	vmo = vm_object_create(sz/PAGE_SIZE);
	if (vmo == NULL) {
		return ENOMEM;
	}
	vmo->vmo_base = vaddr;
	vmo->vmo_lower_redzone = lower_redzone;

	/* Add it to the parent address space. */
	result = vm_object_array_add(as->as_objects, vmo, NULL);
	if (result) {
		vm_object_destroy(as, vmo);
		return result;
	}

	/* Done */
	return 0;
}

/*
 * as_prepare_load: called before loading executable segments.
 */
int
as_prepare_load(struct addrspace *as)
{
	/*
	 * Do nothing.
	 */

	(void)as;
	return 0;
}

/*
 * as_complete_load: called after loading executable segments.
 */
int
as_complete_load(struct addrspace *as)
{
	/*
	 * Do nothing.
	 */

	(void)as;
	return 0;
}

/*
 * as_define_stack - define the vm_object for the user-level stack.
 */
int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	int err;

	/*
	 * make a stack vm_object 
	 *
	 * The stack is USERSTACKSIZE bytes, which is defined in machine/vm.h.
	 * This is generally quite large, so it is zerofilled to make swap use
	 * efficient and fork reasonably fast.
	 */

	err = as_define_region(as, USERSTACKBASE, USERSTACKSIZE, 
			       USERSTACKREDZONE,
			       1, 1, 0);
	if (err) {
		return err;
	}

	/* Initial user-level stack pointer */
	*stackptr = USERSTACK;
	
	return 0;
}
