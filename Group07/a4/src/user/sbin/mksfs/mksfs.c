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

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <err.h>

#include "support.h"
#include "kern/sfs.h"


#ifdef HOST

#include <netinet/in.h> // for arpa/inet.h
#include <arpa/inet.h>  // for ntohl
#include "hostcompat.h"
#define SWAPL(x) ntohl(x)
#define SWAPS(x) ntohs(x)

#else

#define SWAPL(x) (x)
#define SWAPS(x) (x)

#endif

#include "disk.h"

#define MAXBITBLOCKS 32

static
void
check(void)
{
	assert(sizeof(struct sfs_super)==SFS_BLOCKSIZE);
	assert(sizeof(struct sfs_inode)==SFS_BLOCKSIZE);
	assert(SFS_BLOCKSIZE % sizeof(struct sfs_dir) == 0);
}

static
void
writesuper(const char *volname, uint32_t nblocks)
{
	struct sfs_super sp;

	bzero((void *)&sp, sizeof(sp));

	if (strlen(volname) >= SFS_VOLNAME_SIZE) {
		errx(1, "Volume name %s too long", volname);
	}

	sp.sp_magic = SWAPL(SFS_MAGIC);
	sp.sp_nblocks = SWAPL(nblocks);
	strcpy(sp.sp_volname, volname);

	diskwrite(&sp, SFS_SB_LOCATION);
}

static
void
writerootdir(uint32_t rootdata)
{
	struct sfs_inode sfi;
	char block[SFS_BLOCKSIZE];
	struct sfs_dir *entry;
	size_t size;

	/* Create "." and ".." entries for root directory */
	entry = (struct sfs_dir *)block;
	entry->sfd_ino = SWAPL(SFS_ROOT_LOCATION);
	strcpy(entry->sfd_name, ".");
	entry = (struct sfs_dir *)(block + sizeof(struct sfs_dir));
	entry->sfd_ino = SWAPL(SFS_ROOT_LOCATION);
	strcpy(entry->sfd_name, "..");
	diskwrite(&block, rootdata);

	/* Initialize inode for root directory */
	bzero((void *)&sfi, sizeof(sfi));
	size = 2*sizeof(struct sfs_dir); /* for . and .. entries */
	sfi.sfi_size = SWAPL(size);
	sfi.sfi_type = SWAPS(SFS_TYPE_DIR);
	/* linkcount: 1 from "." and 1 from ".."  == 2 */
	sfi.sfi_linkcount = SWAPS(2); 
	sfi.sfi_direct[0] = SWAPL(rootdata);

	/* Now write root inode */
	diskwrite(&sfi, SFS_ROOT_LOCATION);
}

static char bitbuf[MAXBITBLOCKS*SFS_BLOCKSIZE];

static
void
doallocbit(uint32_t bit)
{
	uint32_t byte = bit/CHAR_BIT;
	unsigned char mask = (1<<(bit % CHAR_BIT));

	assert((bitbuf[byte] & mask) == 0);
	bitbuf[byte] |= mask;
}

/* Returns the block number of the root directory's data block */
static
uint32_t
writebitmap(uint32_t fsblocks)
{

	uint32_t nbits = SFS_BITMAPSIZE(fsblocks);
	uint32_t nblocks = SFS_BITBLOCKS(fsblocks);
	char *ptr;
	uint32_t i;
	uint32_t rootdata = SFS_MAP_LOCATION+nblocks;

	if (nblocks > MAXBITBLOCKS) {
		errx(1, "Filesystem too large "
		     "- increase MAXBITBLOCKS and recompile");
	}

	doallocbit(SFS_SB_LOCATION);
	doallocbit(SFS_ROOT_LOCATION);

	/* Mark bits for bitmap itself as allocated */
	for (i=0; i<nblocks; i++) {
		doallocbit(SFS_MAP_LOCATION+i);
	}

	/* Mark bits above actual size of filesystem as allocated */
	for (i=fsblocks; i<nbits; i++) {
		doallocbit(i);
	}

	/* Allocate one more block for the root directory entries */
	doallocbit(rootdata);

	for (i=0; i<nblocks; i++) {
		ptr = bitbuf + i*SFS_BLOCKSIZE;
		diskwrite(ptr, SFS_MAP_LOCATION+i);
	}

	return rootdata;
}

int
main(int argc, char **argv)
{
	uint32_t size, blocksize, rootdata;
	char *volname, *s;

#ifdef HOST
	hostcompat_init(argc, argv);
#endif

	if (argc!=3) {
		errx(1, "Usage: mksfs device/diskfile volume-name");
	}

	check();

	volname = argv[2];

	/* Remove one trailing colon from volname, if present */
	s = strchr(volname, ':');
	if (s != NULL) {
		if (strlen(s)!=1) {
			errx(1, "Illegal volume name %s", volname);
		}
		*s = 0;
	}

	/* Don't allow slashes */
	s = strchr(volname, '/');
	if (s != NULL) {
		errx(1, "Illegal volume name %s", volname);
	}

	opendisk(argv[1]);
	blocksize = diskblocksize();

	if (blocksize!=SFS_BLOCKSIZE) {
		errx(1, "Device has wrong blocksize %u (should be %u)\n",
		     blocksize, SFS_BLOCKSIZE);
	}
	size = diskblocks();

	writesuper(volname, size);
	rootdata = writebitmap(size);
	writerootdir(rootdata);

	closedisk();

	return 0;
}
