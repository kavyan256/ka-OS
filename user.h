#pragma once
#include "common.h"

__attribute__((noreturn)) void exit(void);
void putchar(char ch);

int getchar(void);
int getpid(void);
int readfile(const char *filename, char *buf, int len);
int writefile(const char *filename, const char *buf, int len);
int listfile(int n, char *namebuf, int cap);
int deletefile(const char *filename);
int getargs(char *buf, int cap);
int exec(const char *name, const char *args);