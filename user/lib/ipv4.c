/* IPv4 addresses as text (<ipv4.h>): a strict parser (no leading '+',
 * no more than 3 digits, nothing above 255, exactly four parts) and the
 * settings' static address. */
#include <ipv4.h>
#include <os.h>

/* A decimal number of 1..3 digits at most `max`, at *s; *s moves past it. */
static bool number(const char **s, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    unsigned n = 0;
    while (n < 3 && (*s)[n] >= '0' && (*s)[n] <= '9')
        v = v * 10 + (uint32_t)((*s)[n++] - '0');
    if (!n || v > max || ((*s)[n] >= '0' && (*s)[n] <= '9'))
        return false;
    *s += n;
    *out = v;
    return true;
}

bool ipv4_parse(const char *s, uint32_t *out, const char **end)
{
    uint32_t a = 0;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t part;
        if (i && *s++ != '.')
            return false;
        if (!number(&s, 255, &part))
            return false;
        a = a << 8 | part;
    }
    *out = a;
    if (end)
        *end = s;
    return true;
}

const char *ipv4_format(uint32_t a, char buf[IPV4_TEXT_MAX])
{
    snprintf(buf, IPV4_TEXT_MAX, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff,
             a & 0xff);
    return buf;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

bool ipv4_config_parse(const char *s, struct ipv4_config *out)
{
    struct ipv4_config c = { 0 };
    uint32_t prefix;
    s = skip_spaces(s);
    if (!ipv4_parse(s, &c.address, &s) || *s++ != '/' || !number(&s, 32, &prefix) || !prefix)
        return false;
    c.mask = prefix == 32 ? 0xffffffffu : ~(0xffffffffu >> prefix);
    uint32_t *rest[] = { &c.gateway, &c.dns[0], &c.dns[1] };
    for (unsigned i = 0; i < 3; i++) {
        if (*s && *s != ' ' && *s != '\t')
            return false;   /* something stuck to the last one */
        s = skip_spaces(s);
        if (!*s)
            break;
        if (!ipv4_parse(s, rest[i], &s))
            return false;
    }
    if (*skip_spaces(s))
        return false;   /* more than four parts, or junk */
    *out = c;
    return true;
}
