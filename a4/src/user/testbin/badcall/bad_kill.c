/*
 * Copyright (c) 2012
 *	Angela Demke Brown - University of Toronto
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
 * Derived from other Harvard os161 badcall test code.
 */

/*
 * bad calls to waitpid()
 */

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <signal.h>
#include <stdio.h>

#include "config.h"
#include "test.h"

/* The set of all implemented signals */
sigset_t implemented_sigs = (1 << (SIGHUP-1))
	| (1 << (SIGINT-1))
	| (1 << (SIGKILL-1))
	| (1 << (SIGTERM-1))
	| (1 << (SIGSTOP-1))
	| (1 << (SIGCONT-1))
	| (1 << (SIGWINCH-1))
	| (1 << (SIGINFO-1));

static
void
kill_badpid(int pid, const char *desc)
{
	int rv;
	rv = kill(pid, 0);
	report_test2(rv, errno, EINVAL, NOSUCHPID_ERROR, desc);
}

/* We need to fork to get a pid before calling this function, because
 * the child should not exit, allowing the test to check that the signal
 * is checked correctly and not just check that the pid does not exist.
 */
static
void
kill_badsignal(int pid, int signum, const char *desc)
{
	int rv;

	rv = kill(pid, signum);
	report_test(rv, errno, EINVAL, desc);
}

/* We need to fork to get a pid before calling this function, because
 * the child should not exit, allowing the test to check that the signal
 * is checked correctly and not just check that the pid does not exist.
 */
static
void
kill_unimpsignal(int pid, int signum, const char *desc)
{
	int rv;

	rv = kill(pid, signum);
	report_test(rv, errno, EUNIMP, desc);
}

////////////////////////////////////////////////////////////

void
test_kill(void)
{
	int pid, i, ret;
	char desc[80];

	pid = fork();
	if (pid<0) {
		warn("UH-OH: fork failed");
		return;
	}
	if (pid==0) {
		while(1); /* Keep child around to send signals to */
	}

	kill_badpid(-8, "kill for pid -8");
	kill_badpid(-1, "kill for pid -1");
	kill_badpid(NONEXIST_PID, "nonexistent pid");


	kill_badsignal(pid, -1, "kill with -1 signal");
	kill_badsignal(pid, 32, "kill with signal num (32) too high");
	kill_badsignal(pid, 100, "kill with signal num (100) too high");

	for (i=1; i < 32; i++) {
		sigset_t sig = 1 << (i-1);
		if (!(sig & implemented_sigs)) {
			snprintf(desc, 80, "kill with unimplemented signal %d",i);
			kill_unimpsignal(pid, i, desc);
		}
	}
	
	/* Test use of kill with existing pid and signal 0 */
	ret = kill(pid, 0);
	if (ret != 0) {
		warn("FAILURE: kill with valid pid, signal 0");
	} else {
		warnx("passed: kill with valid pid, signal 0 returned 0");
	}

	/* Finally, send a terminate signal to the child to clean up */
	ret = kill(pid, SIGKILL);
	if (ret != 0) {
		warn("FAILURE: kill with valid pid, signal SIGKILL");
	} else {
		warnx("passed: kill with valid pid, signal SIGKILL returned 0");
	}
	
}
