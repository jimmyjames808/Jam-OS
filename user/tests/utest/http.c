/* utest: <http.h> (user/lib/http.c), the HTTP of `fetch` and `serve`:
 * URLs (https refused, a Location resolved, a safe file name), a
 * response's head (every rule of the strict parser broken on its own),
 * chunked bodies fed whole and a byte at a time (the same bytes out),
 * a request's head and its ranges, and random and mutated heads that must
 * never be taken for more than they are. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <http.h>
#include <os.h>
#include "utest.h"

static uint32_t rng(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static status_t resp(const char *text, struct http_response *r)
{
    return http_response_parse((const uint8_t *)text, strlen(text), r);
}

static unsigned req(const char *text, struct http_request *r)
{
    return http_request_parse((const uint8_t *)text, strlen(text), r);
}

bool t_http_url(void)
{
    struct http_url u, v;
    CHECK_ST(http_url_parse("http://Example.COM:8080/a/b.bin?x=1#frag", &u), OK);
    CHECK(!strcmp(u.host, "example.com") && u.port == 8080 && !strcmp(u.path, "/a/b.bin?x=1"));
    CHECK_ST(http_url_parse("10.2.21.174:8000/big.bin", &u), OK);   /* no scheme: http */
    CHECK(!strcmp(u.host, "10.2.21.174") && u.port == 8000 && !strcmp(u.path, "/big.bin"));
    CHECK_ST(http_url_parse("HTTP://h", &u), OK);
    CHECK(u.port == 80 && !strcmp(u.path, "/"));
    CHECK_ST(http_url_parse("http://h?q", &u), OK);
    CHECK(!strcmp(u.path, "/?q"));
    CHECK_ST(http_url_parse("https://h/x", &u), ERR_NOT_SUPPORTED);
    CHECK_ST(http_url_parse("ftp://h/x", &u), ERR_NOT_SUPPORTED);
    const char *bad[] = { "http://", "http://user@h/", "http://h:0/", "http://h:65536/",
                          "http://h:/", "http://h:8x/", "http://[::1]/", "http://h/a b",
                          "http://h/\x01", "http://h_/x", "", ":80" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (http_url_parse(bad[i], &u) != ERR_INVALID_ARGS)
            FAIL("\"%s\" taken", bad[i]);
    char longurl[HTTP_URL_MAX + 8];
    memset(longurl, 'a', sizeof(longurl) - 1);
    longurl[sizeof(longurl) - 1] = 0;
    memcpy(longurl, "http://h/", 9);
    CHECK_ST(http_url_parse(longurl, &u), ERR_INVALID_ARGS);
    /* names to save as */
    char name[32];
    const char *names[][2] = { { "http://h/a/big.bin?x=1", "big.bin" }, { "http://h/", "index.html" },
                               { "http://h/a/..", "index.html" }, { "http://h/%2e%2e", "_2e_2e" },
                               { "http://h/a%2Fb", "a_2Fb" }, { "http://h/x/", "index.html" } };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        CHECK_ST(http_url_parse(names[i][0], &u), OK);
        http_url_filename(&u, name, sizeof(name));
        if (strcmp(name, names[i][1]))
            FAIL("%s saves as %s, want %s", names[i][0], name, names[i][1]);
    }
    CHECK_ST(http_url_parse("http://h/" "0123456789012345678901234567890123456789", &u), OK);
    http_url_filename(&u, name, 8);
    CHECK(strlen(name) == 7);
    /* Locations */
    CHECK_ST(http_url_parse("http://h:81/d/e/f?q", &u), OK);
    CHECK_ST(http_url_resolve(&u, "g", &v), OK);
    CHECK(!strcmp(v.host, "h") && v.port == 81 && !strcmp(v.path, "/d/e/g"));
    CHECK_ST(http_url_resolve(&u, "/top", &v), OK);
    CHECK(!strcmp(v.path, "/top") && v.port == 81);
    CHECK_ST(http_url_resolve(&u, "//other/x", &v), OK);
    CHECK(!strcmp(v.host, "other") && v.port == 80);
    CHECK_ST(http_url_resolve(&u, "http://o2:9/y", &v), OK);
    CHECK(!strcmp(v.host, "o2") && v.port == 9 && !strcmp(v.path, "/y"));
    CHECK_ST(http_url_resolve(&u, "https://o2/y", &v), ERR_NOT_SUPPORTED);
    CHECK_ST(http_url_resolve(&u, "/a b", &v), ERR_INVALID_ARGS);
    http_url_hostport(&u, name, sizeof(name));
    CHECK(!strcmp(name, "h:81"));
    return true;
}

