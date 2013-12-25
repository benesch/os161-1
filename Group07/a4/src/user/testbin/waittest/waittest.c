/*
 *  waittest - test the waitpid system call
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <signal.h>
#include <sys/wait.h>
/*
 * Helper function for fork that prints a warning on error.
 */
static
int
dofork(int exitval, int nloops)
{
	int pid, i;
	pid = fork();
	if (pid < 0) {
		warn("fork failed.");
	}
	if (pid == 0) {
		warnx("child starting loop.");
		for (i=0; i < nloops; i++) {
			pid = getpid();
		}
		warnx("child exiting with %d.",exitval);
		exit(exitval);
	}
	return pid;
}


int
main()
{
	int pid;
        int result, status;

	warnx("Starting.");

	/* Wait for child - parent should have to wait */ 
	warnx("Creating long-running child.  Parent should have to wait.");
	pid = dofork(10, 10000);
	result = waitpid(pid, &status, 0);
	if (result != pid) {
		warn("unexpected result %d from waitpid, status %d.",result,status);
	} else {
		warnx("waitpid returned status %d (raw %d).", WEXITSTATUS(status), status);
	}

	/* Wait for child - child should exit before parent does wait */
	warnx("Creating short-running child.  Parent should not have to wait.");
	pid = dofork(20, 0);
	result = waitpid(pid, &status, 0);
	if (result != pid) {
		warn("unexpected result %d from waitpid, status %d.",result,status);
	} else {
		warnx("waitpid returned status %d (raw %d).", WEXITSTATUS(status), status);
	}


	/* Wait for child, WNOHANG */
	warnx("Creating long-running child.  Parent should not have to wait (WNOHANG).");
	pid = dofork(30, 10000);
	status = 0xabababab; /* pattern should not be changed unless status is set */
	result = waitpid(pid, &status, WNOHANG);
	if (result != 0 || status != (int)0xabababab) {
		warn("unexpected result from waitpid (result %d, status 0x%x).",result,status);
	} else {
		warnx("waitpid returned status %d (raw %d).", WEXITSTATUS(status), status);
	}

	warnx("Complete.");
	return 0;
}
