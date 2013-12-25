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
#include <kern/reboot.h>
#include <kern/unistd.h>
#include <kern/sysexits.h>
#include <limits.h>
#include <lib.h>
#include <uio.h>
#include <clock.h>
#include <thread.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>

/* BEGIN A3 SETUP */
#include "opt-dumbvm.h" /* To include coremaptests only when not using dumbvm */
#include <vm.h> 
/* END A3 SETUP */

/* BEGIN A4 SETUP */
/* Needed to include optional sfs code */
#include "opt-sfs.h"

#if OPT_SFS
#include <sfs.h>
#endif

/* Hacky semaphore solution to make menu thread wait for command
 * thread, in absence of thread_join solution.
 */
#include <synch.h>
#include <current.h>
struct semaphore *cmd_sem;
int progthread_pid;

/* END A4 SETUP */

/*
 * In-kernel menu and command dispatcher.
 */

#define _PATH_SHELL "/bin/sh"

#define MAXMENUARGS  16

// XXX this should not be in this file
void
getinterval(time_t s1, uint32_t ns1, time_t s2, uint32_t ns2,
	    time_t *rs, uint32_t *rns)
{
	if (ns2 < ns1) {
		ns2 += 1000000000;
		s2--;
	}

	*rns = ns2 - ns1;
	*rs = s2 - s1;
}

////////////////////////////////////////////////////////////
//
// Command menu functions 

/* demke: new routines to copy args for safe passing to child thread, and
 * to free the memory when the args are no longer needed
 */

static
void
free_args(int nargs, char **args)
{
	int j;
	for (j=0; j < nargs; j++) {
		kfree(args[j]);
	}
	kfree(args);
}

static
char **
copy_args(int nargs, char **args)
{
	char **the_copy;
	int i;
	
	the_copy = (char **)kmalloc(nargs * sizeof(char *));
	if (!the_copy) {
		kprintf("Could not allocate memory for copy of args");
		return NULL;
	}
	for (i=0; i < nargs; i++) {
		the_copy[i] = kstrdup(args[i]);	
		if (!the_copy[i]) {
			kprintf("Could not allocate memory for copy of argument %d\n",i);
			/* unwind existing allocations */
			free_args(i, the_copy);
			return NULL;
		}
	}

	return the_copy;
}


/*
 * Function for a thread that runs an arbitrary userlevel program by
 * name.
 *
 * Note: this cannot pass arguments to the program. You may wish to 
 * change it so it can, because that will make testing much easier
 * in the future.
 *
 * It copies the program name because runprogram destroys the copy
 * it gets by passing it to vfs_open(). 
 */
static
void
cmd_progthread(void *ptr, unsigned long nargs)
{
	char **args = ptr;
	char progname[128];
	char progname2[128];
	int result;

	/* BEGIN A4 SETUP */
	/* Record pid of progthread, so only this thread will do a V()
	 * on the semaphore when it exits.
	 */
	progthread_pid = curthread->t_pid;
	/* END A4 SETUP */

	KASSERT(nargs >= 1);

	if (nargs > 2) {
		kprintf("Warning: argument passing from menu not supported\n");
	}

	/* Hope we fit. */
	KASSERT(strlen(args[0]) < sizeof(progname));

	strcpy(progname, args[0]);
	strcpy(progname2,args[0]); /* demke: make extra copy for runprogram */
	free_args(nargs, args);

	result = runprogram(progname2);
	if (result) {
		kprintf("Running program %s failed: %s\n", progname,
			strerror(result));
		return;
	}

	/* NOTREACHED: runprogram only returns on error. */
}

/*
 * Common code for cmd_prog and cmd_shell.
 *
 * Note that this does not wait for the subprogram to finish, but
 * returns immediately to the menu. This is usually not what you want,
 * so you should have it call your system-calls-assignment waitpid
 * code after forking.
 *
 * Also note that because the subprogram's thread uses the "args"
 * array and strings, until you do this a race condition exists
 * between that code and the menu input code.
 */



