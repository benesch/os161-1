#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <err.h>


#ifdef HOST
#include "hostcompat.h"
#endif

extern int cmd_cat(int, char **);
extern int cmd_cp(int, char **);
extern int cmd_ls(int, char **);
extern int cmd_mkdir(int, char **);
extern int cmd_rmdir(int, char **);
extern int cmd_rm(int, char **);
extern int cmd_pwd(int, char **);
extern int cmd_sync(int, char **);
extern int cmd_ln(int, char **);
extern int cmd_mv(int, char **);
extern int cmd_opentest(int, char **);
