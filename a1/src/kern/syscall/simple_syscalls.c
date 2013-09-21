#include <types.h>
#include <lib.h>
#include <syscall.h>
#include <kern/errno.h>

/*
 * sys_printchar prints the given character c if it is a valid printable character,
 * returns 1 if the character is printed and an error code EINVAL otherwise.
 */
int
sys_printchar(char c) 
{
	int ascii = (int) c;
	if (ascii > 0 && ascii < 127 && kprintf("%c", c)) {
		return 1;
	}
	return EINVAL;
} 
