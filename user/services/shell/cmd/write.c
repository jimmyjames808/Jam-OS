/* write: text from the command line into a file, the words joined by
 * spaces and ended with a newline; with no text, the pipe's input as it
 * is. The file is replaced, or added to with -a. */
#include "sh.h"

/* argv[first..argc) joined by spaces, with a newline: malloc'd, *len bytes. */
static char *join_words(int argc, char **argv, int first, size_t *len)
{
    size_t n = 0;
    for (int i = first; i < argc; i++)
        n += strlen(argv[i]) + 1;
    char *text = malloc(n + 1);
    if (!text)
        return NULL;
    size_t at = 0;
    for (int i = first; i < argc; i++) {
        memcpy(text + at, argv[i], strlen(argv[i]));
        at += strlen(argv[i]);
        text[at++] = i + 1 < argc ? ' ' : '\n';
    }
    *len = at;
    return text;
}

SH_CMD(write)
{
    int first = 1;
    bool append = argc > 1 && !strcmp(argv[1], "-a");
    if (append)
        first = 2;
    const char *data = NULL;
    size_t len = 0;
    if (first >= argc || (first + 1 == argc && !sh_stdin(&data, &len))) {
        sh_tty("usage: write [-a] <file> <text...>   (or: command | write [-a] <file>)\n");
        return 2;
    }
    char *text = first + 1 < argc ? join_words(argc, argv, first + 1, &len) : NULL;
    char abs[SH_PATH_MAX] = "";
    status_t st = first + 1 < argc && !text ? ERR_NO_MEMORY
                  : sh_resolve(argv[first], abs, sizeof(abs)) ? OK : ERR_INVALID_ARGS;
    if (st == OK)
        st = sh_write(abs, text ? text : data, len, append ? FS_APPEND : FS_TRUNCATE);
    free(text);
    if (st != OK) {
        bool dir = false;
        uint64_t size;
        bool is_dir = abs[0] && sh_stat(abs, &dir, &size) == OK && dir;
        sh_tty("write: %s: %s\n", argv[first], is_dir ? "is a directory" : sh_why(st));
    }
    return st == OK ? 0 : 1;
}