bool t_http_response(void)
{
    struct http_response r;
    size_t len;
    const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nServer: x\r\n\r\nbody";
    CHECK_ST(http_head_end((const uint8_t *)ok, strlen(ok), &len), OK);
    CHECK_EQ(len, strlen(ok) - 4);
    CHECK_ST(http_head_end((const uint8_t *)ok, 20, &len), ERR_SHOULD_WAIT);
    CHECK_ST(resp(ok, &r), OK);   /* bytes after the head are not looked at */
    CHECK(r.status == 200 && r.length == 1234 && !r.chunked && !r.close && r.minor == 1);
    CHECK(!strcmp(r.reason, "OK") && http_response_has_body(&r, false) &&
          !http_response_has_body(&r, true));
    CHECK_ST(resp("HTTP/1.0 302 Found\nLocation: /x\n\n", &r), OK);   /* bare LFs */
    CHECK(r.status == 302 && r.close && !strcmp(r.location, "/x") && r.length == -1);
    CHECK_ST(resp("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\nContent-Length: 5\r\n"
                  "Connection: keep-alive, Close\r\n\r\n", &r), OK);
    CHECK(r.chunked && r.length == -1 && r.close);
    CHECK_ST(resp("HTTP/1.1 204\r\n\r\n", &r), OK);
    CHECK(r.status == 204 && !http_response_has_body(&r, false));
    CHECK_ST(resp("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-length: 5\r\n\r\n", &r), OK);
    CHECK_ST(resp("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", &r),
             ERR_NOT_SUPPORTED);
    CHECK_ST(resp("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n"
                  "\r\n", &r), ERR_NOT_SUPPORTED);
    const char *bad[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1125899906842625\r\n\r\n",   /* over 2^50 */
        "HTTP/1.1 200 OK\r\nX: a\r\n folded\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX : a\r\n\r\n",
        "HTTP/1.1 200 OK\r\nno colon\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: empty name\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: a\rb\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: a\0b\r\n\r\n",
        "HTTP/2.0 200 OK\r\n\r\n", "HTTP/1.1 99 X\r\n\r\n", "HTTP/1.1 600 X\r\n\r\n",
        "HTTP/1.1 2000 X\r\n\r\n", "HTTP/1.1  200 OK\r\n\r\n", "ICY 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nLocation: /a\r\nLocation: /b\r\n\r\n",
        "HTTP/1.1 301 Moved\r\nLocation: /a b\r\n\r\n",
        "\r\nHTTP/1.1 200 OK\r\n\r\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        /* the \0 one: its length by hand */
        size_t n = i == 10 ? 30 : strlen(bad[i]);
        if (http_response_parse((const uint8_t *)bad[i], n, &r) != ERR_INVALID_ARGS)
            FAIL("bad head %u taken", i);
    }
    /* too many headers, and a head with no end */
    static char big[HTTP_HEAD_MAX + 64];
    size_t n = (size_t)snprintf(big, sizeof(big), "HTTP/1.1 200 OK\r\n");
    for (unsigned i = 0; i < HTTP_HEADERS_MAX + 1; i++)
        n += (size_t)snprintf(big + n, sizeof(big) - n, "X-%u: y\r\n", i);
    n += (size_t)snprintf(big + n, sizeof(big) - n, "\r\n");
    CHECK_ST(http_response_parse((const uint8_t *)big, n, &r), ERR_INVALID_ARGS);
    memset(big, 'a', sizeof(big));
    CHECK_ST(http_head_end((const uint8_t *)big, HTTP_HEAD_MAX - 1, &len), ERR_SHOULD_WAIT);
    CHECK_ST(http_head_end((const uint8_t *)big, sizeof(big), &len), ERR_OUT_OF_RANGE);
    return true;
}

/* Decode body (a chunked one) whole and in pieces of `step` bytes: the
 * bytes out into out (cap), their count; -1 on an error, -2 not done. */
