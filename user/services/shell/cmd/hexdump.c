/* hexdump (hd): hex and ASCII, 16 bytes a line, of a file or the pipe;
 * -s offset, -n bytes (-C, the format it always uses, is accepted). */
#include "sh.h"

static void hex_line(const char *d, uint64_t off, uint64_t end)
{
    char line[96];
    int k = snprintf(line, sizeof(line), "%08lx  ", (unsigned long)off);
    for (int j = 0; j < 16; j++) {
        if (off + j < end)
            k += snprintf(line + k, sizeof(line) - k, "%02x ", (uint8_t)d[off + j]);
        else
            k += snprintf(line + k, sizeof(line) - k, "   ");
        if (j == 7)
            line[k++] = ' ';
    }
    line[k++] = ' ';
    line[k++] = '|';
    for (int j = 0; j < 16 && off + j < end; j++) {
        char c = d[off + j];
        line[k++] = c >= 0x20 && c < 0x7f ? c : '.';
    }
    line[k++] = '|';
    line[k++] = '\n';
    sh_put(line, (size_t)k);
}

SH_CMD(hexdump)
{
    uint64_t skip = 0, limit = UINT64_MAX;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-C"))
            continue;
        uint64_t *what = !strcmp(argv[i], "-s") ? &skip : !strcmp(argv[i], "-n") ? &limit : NULL;
        if (!what || i + 1 >= argc || !sh_parse_u64(argv[i + 1], what)) {
            sh_tty("usage: hexdump [-s offset] [-n bytes] [file]\n");
            return 2;
        }
        i++;
    }
    const char *d;
    size_t n;
    if (!sh_input("hexdump", argc, argv, i, &d, &n))
        return 1;
    if (skip > n)
        skip = n;
    uint64_t end = limit < n - skip ? skip + limit : n;
    for (uint64_t off = skip; off < end; off += 16) {
        if ((off & 0xfff) == 0 && sh_interrupted())
            return 130;
        hex_line(d, off, end);
    }
    sh_say("%08lx\n", (unsigned long)end);
    return 0;
}
