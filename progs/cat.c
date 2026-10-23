#include "user.h"

#define FILE_MAX 8192   // matches FILE_DATA_MAX in kernel.h

void main(void) {
    char args[128];
    getargs(args, sizeof(args));

    // The filename is the first word of the arguments.
    char *name = args;
    for (char *p = args; *p; p++)
        if (*p == ' ') { *p = '\0'; break; }
    if (*name == '\0') {
        printf("usage: cat <file>\n");
        return;
    }

    char buf[FILE_MAX + 1];
    int len = readfile(name, buf, FILE_MAX);
    if (len < 0)
        return;                     // the kernel already printed "file not found"
    buf[len] = '\0';
    printf("%s", buf);
    if (len == 0 || buf[len - 1] != '\n')
        printf("\n");
}
