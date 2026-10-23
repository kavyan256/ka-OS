#include "user.h"

#define MAX_ARGS 8
#define FILE_BUF 1024

static int strlen_(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void help(void) {
    printf("commands:\n"
           "  help                  show this list\n"
           "  hello                 say hello\n"
           "  echo <text>           print text\n"
           "  echo <text> > <file>  write text to a file\n"
           "  write <file> <text>   write text to a file\n"
           "  rm <file>             delete a file\n"
           "  run <prog> [args]     run a program from disk\n"
           "  exit                  exit the shell\n"
           "programs on disk (or any program name):\n"
           "  ls                    list files\n"
           "  cat <file>            print a file\n");
}

// Run a program from disk and wait for it. Returns false if it could not
// be started (no such file, not a program, or no free process slot).
static bool run_program(const char *name, const char *args) {
    int pid = exec(name, args);
    if (pid < 0)
        return false;
    wait(pid);
    return true;
}

// Write `text` plus a newline to `name`.
static void cmd_write(const char *name, const char *text) {
    char buf[FILE_BUF];
    int len = strlen_(text);
    if (len > FILE_BUF - 1) {
        printf("text too long (max %d bytes)\n", FILE_BUF - 1);
        return;
    }
    memcpy(buf, text, len);
    buf[len++] = '\n';
    if (writefile(name, buf, len) < 0)
        printf("write failed: %s\n", name);
}

void main(void) {
    while (1) {
prompt:
        printf(">> ");
        char cmdline[128];
        for (int i = 0;; i++) {
            char ch = getchar();
            putchar(ch);
            if (i == sizeof(cmdline) - 1) {
                printf("command line too long\n");
                goto prompt;
            } else if (ch == '\r') {
                printf("\n");
                cmdline[i] = '\0';
                break;
            } else {
                cmdline[i] = ch;
            }
        }

        // Split the line into words. The splitting breaks cmdline apart, so
        // keep a whole copy in `line`. rest[i] = the text after word i.
        char line[sizeof(cmdline)];
        memcpy(line, cmdline, sizeof(cmdline));
        char *argv[MAX_ARGS];
        char *rest[MAX_ARGS];
        int argc = 0;
        char *p = cmdline;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            argv[argc] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = '\0';
            while (*p == ' ')
                p++;
            rest[argc++] = line + (p - cmdline);
        }
        if (argc == 0)
            continue;

        const char *cmd = argv[0];
        if (strcmp(cmd, "help") == 0)
            help();
        else if (strcmp(cmd, "hello") == 0)
            printf("Hello world from shell!\n");
        else if (strcmp(cmd, "exit") == 0)
            exit();
        else if (strcmp(cmd, "rm") == 0) {
            if (argc < 2)
                printf("usage: rm <file>\n");
            else if (deletefile(argv[1]) < 0)
                printf("rm: no such file: %s\n", argv[1]);
        } else if (strcmp(cmd, "run") == 0) {
            if (argc < 2)
                printf("usage: run <program> [args]\n");
            else if (!run_program(argv[1], argc > 2 ? rest[1] : ""))
                printf("run: cannot execute: %s\n", argv[1]);
        } else if (strcmp(cmd, "write") == 0) {
            if (argc < 2)
                printf("usage: write <file> <text>\n");
            else
                cmd_write(argv[1], rest[1]);
        } else if (strcmp(cmd, "echo") == 0) {
            // echo a b c  or  echo a b c > file
            int redir = -1;
            for (int i = 1; i < argc; i++)
                if (strcmp(argv[i], ">") == 0)
                    redir = i;
            if (redir < 0) {
                printf("%s\n", rest[0]);
            } else if (redir != argc - 2) {
                printf("usage: echo <text> > <file>\n");
            } else {
                // The text is whatever comes before the '>'. Cut it there.
                char *end = line + (argv[redir] - cmdline);
                while (end > rest[0] && end[-1] == ' ')
                    end--;
                *end = '\0';
                cmd_write(argv[redir + 1], rest[0]);
            }
        } else if (!run_program(cmd, rest[0]))
            printf("unknown command: %s (try help)\n", cmd);
    }
}
