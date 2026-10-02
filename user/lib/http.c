/* <http.h>: URLs, heads and Content-Types for `fetch` and `serve` (a
 * chunked body is httpchunk.c's). Every function reads only the bytes it
 * is given and writes only its outputs; a head is walked line by line with
 * the bounds checked before each byte is used, so no input can read or
 * write past a buffer. */
#include <http.h>

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

/* The n bytes at a are the literal lit, in any case. */
static bool ieq(const char *a, size_t n, const char *lit)
{
    size_t k = 0;
    for (; k < n && lit[k]; k++)
        if (lower(a[k]) != lit[k])
            return false;
    return k == n && !lit[k];
}

/* RFC 9110's tchar: what a token (a method, a header's name) is made of. */
static bool tchar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c && strchr("!#$%&'*+-.^_`|~", c));
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

/* n bytes of plain decimal digits as a number at most max. */
static bool decimal(const char *s, size_t n, uint64_t max, uint64_t *out)
{
    uint64_t v = 0;
    if (!n)
        return false;
    for (size_t k = 0; k < n; k++) {
        if (!digit(s[k]) || v > (max - (uint64_t)(s[k] - '0')) / 10)
            return false;
        v = v * 10 + (uint64_t)(s[k] - '0');
    }
    *out = v;
    return true;
}

/* Copy n bytes into out (cap bytes, NUL-terminated, cut), anything not
 * printable ASCII made '?': text from the network, safe to print. */
static void printable(const char *s, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    for (; k < n && k + 1 < cap; k++)
        out[k] = s[k] >= 0x20 && s[k] < 0x7f ? s[k] : '?';
    out[k] = 0;
}

/* ---- URLs ---------------------------------------------------------------------------- */

/* A URL's path bytes: printable ASCII, no space. */
static bool path_ok(const char *s, size_t n)
{
    for (size_t k = 0; k < n; k++)
        if (s[k] <= 0x20 || s[k] >= 0x7f)
            return false;
    return true;
}

/* host[:port] from s[0..n). */
static status_t authority(const char *s, size_t n, struct http_url *u)
{
    size_t h = 0;
    while (h < n && s[h] != ':')
        h++;
    if (!h || h > HTTP_HOST_MAX)
        return ERR_INVALID_ARGS;
    for (size_t k = 0; k < h; k++) {
        char c = lower(s[k]);
        if (!((c >= 'a' && c <= 'z') || digit(c) || c == '.' || c == '-'))
            return ERR_INVALID_ARGS;   /* no user@, no [IPv6], nothing else */
        u->host[k] = c;
    }
    u->host[h] = 0;
    u->port = 80;
    if (h == n)
        return OK;
    uint64_t port;
    if (!decimal(s + h + 1, n - h - 1, 65535, &port) || !port)
        return ERR_INVALID_ARGS;
    u->port = (uint16_t)port;
    return OK;
}

/* The path and query at s (up to a '#' or the end) into u->path. */
static status_t path_part(const char *s, struct http_url *u)
{
    size_t n = 0;
    while (s[n] && s[n] != '#')
        n++;
    size_t lead = s[0] == '/' ? 0 : 1;   /* "?q" or "" gets its '/' */
    if (n + lead >= HTTP_PATH_MAX || !path_ok(s, n))
        return ERR_INVALID_ARGS;
    u->path[0] = '/';
    memcpy(u->path + lead, s, n);
    u->path[n + lead] = 0;
    return OK;
}

status_t http_url_parse(const char *s, struct http_url *out)
{
    struct http_url u;
    size_t len = strnlen(s, HTTP_URL_MAX);
    if (len == HTTP_URL_MAX)
        return ERR_INVALID_ARGS;
    const char *sep = strstr(s, "://");
    const char *slash = strchr(s, '/');
    if (sep && (!slash || sep < slash)) {
        if (!ieq(s, (size_t)(sep - s), "http"))
            return ERR_NOT_SUPPORTED;   /* https (no TLS here) or another scheme */
        s = sep + 3;
    }
    size_t n = 0;
    while (s[n] && s[n] != '/' && s[n] != '?' && s[n] != '#')
        n++;
    status_t st = authority(s, n, &u);
    if (st == OK)
        st = path_part(s + n, &u);
    if (st == OK)
        *out = u;
    return st;
}

