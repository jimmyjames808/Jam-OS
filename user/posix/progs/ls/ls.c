/* A tiny ls for posixtest's process section (`popen("ls /boot")`), in
 * bootfs at test/ls until sbase's ls is ported: each directory named (or
 * "."), one name per line in the order the file system lists them, read
 * with libjam's fs_readdir. Exit 0, or 1 if a directory couldn't be
 * listed. */
#include <stdio.h>
#include <jamos/libjam.h>
#include <string.h>

static int list(const char *dir)
{
    static struct fs_entry e;
    status_t st;
    uint32_t i = 0;
    for (; (st = fs_readdir(dir, i, &e)) == OK; i++)
        printf("%s\n", e.name);
    if (st == ERR_NOT_FOUND && i > 0)
        return 0;   /* past the last entry */
    if (st == ERR_NOT_FOUND && fs_stat(dir, NULL, NULL, NULL) == OK)
        return 0;   /* an empty directory */
    fprintf(stderr, "ls: %s: %s\n", dir, status_str(st));
    return 1;
}

int main(int argc, char **argv)
{
    int r = 0;
    if (argc < 2)
        return list(".");
    for (int i = 1; i < argc; i++)
        r |= list(argv[i]);
    return r;
}
