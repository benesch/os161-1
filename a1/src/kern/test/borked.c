#include <types.h>
#include <lib.h>
#include <kern/errno.h>
#include <test.h>

#define TRUE  1
#define FALSE 0

static 
void show1(const char *s) {
	int i;

	for (i = 0; i < strlen(s); i++) 
		kprintf("%c", s[i]);
	kprintf("\n");
}

static 
void show2(const char *s) {
	int i;
	for (i = 0; i <= strlen(s) - 1; i++) 
		kprintf("%c", s[i]);
	kprintf("\n");
}

static
void test1() {
	show1("");
	show2("");
}

/************************************************************/

static
int is_equal(int a, int b) {
	if ((a = b))
		return TRUE;
	else
		return FALSE;
}

static
void test2 () {
	int x = 7;
	int y = 42;

	if (is_equal(x, y))
		kprintf("%d is equal to %d\n", x, y);
	else
		kprintf("%d is NOT equal to %d\n",x,y);
}

/**************************************************************/

struct bar {
	int field1;
	int field2;
	int field3;
};

/* init_bar takes a pointer to a struct bar and initializes its fields */
static
void init_bar(struct bar *b, int val1, int val2, int val3) {
	b->field1 = val1;
	b->field2 = val2;
	b->field3 = val3;
}

static 
int sum(int x, int y, int z) {
	int sum = x+y+z;
	kprintf("The sum of %d+%d+%d is %d\n",x,y,z,sum);
	return sum;
}


static
struct bar *helper(int x, int y, int z) {
	struct bar *mybar;
	
	init_bar(mybar, x, y, z);
	return mybar;
}	 

static
void test3() {
	int x = 42;
	int y = 92;
	int z = 86;
	int expectedsum = sum(x,y,z);
	struct bar *thebar = helper(x,y,z);
	int realsum = sum(thebar->field1, thebar->field2, thebar->field3);

	if (realsum == expectedsum)
		kprintf("Success: sums match\n");
	else
		kprintf("Failure: sum should be %d but got %d\n",
			expectedsum, realsum);
}

/**************************************************************/

/* test4 is expected to fail, given the definition of SIZE.
 * However, it should fail gracefully by returning an error
 * code, rather than crashing the kernel.
 * DO NOT MODIFY SIZE.
 */

#define SIZE 16*1024*1024 /* 16 MB */

static
int test4() {
	char *buf = (char *)kmalloc(SIZE);
	
	strcpy(buf,"Supercalifragilisticexpialidocious");
	
	kfree(buf);
	return 0;
}

/**************************************************************/

int dbgtest(int nargs, char **args) {
	int testnum=0;

	if (nargs != 2) {
		kprintf("Usage: dbgtest testnum\n");
		kprintf("Use 0 to run all tests.\n");
		return EINVAL;
	}

	testnum=atoi(args[1]);

	switch (testnum) {
	case 0:
		kprintf("Running all a1 debugging tests (1-4)\n");
		kprintf("Running a1 debugging test 1\n");
		test1();
		kprintf("Running a1 debugging test 2\n");
		test2();
		kprintf("Running a1 debugging test 3\n");
		test3();
		kprintf("Running a1 debugging test 4. Returns %d\n",test4());
		break;
	case 1:
		kprintf("Running a1 debugging test 1\n");
		test1();
		break;
	case 2:
		kprintf("Running a1 debugging test 2\n");
		test2();
		break;
	case 3:
		kprintf("Running a1 debugging test 3\n");
		test3();
		break;
	case 4:
		kprintf("Running a1 debugging test 4. Returns %d\n",test4());
		break;
	default:
		kprintf("testnum must be between 0 and 4 (0 runs all tests)\n");
		return EINVAL;
	}

	return 0;
}
