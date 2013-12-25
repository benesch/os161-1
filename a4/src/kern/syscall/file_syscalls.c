/* BEGIN A4 SETUP */
/* This file existed previously, but has been completely replaced for A4.
 * We have kept the dumb versions of sys_read and sys_write to support early
 * testing, but they should be replaced with proper implementations that 
 * use your open file table to find the correct vnode given a file descriptor
 * number.  All the "dumb console I/O" code should be deleted.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <syscall.h>
#include <vfs.h>
#include <vnode.h>
#include <uio.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <copyinout.h>
#include <synch.h>
#include <file.h>
#include <kern/seek.h> /* For lseek */
#include <spinlock.h>
/*
 * mk_useruio
 * sets up the uio for a USERSPACE transfer. 
 */
static
void
mk_useruio(struct iovec *iov, struct uio *u, userptr_t buf, 
	   size_t len, off_t offset, enum uio_rw rw)
{

	iov->iov_ubase = buf;
	iov->iov_len = len;
	u->uio_iov = iov;
	u->uio_iovcnt = 1;
	u->uio_offset = offset;
	u->uio_resid = len;
	u->uio_segflg = UIO_USERSPACE;
	u->uio_rw = rw;
	u->uio_space = curthread->t_addrspace;
}

/*
 * sys_open
 * just copies in the filename, then passes work to file_open.
 * You have to write file_open.
 * 
 */
int
sys_open(userptr_t filename, int flags, int mode, int *retval)
{
	char *fname;
	int result;

	if ( (fname = (char *)kmalloc(__PATH_MAX)) == NULL) {
		return ENOMEM;
	}

	result = copyinstr(filename, fname, __PATH_MAX, NULL);
	if (result) {
		kfree(fname);
		return result;
	}

	result = file_open(fname, flags, mode, retval);
	kfree(fname);
	return result;
}

/* 
 * sys_close
 * You have to write file_close.
 */
int
sys_close(int fd)
{
	return file_close(fd);
}

/* 
 * sys_dup2
 * 
 */
