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

/*
 * triplemat.c
 *
 * 	Calls three matmult programs.
 *
 * When the VM assignment is complete, your system should survive this.
 */

#include <stdio.h>
#include <unistd.h>
#include <err.h>

/**********************************************************************
 *  Copied from matmult.c
 **********************************************************************/

#define Dim 	72	/* sum total of the arrays doesn't fit in
			 * physical memory
			 */

#define RIGHT  8772192		/* correct answer */

int A[Dim][Dim];
int B[Dim][Dim];
int C[Dim][Dim];
int T[Dim][Dim][Dim];

static
int
matmult()
{
    int i, j, k, r;

    for (i = 0; i < Dim; i++)		/* first initialize the matrices */
	for (j = 0; j < Dim; j++) {
	     A[i][j] = i;
	     B[i][j] = j;
	     C[i][j] = 0;
	}

    for (i = 0; i < Dim; i++)		/* then multiply them together */
	for (j = 0; j < Dim; j++)
            for (k = 0; k < Dim; k++)
		T[i][j][k] = A[i][k] * B[k][j];

    for (i = 0; i < Dim; i++)
	for (j = 0; j < Dim; j++)
            for (k = 0; k < Dim; k++)
		C[i][j] += T[i][j][k];

    r = 0;
    for (i = 0; i < Dim; i++)
	    r += C[i][i];

    printf("matmult finished.\n");
    printf("answer is: %d (should be %d)\n", r, RIGHT);
    if (r != RIGHT) {
	    printf("FAILED\n");
	    return 1;
    }
    printf("Passed.\n");
    return 0;
}
/**********************************************************************/

static
pid_t
spawnv(const char *prog)
{
	pid_t pid = fork();
	int result = 1;
	switch (pid) {
	    case -1:
    		err(1, "fork");
	    case 0:
            /* child */
            result = matmult();
            if (result > 0) {
                warnx("%s failed.\n", prog);
            }
            else {
                warnx("%s passed.\n", prog);
            }
	    default:
            /* parent */
    		break;
	}

	return pid;
}

int
main()
{
	int i;

	warnx("Starting: running three copies of matmult...");

	for (i=0; i<3; i++) {
		pid_t pid = spawnv("matmult");
		if (pid == 0) {
			break;
		}
	}
}

