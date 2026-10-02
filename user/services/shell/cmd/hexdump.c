/* hexdump (hd): hex and ASCII, 16 bytes a line, of a file or the pipe;
 * -s offset, -n bytes (-C, the format it always uses, is accepted). A file
 * is read a piece at a time from the offset, so its size doesn't matter. */
#include "sh.h"

#define CHUNK (64u << 10)   /* a multiple of 16: lines never span two reads */

static void hex_line(const char *d, uint64_t off, size_t n)
{
    char line[96];
    int k = snprintf(line, sizeof(line), "%08lx  ", (unsigned long)off);
    for (size_t j = 0; j < 16; j++) {
        if (j < n)
            k += snprintf(line + k, sizeof(line) - k, "%02x ", (uint8_t)d[j]);
        else
            k += snprintf(line + k, sizeof(line) - k, "   ");
        if (j == 7)
            line[k++] = ' ';
    }
    line[k++] = ' ';
    line[k++] = '|';
    for (size_t j = 0; j < 16 && j < n; j++) {
        char c = d[j];
        line[k++] = c >= 0x20 && c < 0x7f ? c : '.';
    }
    line[k++] = '|';
    line[k++] = '\n';
    sh_put(line, (size_t)k);
}

/* d[0..n) printed as the bytes at offset off; false if interrupted. */
static bool hex_lines(const char *d, uint64_t off, size_t n)
{
    for (size_t i = 0; i < n; i += 16) {
        if (((off + i) & 0xfff) == 0 && sh_interrupted())
            return false;
        hex_line(d + i, off + i, n - i < 16 ? n - i : 16);
    }
    return true;
}

/* The file at abs from skip, at most limit bytes; 0, 1 or 130. */
static int hex_file(const char *arg, const char *abs, uint64_t skip, uint64_t limit)
{
    struct jfile f;
    char *buf = malloc(CHUNK);
    if (!buf) {
        sh_tty("hexdump: out of memory\n");
        return 1;
    }
    status_t st = file_open(abs, FS_READ, &f);
    bool opened = st == OK;
    uint64_t off = skip, end = limit < UINT64_MAX - skip ? skip + limit : UINT64_MAX;
    int ret = 0;
    while (st == OK && off < end) {
        size_t want = end - off < CHUNK ? (size_t)(end - off) : CHUNK, got = 0;
        st = file_read(&f, off, buf, want, &got);
        if (st != OK || !got)
            break;
        if (!hex_lines(buf, off, got)) {
            ret = 130;
            break;
        }
        off += got;
    }
    if (opened)
        file_close(&f);
    free(buf);
    if (st != OK) {
        sh_tty("hexdump: %s: can't read it (%s)\n", arg, sh_why(st));
        return 1;
    }
    if (!ret)
        sh_say("%08lx\n", (unsigned long)off);
    return ret;
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
    if (i < argc) {
        char abs[SH_PATH_MAX];
        bool dir = false;
        uint64_t size = 0;
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? sh_stat(abs, &dir, &size)
                                                            : ERR_NOT_FOUND;
        if (st == ERR_NOT_FOUND) {
            sh_tty("hexdump: %s: no such file\n", argv[i]);
            return 1;
        }
        if (st == OK && dir) {
            sh_tty("hexdump: %s: is a directory\n", argv[i]);
            return 1;
        }
        if (st != OK) {
            sh_tty("hexdump: %s: can't read it (%s)\n", argv[i], sh_why(st));
            return 1;
        }
        return hex_file(argv[i], abs, skip < size ? skip : size, limit);
    }
    const char *d;
    size_t n;
    if (!sh_input("hexdump", argc, argv, i, &d, &n))
        return 1;
    if (skip > n)
        skip = n;
    uint64_t end = limit < n - skip ? skip + limit : n;
    if (!hex_lines(d + skip, skip, (size_t)(end - skip)))
        return 130;
    sh_say("%08lx\n", (unsigned long)end);
    return 0;
}