int
sys_dup2(int oldfd, int newfd, int *retval)
{
    DEBUG(DB_VFS, "dup2: newfd=%d, oldfd=%d\n", newfd, oldfd);
    
    /* Check if newfd is a valid file descriptor. */
    if ((newfd < 0) || (newfd >= __OPEN_MAX)) {
        return EBADF;
    } 
    
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* Check if oldfd is a valid file handle. */
    if ((oldfd < 0) || (oldfd >= __OPEN_MAX) || (ft->ft_entries[oldfd] == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    
    /* If newfd and oldfd are the same, do nothing and return. */
    if (newfd == oldfd) {
        *retval = newfd;
        spinlock_release(&ft->ft_spinlock);
        return 0;
    }
    
    /* If newfd is pointing to an open file, close that file. */
    if (ft->ft_entries[newfd] != NULL) {
        file_close(newfd);
    }
    
    ft->ft_entries[newfd] = ft->ft_entries[oldfd];
    ft->ft_entries[newfd]->ft_count++;
    *retval = newfd;

    spinlock_release(&ft->ft_spinlock);
	return 0;
}

/*
 * sys_read
 * calls VOP_READ.
 * 
 * A4: This is the "dumb" implementation of sys_write:
 * it only deals with file descriptors 1 and 2, and 
 * assumes they are permanently associated with the 
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */
int
sys_read(int fd, userptr_t buf, size_t size, int *retval)
{
	DEBUG(DB_VFS, "*** Reading fd %d\n", fd);
    
    struct uio user_uio;
	struct iovec user_iov;
	int result;
	int offset = 0;

    /* Check if fd is a valid file descriptor. */
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* If fd is not a valid file descriptor, or was not opened for reading,
     * return error */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    int how = ft->ft_entries[fd]->ft_flags & O_ACCMODE;
    if ((how != O_RDONLY) && (how != O_RDWR)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }

	/* set up a uio with the buffer, its size, and the current offset */
    offset = ft->ft_entries[fd]->ft_pos;
	mk_useruio(&user_iov, &user_uio, buf, size, offset, UIO_READ);

	/* does the read */
    spinlock_release(&ft->ft_spinlock);
	result = VOP_READ(ft->ft_entries[fd]->ft_vnode, &user_uio);
	if (result) {
		return result;
	}

	/*
	 * The amount read is the size of the buffer originally, minus
	 * how much is left in it.
	 */
	*retval = size - user_uio.uio_resid;
    
    /* Advance file seek position. */
    spinlock_acquire(&ft->ft_spinlock);
    ft->ft_entries[fd]->ft_pos += *retval;

    spinlock_release(&ft->ft_spinlock);
	return 0;
}

/*
 * sys_write
 * calls VOP_WRITE.
 *
 * A4: This is the "dumb" implementation of sys_write:
 * it only deals with file descriptors 1 and 2, and 
 * assumes they are permanently associated with the 
 * console vnode (which must have been previously initialized).
 *
 * In your implementation, you should use the file descriptor
 * to find a vnode from your file table, and then read from it.
 *
 * Note that any problems with the address supplied by the
 * user as "buf" will be handled by the VOP_READ / uio code
 * so you do not have to try to verify "buf" yourself.
 *
 * Most of this code should be replaced.
 */

int
sys_write(int fd, userptr_t buf, size_t len, int *retval) 
{
	/*DEBUG(DB_VFS, "Writing fd %d\n", fd);*/
    
    struct uio user_uio;
    struct iovec user_iov;
    int result;
    int offset = 0;

    /* Check if fd is a valid file descriptor. */
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* If fd is not a valid file descriptor, or was not opened for writing,
     * return error */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    int how = ft->ft_entries[fd]->ft_flags & O_ACCMODE;
    if ((how != O_WRONLY) && (how != O_RDWR)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    
    /* set up a uio with the buffer, its size, and the current offset */
    offset = ft->ft_entries[fd]->ft_pos;
    mk_useruio(&user_iov, &user_uio, buf, len, offset, UIO_WRITE);

    /* does the write */
    result = VOP_WRITE(ft->ft_entries[fd]->ft_vnode, &user_uio);
    if (result) {
        spinlock_release(&ft->ft_spinlock);
        return result;
    }

    /*
     * the amount written is the size of the buffer originally,
     * minus how much is left in it.
     */
    *retval = len - user_uio.uio_resid;
    
    /* Advance file seek position. */
    ft->ft_entries[fd]->ft_pos += *retval;

    spinlock_release(&ft->ft_spinlock);
    return 0;
}

/*
 * sys_lseek
 * 
 */
int
sys_lseek(int fd, off_t offset, int whence, off_t *retval)
{
    DEBUG(DB_VFS, "Lseeking fd %d with offset %d\n", fd, (int)offset);
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* If fd is not a valid file descriptor, return error. */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    
    int pos;
    if (whence == SEEK_SET) {
        /* Update new position to offset.*/
        pos = (int)offset;
    }
    else if (whence == SEEK_CUR) {
        /* Update new position to current position + pos. */
        pos = (ft->ft_entries[fd]->ft_pos + (int)offset);
    } else if (whence == SEEK_END) {
        /* Update new positionto end-of-file + pos. */
        struct stat ft_stat;
        VOP_STAT(ft->ft_entries[fd]->ft_vnode, &ft_stat);
        pos = ft_stat.st_size + offset;
    } else {
        /* whence value is invalid. return error. */
        spinlock_release(&ft->ft_spinlock);
        return EINVAL;
    }

    /* If resulting position is negative, return error. */
    if (pos < 0) {
        spinlock_release(&ft->ft_spinlock);
        return EINVAL;
    }
    
    int result = VOP_TRYSEEK(ft->ft_entries[fd]->ft_vnode, pos);
    if (result != 0) {
        DEBUG(DB_VFS, "   tryseek failed with %d\n", result);
        spinlock_release(&ft->ft_spinlock);
        return ESPIPE;
    }
    
    ft->ft_entries[fd]->ft_pos = pos;
    *retval = (off_t)pos;
    spinlock_release(&ft->ft_spinlock);
	return 0;
}


/* really not "file" calls, per se, but might as well put it here */

/* sys_mkdir
 * Copies the given path into the kernel space, and call vfs_mkdir.
 */
int
sys_mkdir(userptr_t path, int mode) {
	char *p;
	int result;

	if ((p = (char *)kmalloc(__PATH_MAX)) == NULL) {
		return ENOMEM;
	}

	/* Copy in the path */
	result = copyinstr(path, p, __PATH_MAX, NULL);
	if (result) {
		kfree(p);
		return result;
	}

	/* Check that given path of directory is valid */
	if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
		kfree(p);
		return EEXIST;
	}

	result = vfs_mkdir(p, mode);
	kfree(p);
	return result;
}

/* sys_rmdir
 * Copies the given path into the kernel space, and call vfs_rmdir.
 */
int
sys_rmdir(userptr_t path) {
	char *p;
	int result;

	if ((p = (char *)kmalloc(__PATH_MAX)) == NULL) {
		return ENOMEM;
	}

	/* Copy in the path */
	result = copyinstr(path, p, __PATH_MAX, NULL);
	if (result) {
		kfree(p);
		return result;
	}

	/* Check that given path of directory is valid */
	if (strcmp(p, ".") == 0 || strcmp(p, "..") == 0) {
		kfree(p);
		return EINVAL;
	}

	result = vfs_rmdir(p);
	kfree(p);
	return result;
}

/*
 * sys_chdir
 * Copies the given path into the kernel space, and call vfs_chdir.
 */
int
sys_chdir(userptr_t path)
{
	char *p;
	int result;

	if ((p = (char *)kmalloc(__PATH_MAX)) == NULL) {
		return ENOMEM;
	}

	/* Copy in the path */
	result = copyinstr(path, p, __PATH_MAX, NULL);
	if (result) {
		kfree(p);
		return result;
	}

	result = vfs_chdir(p);
	kfree(p);
	return result;
}

/*
 * sys___getcwd
 * Sets up the uio and call vfs_cwd.
 */
int
sys___getcwd(userptr_t buf, size_t buflen, int *retval)
{
    struct uio user_uio;
    struct iovec user_iov;
    int result;

    /* set up a uio with the buffer, its size, and the current offset */
    mk_useruio(&user_iov, &user_uio, buf, buflen, 0, UIO_READ);

    result = vfs_getcwd(&user_uio);
    *retval = result;

    return result;
}

/*
 * sys_fstat
 */
int
sys_fstat(int fd, userptr_t statptr)
{
    DEBUG(DB_VFS, "fstat %d\n", fd);
	struct stat kbuf;
	int err;

    /* Check if fd is a valid file descriptor. */
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* If fd is not a valid file descriptor, return error */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }

	/* Call VOP_STAT on the vnode */
	err = VOP_STAT(ft->ft_entries[fd]->ft_vnode, &kbuf);
    spinlock_release(&ft->ft_spinlock);
	if (err) {
		return err;
	}

	return copyout(&kbuf, statptr, sizeof(struct stat));
}

/*
 * sys_getdirentry
 */
int
sys_getdirentry(int fd, userptr_t buf, size_t buflen, int *retval)
{
    DEBUG(DB_VFS, "*** getdirentry %d\n", fd);
	struct vnode *vn;
	off_t offset = 0;
	int err;
	struct uio my_uio;
	struct iovec uio_iov;

	/* Check if fd is a valid file descriptor. */
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* If fd is not a valid file descriptor, or was not opened for reading,
     * return error */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    int how = ft->ft_entries[fd]->ft_flags & O_ACCMODE;
    if ((how != O_RDONLY) && (how != O_RDWR)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }

	/* Initialize vn and offset using your filetable info for fd */
    vn = ft->ft_entries[fd]->ft_vnode;
    offset = ft->ft_entries[fd]->ft_pos;

	/* set up a uio with the buffer, its size, and the current offset */
	mk_useruio(&uio_iov, &my_uio, buf, buflen, offset, UIO_READ);

	/* does the read */
	err = VOP_GETDIRENTRY(vn, &my_uio);
	if (err) {
        spinlock_release(&ft->ft_spinlock);
		return err;
	}

	/* Set the offset to the updated offset in the uio. 
	 * Save the new offset with your filetable info for fd.
	 */
	offset = my_uio.uio_offset;
    ft->ft_entries[fd]->ft_pos = offset;
	
	/*
	 * the amount read is the size of the buffer originally, minus
	 * how much is left in it. Note: it is not correct to use
	 * uio_offset for this!
	 */
	*retval = buflen - my_uio.uio_resid;
    spinlock_release(&ft->ft_spinlock);
	return 0;
}

/* END A4 SETUP */
