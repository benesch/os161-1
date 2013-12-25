/* BEGIN A4 SETUP */
/*
 * File handles and file tables.
 * New for ASST4
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/limits.h>
#include <kern/stat.h>
#include <kern/unistd.h>
#include <kern/fcntl.h>
#include <file.h>
#include <syscall.h>
#include <lib.h>
#include <vfs.h>
#include <current.h>
#include <spinlock.h>

/*** openfile functions ***/

/*
 * file_open
 * opens a file, places it in the filetable, sets RETFD to the file
 * descriptor. the pointer arguments must be kernel pointers.
 * NOTE -- the passed in filename must be a mutable string.
 * 
 * A4: As per the OS/161 man page for open(), you do not need 
 * to do anything with the "mode" argument.
 */
int
file_open(char *filename, int flags, int mode, int *retfd)
{
    DEBUG(DB_VFS, "*** Opening file %s\n", filename);
    
    /* Check if flags is valid. */
    int how = flags & O_ACCMODE;
    if ((how != O_RDONLY) && (how != O_WRONLY) && (how != O_RDWR)) {
        return EINVAL;
    }
    
    /* Find a NULL entry in filetable. */
    int fd;
    struct filetable *ft = curthread->t_filetable;
    
	spinlock_acquire(&ft->ft_spinlock);
    for (fd = 0; fd < __OPEN_MAX; fd++) {
        if (ft->ft_entries[fd] == NULL) {
            break;
        }
    }
    
    /* File table is full. */
    if (fd == __OPEN_MAX) {
        spinlock_release(&ft->ft_spinlock);
        return EMFILE;
    }
    
    /* Open file. */
    struct vnode *new_vnode = NULL;
    int result = vfs_open(filename, flags, mode, &new_vnode);
    if (result > 0) {
        spinlock_release(&ft->ft_spinlock);
        return result;
    }
    
    ft->ft_entries[fd] = (struct filetable_entry *)kmalloc(sizeof(struct filetable_entry));
    ft->ft_entries[fd]->ft_vnode = new_vnode;
    ft->ft_entries[fd]->ft_pos = 0;
    ft->ft_entries[fd]->ft_flags = flags;
    ft->ft_entries[fd]->ft_count = 1;
    
    *retfd = fd;
    
    spinlock_release(&ft->ft_spinlock);
    return 0;
}


/* 
 * file_close
 * Called when a process closes a file descriptor.  
 */
int
file_close(int fd)
{
    DEBUG(DB_VFS, "*** Closing fd %d\n", fd);
    
    struct filetable *ft = curthread->t_filetable;
    spinlock_acquire(&ft->ft_spinlock);
    
    /* if fd is not a valid file descriptor, return error */
    if ((fd < 0) || (fd >= __OPEN_MAX) || (ft->ft_entries[fd] == NULL) ||
            (ft->ft_entries[fd]->ft_vnode == NULL)) {
        spinlock_release(&ft->ft_spinlock);
        return EBADF;
    }
    
    /* If there is no other fd pointing to this entry, close the file. */
    ft->ft_entries[fd]->ft_count--;
    if (ft->ft_entries[fd]->ft_count == 0) {
        vfs_close(ft->ft_entries[fd]->ft_vnode);
        kfree(ft->ft_entries[fd]);
    }
    
    /*Remove entry from file table. */
    ft->ft_entries[fd] = NULL;
    spinlock_release(&ft->ft_spinlock);
    
    return 0;
}

/*** filetable functions ***/

/* 
 * filetable_init
 * pretty straightforward -- allocate the space, set up 
 * first 3 file descriptors for stdin, stdout and stderr,
 * and initialize all other entries to NULL.
 * 
 * Should set curthread->t_filetable to point to the
 * newly-initialized filetable.
 * 
 * Should return non-zero error code on failure.  Currently
 * does nothing but returns success so that loading a user
 * program will succeed even if you haven't written the
 * filetable initialization yet.
 */

int
filetable_init(void)
{
    DEBUG(DB_VFS, "*** Initializing filetable\n");
    struct filetable *ft = (struct filetable *)kmalloc(sizeof(struct filetable));
    
    /* Initialize first 3 filedescriptors */
    int result;
    char path[5];
    strcpy(path, "con:");
    
    ft->ft_entries[0] = (struct filetable_entry *)kmalloc(sizeof(struct filetable_entry));
    struct vnode *cons_vnode = NULL;
    result = vfs_open(path, O_RDWR, 0, &cons_vnode);
    ft->ft_entries[0]->ft_vnode = cons_vnode;
    ft->ft_entries[0]->ft_pos = 0;
    ft->ft_entries[0]->ft_flags = O_RDWR;
    ft->ft_entries[0]->ft_count = 3;
    
    ft->ft_entries[1] = ft->ft_entries[0];
    ft->ft_entries[2] = ft->ft_entries[0];
    
    /* Initialize the rest of filetable entries to NULL. */
    int fd;
    for (fd = 3; fd < __OPEN_MAX; fd++) {
        ft->ft_entries[fd] = NULL;
    }
    
	spinlock_init(&ft->ft_spinlock);
    
    /* Update current thread's filetable field. */
    curthread->t_filetable = ft;
    
    return 0;
}	

/*
 * filetable_destroy
 * closes the files in the file table, frees the table.
 * This should be called as part of cleaning up a process (after kill
 * or exit).
 */
void
filetable_destroy(struct filetable *ft)
{
    DEBUG(DB_VFS, "*** Destroying filetable\n");
    int fd;
    for (fd = 0; fd < __OPEN_MAX; fd++) {
        struct filetable_entry *ft_entry = ft->ft_entries[fd];
        if (ft_entry != NULL) {
            file_close(fd);
        }
    }
    
	spinlock_cleanup(&ft->ft_spinlock);
    kfree(ft);
}	


/* 
 * You should add additional filetable utility functions here as needed
 * to support the system calls.  For example, given a file descriptor
 * you will want some sort of lookup function that will check if the fd is 
 * valid and return the associated vnode (and possibly other information like
 * the current file position) associated with that open file.
 */


/* END A4 SETUP */
