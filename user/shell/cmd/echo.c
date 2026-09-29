/* echo: the words, separated by spaces; -n no newline, -e \n \t \e
 * escapes. */
#include "../sh.h"

/* -n / -e words at the start: the index of the first word to print. */
static int options(int argc, char **argv, bool *newline, bool *esc)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        bool ok = true;
        for (const char *p = argv[i] + 1; *p; p++)
            ok &= *p == 'n' || *p == 'e';
        if (!ok)
            break;   /* an ordinary word that starts with - */
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n')
                *newline = false;
            else
                *esc = true;
        }
    }
    return i;
}

static void put_escaped(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == '\\' && s[1]) {
            s++;
            c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'e' ? '\033' : *s;
        }
        sh_put(&c, 1);
    }
}

SH_CMD(echo)
{
    bool newline = true, esc = false;
    int i = options(argc, argv, &newline, &esc);
    for (int k = i; k < argc; k++) {
        if (k > i)
            sh_put(" ", 1);
        if (esc)
            put_escaped(argv[k]);
        else
            sh_put(argv[k], strlen(argv[k]));
    }
    if (newline)
        sh_put("\n", 1);
    return 0;
}