static
int
common_prog(int nargs, char **args)
{
	int result;
	char **args_copy;
#if OPT_SYNCHPROBS
	kprintf("Warning: this probably won't work with a "
		"synchronization-problems kernel.\n");
#endif

	/* demke: Make a copy of arguments to pass to new thread,
	 * so that we aren't depending on parent's stack!
	 */
	args_copy = copy_args(nargs, args);
	if (!args_copy) {
		return ENOMEM;
	}

	/* demke: and now call thread_fork with the copy */
	
	result = thread_fork(args_copy[0] /* thread name */,
			cmd_progthread /* thread function */,
			args_copy /* thread arg */, nargs /* thread arg */,
			NULL);
	if (result) {
		kprintf("thread_fork failed: %s\n", strerror(result));
		/* demke: need to free copy of args if fork fails */
		free_args(nargs, args_copy);
		return result;
	}

	/* BEGIN A4 SETUP */
	/* This is not needed if you have a working pid_join -
	 * that should be used instead.
	 */
	/* Wait for progthread to finish and send a V() */
	P(cmd_sem);
	/* END A4 SETUP */

	return 0;
}

/*
 * Command for running an arbitrary userlevel program.
 */
static
int
cmd_prog(int nargs, char **args)
{
	if (nargs < 2) {
		kprintf("Usage: p program [arguments]\n");
		return EINVAL;
	}

	/* drop the leading "p" */
	args++;
	nargs--;

	return common_prog(nargs, args);
}

/*
 * Command for printing debugger flags.
 */
static
void
dbflags_print() {
    int i;
    if (dbflags != 0) {
		kprintf("ENABLED flags: ");
		for (i = 0; strcmp(flag_name[i], "/0") != 0; i++) {
            if (dbflags & (1 << i))
				kprintf("%s ", flag_name[i]);
        }
		kprintf("\n");
    }
    else
        kprintf("All flags are DISABLED.\n");
	return;
}

/*
 * Command for setting debugger flags.
 */
static
int
cmd_dbflags(int nargs, char **args) {
    uint32_t mask = 0;
    int n, m;

    if (nargs == 2 && strcmp(args[1], "print") == 0) {
		dbflags_print();
		return 0;
    }
    if (nargs > 2 && 
        (strcmp(args[1], "+") == 0 || strcmp(args[1], "-") == 0)) {
	
        // Get flags
        for (n = 2; n < nargs; n++) {
            for (m = 0; strcmp(flag_name[m], "/0") != 0; m++) {
                if (strcmp(flag_name[m], args[n]) == 0) {
					mask = mask | (1 << m);
					break;
                }
            }
        }

        if (strcmp(args[1], "+") == 0) 
			dbflags = dbflags | mask;
        else 
			dbflags = dbflags & ~mask;

		dbflags_print();
        return 0;
    }
	
    // Usage not recognized, so print error and exit.
    kprintf("Usage: dbflags [ + FLAGNAME ... | - FLAGNAME ... | print ]\n\n");
    return 0;	
}

/*
 * Command for starting the system shell.
 */
static
int
cmd_shell(int nargs, char **args)
{
	(void)args;
	if (nargs != 1) {
		kprintf("Usage: s\n");
		return EINVAL;
	}

	args[0] = (char *)_PATH_SHELL;

	return common_prog(nargs, args);
}

/* BEGIN A4 SETUP */
/*
 * Command for creating a directory.
 */
static
int
cmd_mkdir(int nargs, char **args)
{
	if (nargs != 2) {
		kprintf("Usage: mkdir directory\n");
		return EINVAL;
	}

	return vfs_mkdir(args[1], 0); /* mode is ignored */
}

/*
 * Command for removing a directory.
 */
static
int
cmd_rmdir(int nargs, char **args)
{
	if (nargs != 2) {
		kprintf("Usage: rmdir directory\n");
		return EINVAL;
	}

	return vfs_rmdir(args[1]);
}
/* END A4 SETUP */

/*
 * Command for changing directory.
 */
static
int
cmd_chdir(int nargs, char **args)
{
	if (nargs != 2) {
		kprintf("Usage: cd directory\n");
		return EINVAL;
	}

	return vfs_chdir(args[1]);
}