status_t http_url_resolve(const struct http_url *base, const char *loc, struct http_url *out)
{
    const char *sep = strstr(loc, "://");
    const char *slash = strchr(loc, '/');
    if (sep && (!slash || sep < slash))
        return http_url_parse(loc, out);
    if (loc[0] == '/' && loc[1] == '/')
        return http_url_parse(loc + 2, out);
    struct http_url u = *base;
    if (loc[0] == '/' || loc[0] == '?') {
        if (path_part(loc, &u) != OK)
            return ERR_INVALID_ARGS;
        *out = u;
        return OK;
    }
    /* Relative: after the last '/' of base's path (before its query). */
    size_t dir = 0;
    for (size_t k = 0; base->path[k] && base->path[k] != '?'; k++)
        if (base->path[k] == '/')
            dir = k + 1;
    char joined[HTTP_PATH_MAX];
    size_t n = strnlen(loc, HTTP_PATH_MAX);
    if (dir + n >= HTTP_PATH_MAX)
        return ERR_INVALID_ARGS;
    memcpy(joined, base->path, dir);
    memcpy(joined + dir, loc, n);
    joined[dir + n] = 0;
    if (path_part(joined, &u) != OK)
        return ERR_INVALID_ARGS;
    *out = u;
    return OK;
}

void http_url_filename(const struct http_url *u, char *out, size_t cap)
{
    size_t end = 0, start = 0;
    while (u->path[end] && u->path[end] != '?')
        end++;
    for (size_t k = 0; k < end; k++)
        if (u->path[k] == '/')
            start = k + 1;
    size_t k = 0;
    bool dots = true;
    for (; start + k < end && k + 1 < cap; k++) {
        char c = u->path[start + k];
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) ||
                    c == '.' || c == '_' || c == '+' || c == '-';
        out[k] = keep ? c : '_';
        dots = dots && c == '.';
    }
    out[k] = 0;
    if (dots)   /* "", ".", "..": no name of its own */
        snprintf(out, cap, "index.html");
}

void http_url_hostport(const struct http_url *u, char *out, size_t cap)
{
    if (u->port == 80)
        snprintf(out, cap, "%s", u->host);
    else
        snprintf(out, cap, "%s:%u", u->host, u->port);
}

/* ---- heads ------------------------------------------------------------------------------ */

status_t http_head_end(const uint8_t *buf, size_t n, size_t *len)
{
    size_t lim = n < HTTP_HEAD_MAX ? n : HTTP_HEAD_MAX;
    for (size_t k = 0; k < lim; k++) {
        if (buf[k] != '\n')
            continue;
        /* a blank line: "\n\n" or "\n\r\n" */
        if (k + 1 < lim && buf[k + 1] == '\n') {
            *len = k + 2;
            return OK;
        }
        if (k + 2 < lim && buf[k + 1] == '\r' && buf[k + 2] == '\n') {
            *len = k + 3;
            return OK;
        }
    }
    return n >= HTTP_HEAD_MAX ? ERR_OUT_OF_RANGE : ERR_SHOULD_WAIT;
}

/* A walk over a head's lines. */
struct lines {
    const char *s;
    size_t      n, at;
    unsigned    count;   /* lines taken */
};

/* The next line (its CRLF or LF cut) into *line, *len: false at the end
 * (or past the blank line). A bare CR inside a line, a control character
 * or too many lines: *bad. */