static long dechunk(const char *body, size_t n, size_t step, char *out, size_t cap)
{
    struct http_chunks c;
    http_chunks_init(&c);
    size_t got = 0, at = 0;
    while (at < n && !c.done) {   /* each turn takes at least a byte, or ends */
        size_t piece = n - at < step ? n - at : step, used, data, dn;
        if (http_chunks_step(&c, (const uint8_t *)body + at, piece, &used, &data, &dn) != OK)
            return -1;
        if (dn > cap - got)
            return -1;
        memcpy(out + got, body + at + data, dn);
        got += dn;
        at += used;
        if (!used)
            return -1;
    }
    return c.done ? (long)got : -2;
}

bool t_http_chunks(void)
{
    const char *body = "5\r\nhello\r\n1;ext=1\r\n \r\nA\r\n0123456789\r\n0\r\nTrailer: x\r\n\r\n";
    char out[64];
    for (size_t step = 1; step <= strlen(body); step++) {
        long n = dechunk(body, strlen(body), step, out, sizeof(out));
        if (n != 16 || memcmp(out, "hello 0123456789", 16))
            FAIL("step %u: %ld bytes", (unsigned)step, n);
    }
    CHECK_EQ(dechunk("0\n\n", 3, 1, out, sizeof(out)), 0);   /* bare LFs */
    CHECK_EQ(dechunk("3\r\nabc\r\n", 8, 4, out, sizeof(out)), -2);   /* not ended yet */
    const char *bad[] = { "g\r\n", "\r\n", "5\r\nhelloX\r\n", "5\r\nhello\rX",
                          "fffffffffffffffff\r\n", "4000000000001\r\n", "5 x\r\n",
                          "5;a\x01\r\n", "0\r\nX: a\rb\r\n\r\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (dechunk(bad[i], strlen(bad[i]), 3, out, sizeof(out)) != -1)
            FAIL("bad chunked body %u taken", i);
    /* a size line, and trailers, that never end */
    static char line[HTTP_HEAD_MAX + 64];
    memset(line, ';', sizeof(line));
    line[0] = '1';
    CHECK_EQ(dechunk(line, HTTP_CHUNK_LINE + 2, 7, out, sizeof(out)), -1);
    memcpy(line, "0\r\n", 3);
    memset(line + 3, 'x', sizeof(line) - 3);
    CHECK_EQ(dechunk(line, sizeof(line), 1000, out, sizeof(out)), -1);
    return true;
}

bool t_http_request(void)
{
    struct http_request r;
    uint64_t from, n;
    CHECK_EQ(req("GET /x HTTP/1.1\r\nHost: h\r\nUser-Agent: curl\r\n\r\n", &r), 0);
    CHECK(r.method == HTTP_GET && r.minor == 1 && !r.close && !r.range && !strcmp(r.target, "/x"));
    CHECK_ST(http_request_span(&r, 100, &from, &n), OK);
    CHECK(from == 0 && n == 100);
    CHECK_EQ(req("HEAD / HTTP/1.0\r\n\r\n", &r), 0);
    CHECK(r.method == HTTP_HEAD && r.close);
    CHECK_EQ(req("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: 0\r\n\r\n", &r),
             0);
    CHECK(r.close);
    CHECK_EQ(req("POST /up HTTP/1.1\r\nHost: h\r\n\r\n", &r), 501);
    CHECK(!strcmp(r.target, "/up"));
    CHECK_EQ(req("get / HTTP/1.1\r\nHost: h\r\n\r\n", &r), 501);
    CHECK_EQ(req("GET / HTTP/2.0\r\n\r\n", &r), 505);
    const char *bad[] = { "GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n",
                          "GET  / HTTP/1.1\r\nHost: h\r\n\r\n", "GET /\x7f HTTP/1.1\r\n\r\n",
                          "GET / HTTP/1.1 x\r\n\r\n", "GET /\r\n\r\n", "\r\n\r\n",
                          "GET / HTTP/1.1\r\nHost: h\r\n bad\r\n\r\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (req(bad[i], &r) != 400)
            FAIL("bad request %u not answered 400", i);
    /* ranges */
    struct { const char *h; status_t st; uint64_t from, n; } ranges[] = {
        { "bytes=0-9", OK, 0, 10 },        { "bytes=90-", OK, 90, 10 },
        { "bytes=-10", OK, 90, 10 },       { "bytes=-500", OK, 0, 100 },
        { "bytes=95-200", OK, 95, 5 },     { "bytes=100-", ERR_OUT_OF_RANGE, 0, 0 },
        { "bytes=-0", ERR_OUT_OF_RANGE, 0, 0 },
        { "bytes=0-1,5-6", OK, 0, 100 },   /* not one range: the whole file */
        { "bytes=9-1", OK, 0, 100 },       { "items=0-1", OK, 0, 100 },
        { "bytes=x-1", OK, 0, 100 },       { "bytes=-", OK, 0, 100 },
    };
    for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        char text[128];
        snprintf(text, sizeof(text), "GET / HTTP/1.1\r\nHost: h\r\nRange: %s\r\n\r\n", ranges[i].h);
        CHECK_EQ(req(text, &r), 0);
        status_t st = http_request_span(&r, 100, &from, &n);
        if (st != ranges[i].st || (st == OK && (from != ranges[i].from || n != ranges[i].n)))
            FAIL("Range: %s gives %s %lu+%lu", ranges[i].h, status_str(st), (unsigned long)from,
                 (unsigned long)n);
    }
    CHECK(!strcmp(http_content_type("/data/a.HTML"), "text/html; charset=utf-8"));
    CHECK(!strcmp(http_content_type("big.bin"), "application/octet-stream"));
    CHECK(!strcmp(http_content_type("a.dir/noext"), "application/octet-stream"));
    CHECK(!strcmp(http_content_type("song.mp3"), "audio/mpeg"));
    return true;
}

/* A good head with byte k changed to v, then parsed both ways. */
static bool mutated(const char *good, unsigned k, uint8_t v)
{
    static uint8_t b[256];
    size_t n = strlen(good);
    memcpy(b, good, n);
    b[k % n] = v;
    struct http_response r;
    struct http_request q;
    size_t len;
    if (http_head_end(b, n, &len) == OK && http_response_parse(b, len, &r) == OK) {
        if (r.status < 100 || r.status > 599 || r.length > (int64_t)HTTP_LENGTH_MAX ||
            (r.chunked && r.length != -1) || strnlen(r.location, HTTP_URL_MAX) == HTTP_URL_MAX ||
            strnlen(r.reason, sizeof(r.reason)) == sizeof(r.reason))
            return false;
    }
    if (http_head_end(b, n, &len) == OK && http_request_parse(b, len, &q) == 0)
        return (q.method == HTTP_GET || q.method == HTTP_HEAD) && q.minor <= 1 &&
               strnlen(q.target, HTTP_LOG_PATH) < HTTP_LOG_PATH;
    return true;
}

bool t_http_fuzz(void)
{
    uint32_t seed = 0x48545450u;
    static uint8_t buf[2048];
    unsigned taken = 0;
    for (unsigned i = 0; i < 3000; i++) {
        size_t n = rng(&seed) % sizeof(buf), k = 0;
        if (i & 1) {   /* half of them a good status line first */
            memcpy(buf, "HTTP/1.1 200 OK\r\n", 17);
            k = n < 17 ? n : 17;
        }
        for (; k < n; k++) {   /* random bytes, many of them the ones heads are made of */
            uint32_t x = rng(&seed);
            buf[k] = x & 1 ? (uint8_t)"\r\n:: \n-0aZ"[(x >> 1) % 11] : (uint8_t)(x >> 8);
        }
        struct http_response r;
        struct http_request q;
        size_t len;
        if (http_head_end(buf, n, &len) == OK) {
            CHECK(len <= n);
            taken += http_response_parse(buf, len, &r) == OK;
            (void)http_request_parse(buf, len, &q);
        }
        struct http_chunks c;
        http_chunks_init(&c);
        size_t at = 0, used, data, dn;
        while (at < n && !c.done &&   /* each turn takes a byte at least, or fails */
               http_chunks_step(&c, buf + at, n - at, &used, &data, &dn) == OK) {
            CHECK(used <= n - at && data + dn <= used);
            if (!used)
                break;
            at += used;
        }
    }
    const char *goods[] = { "HTTP/1.1 301 Moved\r\nContent-Length: 12\r\nLocation: /x\r\n\r\n",
                            "GET /a HTTP/1.1\r\nHost: h\r\nRange: bytes=1-2\r\n\r\n" };
    for (unsigned i = 0; i < 20000; i++) {
        uint32_t x = rng(&seed);
        if (!mutated(goods[i & 1], x >> 8, (uint8_t)x))
            FAIL("a mutated head (byte %u to %u) was taken for more than it is", x >> 8,
                 x & 0xff);
    }
    printf("utest: http_fuzz: 3000 random heads (%u taken as responses), 20000 mutated\n", taken);
    return true;
}
