#include "user.h"

void main(void) {
    char name[100];
    int size;
    for (int i = 0; (size = listfile(i, name, sizeof(name))) >= 0; i++)
        printf("%s  %d bytes\n", name, size);
}