static bool next_line(struct lines *w, const char **line, size_t *len, bool *bad)
{
    if (w->at >= w->n)
        return false;
    size_t k = w->at;
    while (k < w->n && w->s[k] != '\n')
        k++;
    size_t end = k;
    if (end > w->at && w->s[end - 1] == '\r')
        end--;
    for (size_t j = w->at; j < end; j++)
        if ((uint8_t)w->s[j] < 0x20 && w->s[j] != '\t')
            *bad = true;   /* a CR alone, a NUL, ... */
    if (++w->count > HTTP_HEADERS_MAX + 2)
        *bad = true;
    *line = w->s + w->at;
    *len = end - w->at;
    w->at = k < w->n ? k + 1 : k;
    return *len > 0;   /* the blank line ends the head */
}

/* "Name: value": the name (a token) and the value without its leading and
 * trailing spaces. false: not a header line (a folded one included). */
static bool header(const char *l, size_t n, const char **name, size_t *nlen, const char **val,
                   size_t *vlen)
{
    size_t c = 0;
    while (c < n && l[c] != ':')
        c++;
    if (!c || c == n)
        return false;
    for (size_t k = 0; k < c; k++)
        if (!tchar(l[k]))
            return false;   /* a space before ':' or a folded line */
    size_t a = c + 1, b = n;
    while (a < b && (l[a] == ' ' || l[a] == '\t'))
        a++;
    while (b > a && (l[b - 1] == ' ' || l[b - 1] == '\t'))
        b--;
    *name = l;
    *nlen = c;
    *val = l + a;
    *vlen = b - a;
    return true;
}

/* A Content-Length value, which must agree with an earlier one (-1: none). */
static bool content_length(const char *v, size_t n, int64_t *len)
{
    uint64_t x;
    if (!decimal(v, n, HTTP_LENGTH_MAX, &x) || (*len >= 0 && (uint64_t)*len != x))
        return false;
    *len = (int64_t)x;
    return true;
}

/* A Connection value has `close` (or `keep-alive`) among its tokens. */
static bool has_token(const char *v, size_t n, const char *tok)
{
    size_t a = 0;
    while (a < n) {
        size_t b = a;
        while (b < n && v[b] != ',')
            b++;
        size_t x = a, y = b;
        while (x < y && (v[x] == ' ' || v[x] == '\t'))
            x++;
        while (y > x && (v[y - 1] == ' ' || v[y - 1] == '\t'))
            y--;
        if (ieq(v + x, y - x, tok))
            return true;
        a = b + 1;
    }
    return false;
}

/* "HTTP/1.x" at s (8 bytes): its minor version, or -1. */
static int version(const char *s, size_t n)
{
    if (n != 8 || memcmp(s, "HTTP/1.", 7) || (s[7] != '0' && s[7] != '1'))
        return -1;
    return s[7] - '0';
}

static bool status_line(const char *l, size_t n, struct http_response *r)
{
    int minor = n >= 12 ? version(l, 8) : -1;
    uint64_t code;
    if (minor < 0 || l[8] != ' ' || !decimal(l + 9, 3, 999, &code) || code < 100 ||
        code > 599 || (n > 12 && l[12] != ' '))
        return false;
    r->minor = (unsigned)minor;
    r->status = (unsigned)code;
    r->close = minor == 0;
    if (n > 13)
        printable(l + 13, n - 13, r->reason, sizeof(r->reason));
    return true;
}

/* One header of a response into r: false if it breaks the rules above;
 * *unsupported for a coding other than chunked. */
static bool response_header(const char *name, size_t nl, const char *v, size_t vl,
                            struct http_response *r, bool *unsupported)
{
    if (ieq(name, nl, "content-length"))
        return content_length(v, vl, &r->length);
    if (ieq(name, nl, "transfer-encoding")) {
        if (!ieq(v, vl, "chunked") || r->chunked)
            *unsupported = true;   /* gzip, "chunked, chunked", ...: not taken */
        r->chunked = true;
        return true;
    }
    if (ieq(name, nl, "connection")) {
        if (has_token(v, vl, "close"))
            r->close = true;
        else if (has_token(v, vl, "keep-alive") && r->minor == 0)
            r->close = false;
        return true;
    }
    if (ieq(name, nl, "location")) {
        if (r->location[0] || vl >= sizeof(r->location) || !path_ok(v, vl))
            return false;
        memcpy(r->location, v, vl);
        r->location[vl] = 0;
    }
    return true;
}

