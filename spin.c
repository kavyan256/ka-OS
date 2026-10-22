#include "user.h"

// CPU-bound test program for preemption: prints its own letter forever and
// never yields or blocks. Two copies only interleave if the timer preempts.
void main(void) {
    char me = 'A' + getpid() - 2;   // first user pid is 2 -> 'A'
    printf("spin %c started\n", me);
    for (;;) {
        for (volatile int i = 0; i < 300000; i++)
            ;
        putchar(me);
    }
}
