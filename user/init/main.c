/* init: the first user process, started by the kernel's userboot with the
 * root capabilities (M5-PLAN.md, phase 2).
 *
 * For now it proves ring 3 works end to end: it prints its arguments and
 * the handles it was given, then reads init.cfg from bootfs and says what
 * it would start. Starting the programs (process_create + the libos ELF
 * loader) comes with phase 2. */
#include <os.h>

#define MAX_WORDS 16

/* Split one init.cfg line into words (in place). Returns how many. */
static int split(char *line, char **words)
{
    int n = 0;
    for (char *p = line; *p;) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (!*p)
            break;
        if (n == MAX_WORDS)
            return -1;
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    return n;
}

/* init.cfg: one program per line, "<path in bootfs> [args...]"; blank lines
 * and lines starting with '#' are ignored. */
static void run_config(const struct bootfs_view *fs, const char *cfg, uint64_t len)
{
    char *text = malloc(len + 1);
    if (!text) {
        printf("init: no memory for init.cfg\n");
        return;
    }
    memcpy(text, cfg, len);
    text[len] = '\0';

    int lineno = 0;
    for (char *line = text; line; ) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        lineno++;
        char *words[MAX_WORDS];
        int n = line[0] == '#' ? 0 : split(line, words);
        if (n < 0) {
            printf("init: init.cfg:%d: more than %d words\n", lineno, MAX_WORDS);
        } else if (n > 0) {
            const void *data;
            uint64_t size;
            status_t st = bootfs_lookup(fs, words[0], &data, &size);
            printf("init: would run %s", words[0]);
            for (int i = 1; i < n; i++)
                printf(" %s", words[i]);
            if (st == OK)
                printf(" (%lu bytes in bootfs)\n", (unsigned long)size);
            else
                printf(" (%s)\n", st == ERR_NOT_FOUND ? "not in bootfs" : status_str(st));
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
}

int main(int argc, char **argv)
{
    printf("init: hello from ring 3\n");
    for (int i = 0; i < argc; i++)
        printf("init: argv[%d] = \"%s\"\n", i, argv[i]);
    for (char **e = environ; *e; e++)
        printf("init: env %s\n", *e);
    for (unsigned i = 0; i < startup_handle_count(); i++) {
        uint32_t role;
        handle_t h = startup_handle_at(i, &role);
        printf("init: handle %u = %#x (%s)\n", i, h, startup_role_name(role));
    }

    handle_t bootfs_vmo = startup_handle(SR_BOOTFS);
    if (bootfs_vmo == HANDLE_INVALID) {
        printf("init: no bootfs handle, nothing to run\n");
        return 1;
    }
    struct bootfs_view fs;
    status_t st = bootfs_open(bootfs_vmo, &fs);
    if (st != OK) {
        printf("init: can't map bootfs (%s)\n", status_str(st));
        return 1;
    }
    const void *cfg;
    uint64_t len;
    st = bootfs_lookup(&fs, "init.cfg", &cfg, &len);
    if (st != OK) {
        printf("init: no init.cfg in bootfs (%s)\n", status_str(st));
        return 1;
    }
    run_config(&fs, cfg, len);
    return 0;
}