/*
 * Command for printing the current directory.
 */
static
int
cmd_pwd(int nargs, char **args)
{
	char buf[PATH_MAX+1];
	int result;
	struct iovec iov;
	struct uio ku;

	(void)nargs;
	(void)args;

	uio_kinit(&iov, &ku, buf, sizeof(buf)-1, 0, UIO_READ);
	result = vfs_getcwd(&ku);
	if (result) {
		kprintf("vfs_getcwd failed (%s)\n", strerror(result));
		return result;
	}

	/* null terminate */
	buf[sizeof(buf)-1-ku.uio_resid] = 0;

	/* print it */
	kprintf("%s\n", buf);

	return 0;
}

/*
 * Command for running sync.
 */
static
int
cmd_sync(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	vfs_sync();

	return 0;
}

/*
 * Command for doing an intentional panic.
 */
static
int
cmd_panic(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	panic("User requested panic\n");
	return 0;
}

/*
 * Command for shutting down.
 */
static
int
cmd_quit(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	vfs_sync();
	sys_reboot(RB_POWEROFF);
	thread_exit(EX_OK);
	return 0;
}

/*
 * Command for mounting a filesystem.
 */

/* Table of mountable filesystem types. */
static const struct {
	const char *name;
	int (*func)(const char *device);
} mounttable[] = {
/* BEGIN A4 SETUP */
#if OPT_SFS
        { "sfs", sfs_mount },
#endif
/* END A4 SETUP */
	{ NULL, NULL }
};

static
int
cmd_mount(int nargs, char **args)
{
	char *fstype;
	char *device;
	int i;

	if (nargs != 3) {
		kprintf("Usage: mount fstype device:\n");
		return EINVAL;
	}

	fstype = args[1];
	device = args[2];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	for (i=0; mounttable[i].name; i++) {
		if (!strcmp(mounttable[i].name, fstype)) {
			return mounttable[i].func(device);
		}
	}
	kprintf("Unknown filesystem type %s\n", fstype);
	return EINVAL;
}

static
int
cmd_unmount(int nargs, char **args)
{
	char *device;

	if (nargs != 2) {
		kprintf("Usage: unmount device:\n");
		return EINVAL;
	}

	device = args[1];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	return vfs_unmount(device);
}

/*
 * Command to set the "boot fs". 
 *
 * The boot filesystem is the one that pathnames like /bin/sh with
 * leading slashes refer to.
 *
 * The default bootfs is "emu0".
 */
static
int
cmd_bootfs(int nargs, char **args)
{
	char *device;

	if (nargs != 2) {
		kprintf("Usage: bootfs device\n");
		return EINVAL;
	}

	device = args[1];

	/* Allow (but do not require) colon after device name */
	if (device[strlen(device)-1]==':') {
		device[strlen(device)-1] = 0;
	}

	return vfs_setbootfs(device);
}

static
int
cmd_kheapstats(int nargs, char **args)
{
	(void)nargs;
	(void)args;

	kheap_printstats();
	
	return 0;
}

////////////////////////////////////////
//
// Menus.

static
void
showmenu(const char *name, const char *x[])
{
	int ct, half, i;

	kprintf("\n");
	kprintf("%s\n", name);
	
	for (i=ct=0; x[i]; i++) {
		ct++;
	}
	half = (ct+1)/2;

	for (i=0; i<half; i++) {
		kprintf("    %-36s", x[i]);
		if (i+half < ct) {
			kprintf("%s", x[i+half]);
		}
		kprintf("\n");
	}

	kprintf("\n");
}

static const char *opsmenu[] = {
	"[s]       Shell                     ",
	"[p]       Other program             ",
	"[dbflags] View or set debug flags   ",
	"[mount]   Mount a filesystem        ",
	"[unmount] Unmount a filesystem      ",
	"[bootfs]  Set \"boot\" filesystem     ",
	"[pf]      Print a file              ",
	/* BEGIN A4 SETUP */
	"[mkdir]   Create a directory        ",
	"[rmdir]   Remove a directory        ",
	/* END A4 SETUP */
	"[cd]      Change directory          ",
	"[pwd]     Print current directory   ",
	"[sync]    Sync filesystems          ",
	"[panic]   Intentional panic         ",
	"[q]       Quit and shut down        ",
	NULL
};