status_t http_response_parse(const uint8_t *head, size_t len, struct http_response *out)
{
    struct http_response r;
    memset(&r, 0, sizeof(r));
    r.length = -1;
    struct lines w = { (const char *)head, len, 0, 0 };
    const char *l, *name, *v;
    size_t n, nl, vl;
    bool bad = false, unsupported = false;
    if (!next_line(&w, &l, &n, &bad) || bad || !status_line(l, n, &r))
        return ERR_INVALID_ARGS;
    while (next_line(&w, &l, &n, &bad)) {
        if (bad || !header(l, n, &name, &nl, &v, &vl) ||
            !response_header(name, nl, v, vl, &r, &unsupported))
            return ERR_INVALID_ARGS;
    }
    if (bad)
        return ERR_INVALID_ARGS;
    if (unsupported)
        return ERR_NOT_SUPPORTED;
    if (r.chunked)
        r.length = -1;   /* RFC 9112 6.3: Transfer-Encoding wins */
    *out = r;
    return OK;
}

bool http_response_has_body(const struct http_response *r, bool head_request)
{
    return !head_request && r->status >= 200 && r->status != 204 && r->status != 304;
}

/* ---- a request ------------------------------------------------------------------------- */

static bool has_byte(const char *s, size_t n, char c)
{
    for (size_t k = 0; k < n; k++)
        if (s[k] == c)
            return true;
    return false;
}

/* "bytes=a-b", "bytes=a-", "bytes=-n" (one range) into r; anything else
 * leaves r without a range. */
static void range(const char *v, size_t n, struct http_request *r)
{
    if (n < 7 || !ieq(v, 6, "bytes=") || has_byte(v, n, ','))
        return;
    v += 6;
    n -= 6;
    size_t d = 0;
    while (d < n && v[d] != '-')
        d++;
    if (d == n)
        return;
    uint64_t a = 0, b = UINT64_MAX;
    bool has_a = d > 0, has_b = d + 1 < n;
    if ((has_a && !decimal(v, d, HTTP_LENGTH_MAX, &a)) ||
        (has_b && !decimal(v + d + 1, n - d - 1, HTTP_LENGTH_MAX, &b)) || (!has_a && !has_b) ||
        (has_a && has_b && b < a))
        return;
    r->range = true;
    r->suffix = !has_a;
    r->first = a;
    r->last = b;
}

/* "METHOD target HTTP/1.x" into r: 0, or the status to answer. */
static unsigned request_line(const char *l, size_t n, struct http_request *r)
{
    size_t m = 0;
    while (m < n && tchar(l[m]))
        m++;
    if (!m || m == n || l[m] != ' ')
        return 400;
    size_t t = m + 1, e = t;
    while (e < n && l[e] != ' ')
        e++;
    if (e == t || e == n || !path_ok(l + t, e - t))
        return 400;
    printable(l + t, e - t, r->target, sizeof(r->target));
    const char *v = l + e + 1;
    int minor = version(v, n - e - 1);
    if (minor < 0)   /* "HTTP/d.d" of another version, or not a version at all */
        return n - e - 1 == 8 && !memcmp(v, "HTTP/", 5) && digit(v[5]) && v[6] == '.' &&
                       digit(v[7]) ? 505 : 400;
    r->minor = (unsigned)minor;
    r->close = minor == 0;
    /* Methods are case-sensitive (RFC 9110 9.1). */
    if (m == 3 && !memcmp(l, "GET", 3))
        r->method = HTTP_GET;
    else if (m == 4 && !memcmp(l, "HEAD", 4))
        r->method = HTTP_HEAD;
    else
        return 501;
    return 0;
}

