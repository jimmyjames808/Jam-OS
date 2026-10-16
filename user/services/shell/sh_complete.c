/* Tab completion: the first word of a command completes to a command,
 * alias or program in a directory of $PATH (/boot/bin, /boot/usr/bin, an
 * installed package's /pkg/bin); any other word (or one with a /) to
 * a path. One candidate is completed in full; several, as far as they
 * agree, and if that adds nothing they are listed. */
#include "sh_core.h"

#define MAX_CAND 512
#define DIR_MAX  256   /* entries read from one directory of $PATH */

struct cands {
    char     names[MAX_CAND][64];   /* the candidates */
    bool     dir[MAX_CAND];         /* names[i] is a directory (completes with '/') */
    unsigned n;                     /* how many */
};

static void cand_add(struct cands *c, const char *name, bool dir)
{
    for (unsigned i = 0; i < c->n; i++)
        if (!strcmp(c->names[i], name))
            return;
    if (c->n < MAX_CAND && strlen(name) < sizeof(c->names[0])) {
        memcpy(c->names[c->n], name, strlen(name) + 1);
        c->dir[c->n++] = dir;
    }
}

/* The programs starting with word in the directories of $PATH (the boot
 * image's, an installed package's). */
static void program_cands(struct cands *c, const char *word)
{
    size_t wl = strlen(word);
    const char *dirs = sh_getvar("PATH");
    struct sh_dirent *ents = calloc(DIR_MAX, sizeof(*ents));
    if (!dirs)
        dirs = SH_PATH_DEFAULT;
    while (ents && *dirs) {
        const char *end = strchr(dirs, ':');
        size_t l = end ? (size_t)(end - dirs) : strlen(dirs);
        char dir[SH_PATH_MAX], abs[SH_PATH_MAX];
        if (l && l < sizeof(dir)) {
            memcpy(dir, dirs, l);
            dir[l] = '\0';
            int n = sh_resolve(dir, abs, sizeof(abs)) ? sh_readdir(abs, ents, DIR_MAX) : -1;
            for (int i = 0; i < n; i++)
                if (!ents[i].dir && !strncmp(ents[i].name, word, wl))
                    cand_add(c, ents[i].name, false);
        }
        dirs += end ? l + 1 : l;
    }
    free(ents);
}

/* Commands, aliases and programs starting with word. */
static void command_cands(struct cands *c, const char *word)
{
    size_t wl = strlen(word);
    const struct sh_cmd *cmd;
    for (size_t i = 0; (cmd = sh_cmd_at(i)); i++)
        if (!strncmp(cmd->name, word, wl))
            cand_add(c, cmd->name, false);
    const char *name, *value;
    for (int i = 0; i < SH_MAX_ALIAS; i++)
        if (sh_alias_at(i, &name, &value) && !strncmp(name, word, wl))
            cand_add(c, name, false);
    program_cands(c, word);
}

/* A path: list the directory part, match the last part. Returns that last
 * part (the stem the candidates complete). */
static const char *path_cands(struct cands *c, const char *word)
{
    const char *slash = NULL, *stem = word;
    for (const char *p = word; *p; p++)
        if (*p == '/')
            slash = p;
    char dir[SH_PATH_MAX], abs[SH_PATH_MAX];
    if (slash) {
        size_t dl = (size_t)(slash - word) + 1;
        memcpy(dir, word, dl);
        dir[dl] = '\0';
        stem = slash + 1;
    } else {
        memcpy(dir, ".", 2);
    }
    if (sh_resolve(dir, abs, sizeof(abs))) {
        struct sh_dirent *ents = calloc(256, sizeof(*ents));
        int n = ents ? sh_readdir(abs, ents, 256) : -1;
        for (int i = 0; i < n; i++)
            if (!strncmp(ents[i].name, stem, strlen(stem)))
                cand_add(c, ents[i].name, ents[i].dir);
        free(ents);
    }
    return stem;
}

/* Nothing to add: show the choices (the line is redrawn after). */
static void list_cands(const struct cands *c)
{
    sh_put("\r\n", 2);
    unsigned col = 0;
    for (unsigned i = 0; i < c->n; i++) {
        sh_say("%s%s%s", c->names[i], c->dir[i] ? "/" : "", "  ");
        col += (unsigned)strlen(c->names[i]) + 2 + c->dir[i];
        if (col > 70) {
            sh_put("\r\n", 2);
            col = 0;
        }
    }
    if (col)
        sh_put("\r\n", 2);
}

/* Insert what the candidates agree on after stem (all of it, and a space
 * or / after a single one); else list them. true: redraw the line. */
static bool apply(const struct cands *c, size_t sl, char *line, unsigned *len, unsigned *pos,
                  unsigned cap)
{
    size_t common = strlen(c->names[0]);
    for (unsigned i = 1; i < c->n; i++) {
        size_t j = 0;
        while (j < common && c->names[i][j] == c->names[0][j])
            j++;
        common = j;
    }
    char add[80];
    size_t na = 0;
    if (common > sl) {
        na = common - sl;
        memcpy(add, c->names[0] + sl, na);
    }
    if (c->n == 1)
        add[na++] = c->dir[0] ? '/' : ' ';
    if (na && *len + na <= cap) {
        memmove(line + *pos + na, line + *pos, *len - *pos);
        memcpy(line + *pos, add, na);
        *len += (unsigned)na;
        *pos += (unsigned)na;
        return true;
    }
    if (c->n > 1) {
        list_cands(c);
        return true;
    }
    return false;
}

bool sh_complete(char *line, unsigned *len, unsigned *pos, unsigned cap)
{
    /* The word before the cursor, and whether it is the command's first. */
    unsigned ws = *pos;
    while (ws > 0 && line[ws - 1] != ' ')
        ws--;
    unsigned k = ws;
    while (k > 0 && line[k - 1] == ' ')
        k--;
    bool first = k == 0 || line[k - 1] == '|' || line[k - 1] == ';' || line[k - 1] == '&';
    char word[SH_PATH_MAX];
    unsigned wl = *pos - ws;
    if (wl >= sizeof(word))
        return false;
    memcpy(word, line + ws, wl);
    word[wl] = '\0';

    struct cands *c = calloc(1, sizeof(*c));
    if (!c)
        return false;
    const char *stem = word;
    if (first && !strchr(word, '/'))
        command_cands(c, word);
    else
        stem = path_cands(c, word);
    bool redraw = c->n && apply(c, strlen(stem), line, len, pos, cap);
    free(c);
    return redraw;
}