static
int
cmd_opsmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 operations menu", opsmenu);
	return 0;
}

static const char *testmenu[] = {
	"[at]  Array test                    ",
	"[bt]  Bitmap test                   ",
	"[km1] Kernel malloc test            ",
	"[km2] kmalloc stress test           ",
	"[tt1] Thread test 1                 ",
	"[tt2] Thread test 2                 ",
	"[tt3] Thread test 3                 ",
#if OPT_NET
	"[net] Network test                  ",
#endif
	"[sy1] Semaphore test                ",
	"[sy2] Lock test             (1)     ",
	"[sy3] CV test               (1)     ",
/* BEGIN A3 SETUP */
/* Only include coremap tests if not using dumbvm */
#if !OPT_DUMBVM
        /* ASST3 tests */
	"[cm] Coremap test           (3)     ",
	"[cm2] Coremap stress test   (3)     ",
#endif
/* END A3 SETUP */
	"[fs1] Filesystem test               ",
	"[fs2] FS read stress        (4)     ",
	"[fs3] FS write stress       (4)     ",
	"[fs4] FS write stress 2     (4)     ",
	"[fs5] FS long stress        (4)     ",
	NULL
};

static
int
cmd_testmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 tests menu", testmenu);
	kprintf("    (1) These tests will fail until you finish the "
		"synch assignment.\n");
	kprintf("    (4) These tests may fail until you finish the "
		"file system assignment.\n");
	kprintf("\n");

	return 0;
}

static const char *mainmenu[] = {
	"[?o] Operations menu                ",
	"[?t] Tests menu                     ",
	"[kh] Kernel heap stats              ",
/* BEGIN A3 SETUP */
/* Only include vm stats if not using dumbvm */
#if !OPT_DUMBVM
        /* ASST3 vm stats */
        "[vm] Virtual memory stats           ", 
#endif
/* END A3 SETUP */

	"[q] Quit and shut down              ",
	NULL
};

static
int
cmd_mainmenu(int n, char **a)
{
	(void)n;
	(void)a;

	showmenu("OS/161 kernel menu", mainmenu);
	return 0;
}

////////////////////////////////////////
//
// Command table.

static struct {
	const char *name;
	int (*func)(int nargs, char **args);
} cmdtable[] = {
	/* menus */
	{ "?",		cmd_mainmenu },
	{ "h",		cmd_mainmenu },
	{ "help",	cmd_mainmenu },
	{ "?o",		cmd_opsmenu },
	{ "?t",		cmd_testmenu },

	/* operations */
	{ "s",		cmd_shell },
	{ "p",		cmd_prog },
	{ "dbflags", cmd_dbflags },
	{ "mount",	cmd_mount },
	{ "unmount",	cmd_unmount },
	{ "bootfs",	cmd_bootfs },
	{ "pf",		printfile },
	/* BEGIN A4 SETUP */
        { "mkdir",      cmd_mkdir },
        { "rmdir",      cmd_rmdir },
	/* END A4 SETUP */
	{ "cd",		cmd_chdir },
	{ "pwd",	cmd_pwd },
	{ "sync",	cmd_sync },
	{ "panic",	cmd_panic },
	{ "q",		cmd_quit },
	{ "exit",	cmd_quit },
	{ "halt",	cmd_quit },

	/* stats */
	{ "kh",         cmd_kheapstats },
/* BEGIN A3 SETUP */
/* Only include vm stats if not using dumbvm */
#if !OPT_DUMBVM
        /* ASST3 vm stats */
        { "vm",         vm_printstats },
#endif
/* END A3 SETUP */

	/* base system tests */
	{ "at",		arraytest },
	{ "bt",		bitmaptest },
	{ "km1",	malloctest },
	{ "km2",	mallocstress },
#if OPT_NET
	{ "net",	nettest },
#endif
	{ "tt1",	threadtest },
	{ "tt2",	threadtest2 },
	{ "tt3",	threadtest3 },
	{ "sy1",	semtest },

	/* synchronization assignment tests */
	{ "sy2",	locktest },
	{ "sy3",	cvtest },

	/* ASST2 tests */
	/* For testing the wait implementation. */
	{ "wt",		waittest },

/* BEGIN A3 SETUP */
/* Only include coremap tests if not using dumbvm */
#if !OPT_DUMBVM
	/* ASST3 tests */
	{ "cm",		coremaptest },
	{ "cm2",	coremapstress },
#endif
/* END A3 SETUP */

	/* file system assignment tests */
	{ "fs1",	fstest },
	{ "fs2",	readstress },
	{ "fs3",	writestress },
	{ "fs4",	writestress2 },
	{ "fs5",	longstress },

	{ NULL, NULL }
};