/* One header of a request: 0, or the status to answer. *host: a Host came. */
static unsigned request_header(const char *name, size_t nl, const char *v, size_t vl,
                               struct http_request *r, bool *host)
{
    if (ieq(name, nl, "content-length")) {
        int64_t len = -1;
        return content_length(v, vl, &len) && len == 0 ? 0 : 400;   /* no bodies here */
    }
    if (ieq(name, nl, "transfer-encoding"))
        return 400;
    if (ieq(name, nl, "host")) {
        if (*host)
            return 400;   /* RFC 9112 3.2: exactly one */
        *host = true;
    } else if (ieq(name, nl, "connection")) {
        if (has_token(v, vl, "close"))
            r->close = true;
        else if (has_token(v, vl, "keep-alive") && r->minor == 0)
            r->close = false;
    } else if (ieq(name, nl, "range") && !r->range) {
        range(v, vl, r);
    }
    return 0;
}

unsigned http_request_parse(const uint8_t *head, size_t len, struct http_request *out)
{
    struct http_request r;
    memset(&r, 0, sizeof(r));
    struct lines w = { (const char *)head, len, 0, 0 };
    const char *l, *name, *v;
    size_t n, nl, vl;
    bool bad = false, host = false;
    unsigned answer = 400;
    if (next_line(&w, &l, &n, &bad) && !bad)
        answer = request_line(l, n, &r);
    *out = r;
    if (answer)
        return answer;
    while (next_line(&w, &l, &n, &bad)) {
        if (bad || !header(l, n, &name, &nl, &v, &vl))
            return 400;
        if ((answer = request_header(name, nl, v, vl, &r, &host)) != 0)
            return answer;
    }
    if (bad || (r.minor == 1 && !host))
        return 400;
    *out = r;
    return 0;
}

status_t http_request_span(const struct http_request *r, uint64_t size, uint64_t *from,
                           uint64_t *n)
{
    if (!r->range) {
        *from = 0;
        *n = size;
        return OK;
    }
    if (r->suffix) {
        if (!r->last || !size)
            return ERR_OUT_OF_RANGE;
        *n = r->last < size ? r->last : size;
        *from = size - *n;
        return OK;
    }
    if (r->first >= size)
        return ERR_OUT_OF_RANGE;
    uint64_t last = r->last < size - 1 ? r->last : size - 1;
    *from = r->first;
    *n = last - r->first + 1;
    return OK;
}

const char *http_reason(unsigned status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/* Extensions and their types, in lower case. */
static const struct {
    const char *ext, *type;
} types[] = {
    { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" },
    { "txt", "text/plain; charset=utf-8" }, { "md", "text/plain; charset=utf-8" },
    { "log", "text/plain; charset=utf-8" }, { "c", "text/plain; charset=utf-8" },
    { "h", "text/plain; charset=utf-8" },   { "css", "text/css" },
    { "js", "text/javascript" },            { "json", "application/json" },
    { "xml", "application/xml" },           { "csv", "text/csv" },
    { "png", "image/png" },                 { "jpg", "image/jpeg" },
    { "jpeg", "image/jpeg" },               { "gif", "image/gif" },
    { "svg", "image/svg+xml" },             { "ico", "image/x-icon" },
    { "webp", "image/webp" },               { "bmp", "image/bmp" },
    { "mp3", "audio/mpeg" },                { "wav", "audio/wav" },
    { "mp4", "video/mp4" },                 { "mpg", "video/mpeg" },
    { "pdf", "application/pdf" },           { "zip", "application/zip" },
    { "gz", "application/gzip" },           { "tar", "application/x-tar" },
};

const char *http_content_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    const char *slash = strrchr(name, '/');
    if (!dot || (slash && dot < slash))
        return "application/octet-stream";
    size_t n = strlen(dot + 1);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (ieq(dot + 1, n, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}
