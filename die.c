#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "config.h"

#ifndef HAVE_SYS_ERRLIST_DECLARED   /* RHLinux defines this in stdio.h -- not very std. */
extern char *sys_errlist[];  /* builtin list of text error messages */
#endif 

/* die() - print an error message and exit */
/* notes:  prints text message from errno  */
/*         parameters just like printf()   */
void die(char *format, ...)
{
    va_list parms;
    va_start(parms, format);
    printf("Fatal error: ");
    vprintf(format, parms);
    if (0 != errno)
    {
        printf(" (%s)\n", strerror(errno));
    }
    else
    {
        printf("\n");
    }
    exit(1);
}