/*
 * Process a single command.
 */
static
int
cmd_dispatch(char *cmd)
{
	time_t beforesecs, aftersecs, secs;
	uint32_t beforensecs, afternsecs, nsecs;
	char *args[MAXMENUARGS];
	int nargs=0;
	char *word;
	char *context;
	int i, result;

	for (word = strtok_r(cmd, " \t", &context);
	     word != NULL;
	     word = strtok_r(NULL, " \t", &context)) {

		if (nargs >= MAXMENUARGS) {
			kprintf("Command line has too many words\n");
			return E2BIG;
		}
		args[nargs++] = word;
	}

	if (nargs==0) {
		return 0;
	}

	for (i=0; cmdtable[i].name; i++) {
		if (*cmdtable[i].name && !strcmp(args[0], cmdtable[i].name)) {
			KASSERT(cmdtable[i].func!=NULL);

			gettime(&beforesecs, &beforensecs);

			result = cmdtable[i].func(nargs, args);

			gettime(&aftersecs, &afternsecs);
			getinterval(beforesecs, beforensecs,
				    aftersecs, afternsecs,
				    &secs, &nsecs);

			kprintf("Operation took %lu.%09lu seconds\n",
				(unsigned long) secs,
				(unsigned long) nsecs);

			return result;
		}
	}

	kprintf("%s: Command not found\n", args[0]);
	return EINVAL;
}

/*
 * Evaluate a command line that may contain multiple semicolon-delimited
 * commands.
 *
 * If "isargs" is set, we're doing command-line processing; print the
 * comamnds as we execute them and panic if the command is invalid or fails.
 */
static
void
menu_execute(char *line, int isargs)
{
	char *command;
	char *context;
	int result;

	for (command = strtok_r(line, ";", &context);
	     command != NULL;
	     command = strtok_r(NULL, ";", &context)) {

		if (isargs) {
			kprintf("OS/161 kernel: %s\n", command);
		}

		result = cmd_dispatch(command);
		if (result) {
			kprintf("Menu command failed: %s\n", strerror(result));
			if (isargs) {
				panic("Failure processing kernel arguments\n");
			}
		}
	}
}

/*
 * Command menu main loop.
 *
 * First, handle arguments passed on the kernel's command line from
 * the bootloader. Then loop prompting for commands.
 *
 * The line passed in from the bootloader is treated as if it had been
 * typed at the prompt. Semicolons separate commands; spaces and tabs
 * separate words (command names and arguments).
 *
 * So, for instance, to run the arraytest, print the kernel heap statistics,
 * and then quit, one would use the kernel command line
 *
 *      "at; kh; q"
 */

void
menu(char *args)
{
	char buf[64];

	/* BEGIN A4 SETUP */
	/* Initialize hacky semaphore solution to make menu thread 
	 * wait for command program to finish.
	 */
	cmd_sem = sem_create("cmdsem", 0);
	if (cmd_sem == NULL) {
		panic("menu: could not create cmd_sem\n");
	}
	/* END A4 SETUP */

	menu_execute(args, 1);

	while (1) {
		kprintf("OS/161 kernel [? for menu]: ");
		kgets(buf, sizeof(buf));
		menu_execute(buf, 0);
	}
}
