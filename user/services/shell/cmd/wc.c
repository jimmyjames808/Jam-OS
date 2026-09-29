/* wc: count lines, words and bytes of a file or the pipe (-l -w -c: only
 * those). */
#include "sh.h"

SH_CMD(wc)
{
    bool l = false, w = false, c = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'l')
                l = true;
            else if (*p == 'w')
                w = true;
            else if (*p == 'c')
                c = true;
            else {
                sh_tty("usage: wc [-l|-w|-c] [file]\n");
                return 2;
            }
        }
    if (!l && !w && !c)
        l = w = c = true;
    const char *d;
    size_t n;
    if (!sh_input("wc", argc, argv, i, &d, &n))
        return 1;
    uint64_t lines = 0, words = 0;
    bool in_word = false;
    for (size_t k = 0; k < n; k++) {
        char ch = d[k];
        if (ch == '\n')
            lines++;
        bool space = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
        if (!space && !in_word)
            words++;
        in_word = !space;
    }
    if (l)
        sh_say(l && !w && !c ? "%lu" : "%7lu", (unsigned long)lines);
    if (w)
        sh_say(!l && !c ? "%lu" : " %7lu", (unsigned long)words);
    if (c)
        sh_say(!l && !w ? "%lu" : " %7lu", (unsigned long)n);
    if (i < argc)
        sh_say(" %s", argv[i]);
    sh_say("\n");
    return 0;
}
