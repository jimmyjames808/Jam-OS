/* Shell variables (exported ones are a program's environment) and
 * aliases, each a fixed table of slots. */
#include "sh_core.h"

struct var {
    char  name[SH_NAME_MAX];
    char *value;   /* NULL: free slot */
    bool  exported;
};
static struct var vars[SH_MAX_VARS];

struct alias {
    char  name[SH_NAME_MAX];
    char *value;   /* NULL: free slot */
};
static struct alias aliases[SH_MAX_ALIAS];

char *sh_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

bool sh_name_char(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (!first && c >= '0' && c <= '9');
}

size_t sh_assignment(const char *word)
{
    size_t n = 0;
    while (sh_name_char(word[n], n == 0))
        n++;
    return n && word[n] == '=' ? n : 0;
}

/* ---- variables ------------------------------------------------------------------------ */

static struct var *find_var(const char *name)
{
    for (int i = 0; i < SH_MAX_VARS; i++)
        if (vars[i].value && !strcmp(vars[i].name, name))
            return &vars[i];
    return NULL;
}

const char *sh_getvar(const char *name)
{
    struct var *v = find_var(name);
    return v ? v->value : NULL;
}

void sh_setvar(const char *name, const char *value, int exported)
{
    if (strlen(name) >= sizeof(vars[0].name))
        return;
    struct var *v = find_var(name);
    if (!v) {
        for (int i = 0; i < SH_MAX_VARS && !v; i++)
            if (!vars[i].value)
                v = &vars[i];
        if (!v) {
            sh_tty("set: too many variables (%d)\n", SH_MAX_VARS);
            return;
        }
        memcpy(v->name, name, strlen(name) + 1);
        v->exported = false;
    }
    char *d = sh_strdup(value);
    if (!d)
        return;
    free(v->value);
    v->value = d;
    if (exported >= 0)
        v->exported = exported;
}

void sh_unsetvar(const char *name)
{
    struct var *v = find_var(name);
    if (v) {
        free(v->value);
        v->value = NULL;
    }
}

bool sh_exportvar(const char *name)
{
    struct var *v = find_var(name);
    if (v)
        v->exported = true;
    return v != NULL;
}

bool sh_var_at(int i, const char **name, const char **value, bool *exported)
{
    if (i < 0 || i >= SH_MAX_VARS || !vars[i].value)
        return false;
    *name = vars[i].name;
    *value = vars[i].value;
    *exported = vars[i].exported;
    return true;
}

char **sh_make_env(void)
{
    char **env = calloc(SH_MAX_VARS + 1, sizeof(char *));
    int n = 0;
    for (int i = 0; env && i < SH_MAX_VARS; i++) {
        if (!vars[i].value || !vars[i].exported)
            continue;
        size_t l = strlen(vars[i].name) + strlen(vars[i].value) + 2;
        char *s = malloc(l);
        if (!s)
            continue;
        snprintf(s, l, "%s=%s", vars[i].name, vars[i].value);
        env[n++] = s;
    }
    return env;
}

void sh_free_env(char **env)
{
    for (int i = 0; env && env[i]; i++)
        free(env[i]);
    free(env);
}

/* ---- aliases -------------------------------------------------------------------------- */

static struct alias *find_alias(const char *name)
{
    for (int i = 0; i < SH_MAX_ALIAS; i++)
        if (aliases[i].value && !strcmp(aliases[i].name, name))
            return &aliases[i];
    return NULL;
}

const char *sh_alias_of(const char *name)
{
    struct alias *a = find_alias(name);
    return a ? a->value : NULL;
}

bool sh_set_alias(const char *name, const char *value)
{
    if (!*name || strlen(name) >= sizeof(aliases[0].name))
        return false;
    struct alias *a = find_alias(name);
    for (int i = 0; i < SH_MAX_ALIAS && !a; i++)
        if (!aliases[i].value)
            a = &aliases[i];
    if (!a)
        return false;
    char *d = sh_strdup(value);
    if (!d)
        return false;
    memcpy(a->name, name, strlen(name) + 1);
    free(a->value);
    a->value = d;
    return true;
}

bool sh_unalias(const char *name)
{
    struct alias *a = find_alias(name);
    if (!a)
        return false;
    free(a->value);
    a->value = NULL;
    return true;
}

bool sh_alias_at(int i, const char **name, const char **value)
{
    if (i < 0 || i >= SH_MAX_ALIAS || !aliases[i].value)
        return false;
    *name = aliases[i].name;
    *value = aliases[i].value;
    return true;
}
