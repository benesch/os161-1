/*
 * killtest - test kill().
 *
 * This should work correctly when SIGKILL is implemented for kill().
 * To test that children sent a KILL signal actually exit properly, this
 * test uses waitpid to retrieve their exit status.
 *
 * Thus, this test really exercises the entire assignment (but not all the
 * corner cases for waitpid)
 */

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <signal.h>

/*
 * Helper function for fork that prints a warning on error.
 */
static
int
dofork(void)
{
	int pid;
	pid = fork();
	if (pid < 0) {
		warn("fork failed.");
	}
	if (pid == 0) {
		while (1) {}
	}
	return pid;
}
int parent;

static
int 
dofork2(void)
{
	int pid,me,i,ret;
	pid = fork();
	if (pid < 0) {
		warn("fork failed.");
	} else if (pid == 0) {
		me = getpid();
		/* child... try to stop previous */
		if (me-1 != parent) {
			warnx("Child %d, parent %d, trying to kill %d.",me,parent,me-1);
			ret = kill(me-1, SIGSTOP);
			if (ret != 0) {
				warn("kill of %d from %d failed.\n",me-1,me);
			}
		}
		for (i = 0; i < 1000 ;i++) {
			printf("%d",me);
		}
		warnx("child %d done, exiting.",me);
		exit(me);
	} 
	return pid;
}

/*
 * Actually run the test for signals that should cause termination.
 */
static
void
testsig_die(int signum, const char *signame)
{
	int pid0, ret, status;

	pid0 = dofork();
	warnx("Child %d created.",pid0);

	ret = kill(pid0, signum);
	if (ret == -1) {
		warn("kill failed.");
	}
	else {
		warnx("Child %d sent %s.",pid0, signame);
	}

	ret = waitpid(pid0, &status, 0);
	if (ret != pid0) {
		warn("waitpid failed (signal %s, status %d)",signame,WEXITSTATUS(status));
	} else {
		warnx("waitpid succeeded (signal %s, status %d, rawstatus %d).",signame,WEXITSTATUS(status),status);
		if (WIFSIGNALED(status)) {
			warnx("\t status indicates exit due to signal.");
		} else {
			warnx("\t status %d does NOT indicate exit due to signal.");
		}
	}
		
}
/*
 * Actually run the test for signals that should be ignored.
 */
static
void
testsig_ignore(int signum, const char *signame)
{
	int pid0, ret, status=123456;
	int i, ok=0;

	pid0 = dofork();
	warnx("Child %d created.",pid0);

	ret = kill(pid0, signum);
	if (ret == -1) {
		warn("kill failed.");
	}
	else {
		warnx("Child %d sent %s.",pid0,signame);
	}

	/* check repeatedly to make sure signaled child is still there */
	for (i=0; i < 100; i++) {
		ret = waitpid(pid0, &status, WNOHANG);
		if (ret != 0) {
			warn("waitpid with WNOHANG failed (%s)",signame);
		} else {
			ok++;
		}
	}
	if (ok == 100) {
		warnx("Success: signal %s appears to be ignored.",signame);
	}

	/* try killing child just to clean up */
	warnx("Sending SIGKILL to Child %d to clean up...",pid0);
	ret = kill(pid0, SIGKILL);
	if (ret == -1) {
		warn("kill failed.");
	}
	ret = waitpid(pid0, &status, 0);
	warnx("\tretrieved %d status from pid %d\n",status,pid0);
}

/* Test ability to signal non-child (siblings or parent */

static
void
testsig_circle()
{
	int pid1, pid2, pid3, pid4;
	int i, ret, status, realstatus;

	parent = getpid();
	warnx("circular stop and continue test, parent has pid %d.",parent);
	pid1 = dofork2();
	if (pid1 < 0) {
		warn("fork failed.");
	}
	pid2 = dofork2();
	if (pid2 < 0) {
		warn("fork failed.");
	}
	pid3 = dofork2();
	if (pid3 < 0) {
		warn("fork failed.");
	}
	pid4 = dofork2();
	if (pid4 < 0) {
		warn("fork failed.");
	}

	/* parent... keep busy for a while, then continue children */
	for (i=0; i < 1000000; i++)
		ret = random();

	warnx("Continuing child %d.",pid3);
	ret = kill(pid3, SIGCONT);

	for (i=0; i < 1000000; i++) 
		ret = random();

	warnx("Continuing child %d.",pid2);
	ret = kill(pid2, SIGCONT);
 
	for (i=0; i < 1000000; i++)
		ret = random();
	
	warnx("Continuing child %d.",pid1);
	ret = kill(pid1, SIGCONT);


	/* Get exitstatus from children */
	warnx("Getting exitstatus from %d.",pid1);
	ret = waitpid(pid1, &status, 0);
	realstatus = WEXITSTATUS(status);
	if (realstatus != pid1) {
		warn("Got status %d, expected %d.",realstatus,pid1);
	}

	warnx("Getting exitstatus from %d.",pid2);
	ret = waitpid(pid2, &status, 0);
	realstatus = WEXITSTATUS(status);
	if (realstatus != pid2) {
		warn("Got status %d, expected %d.",realstatus,pid2);
	}
	
	warnx("Getting exitstatus from %d.",pid3);
	ret = waitpid(pid3, &status, 0);
	realstatus = WEXITSTATUS(status);
	if (realstatus != pid3) {
		warn("Got status %d, expected %d.",realstatus,pid3);
	}

	warnx("Getting exitstatus from %d.",pid4);
	ret = waitpid(pid4, &status, 0);
	realstatus = WEXITSTATUS(status);
	if (realstatus != pid4) {
		warn("Got status %d, expected %d.",realstatus,pid4);
	}

}


int
main()
{
	warnx("Starting.");

	/* Test signals that should cause termination. */ 
	testsig_die(SIGHUP, "SIGHUP");
	testsig_die(SIGINT, "SIGINT");
	testsig_die(SIGKILL, "SIGKILL");
	testsig_die(SIGTERM, "SIGTERM");
	/* test signals that should be ignored */
	testsig_ignore(SIGWINCH, "SIGWINCH");
	testsig_ignore(SIGINFO, "SIGINFO");

	/* test stop and continue behavior? */
	testsig_circle();

	warnx("Complete.");
	return 0;
}
