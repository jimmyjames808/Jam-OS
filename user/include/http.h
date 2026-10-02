/* <http.h>: the small HTTP/1.1 of `fetch` and `serve` (user/lib/http.c):
 * URLs, a response's head and its chunked body for the client, a request's
 * head for the server, and a file's Content-Type. Pure functions over
 * bytes: nothing here touches the network, so utest's http_* tests feed
 * them everything a hostile peer could send.
 *
 * Strict and bounded on purpose (RFC 9110, 9112): a head is at most
 * HTTP_HEAD_MAX bytes and HTTP_HEADERS_MAX lines, each line ends in CRLF
 * (a bare LF is accepted, a bare CR is not), a line starting with a space
 * (the obsolete folding) is refused, header names are tokens, numbers are
 * plain decimal digits that must not overflow, and the headers that frame
 * a body (Content-Length, Transfer-Encoding) must agree with each other.
 * Anything else is an error, never a guess: the caller stops.
 *
 * Only http:// is spoken: https needs TLS, which Jam OS does not have
 * (not in M9.5; http_url_parse says ERR_NOT_SUPPORTED). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define HTTP_HEAD_MAX    16384u   /* bytes of a head: the first line, the headers, the blank line */
#define HTTP_HEADERS_MAX 64u      /* header lines in a head */
#define HTTP_HOST_MAX    254u     /* a host name's bytes (a DNS name's most), without the NUL */
#define HTTP_PATH_MAX    1024u    /* a URL's path and query, with the NUL */
#define HTTP_URL_MAX     1400u    /* a whole URL (a Location), with the NUL */
#define HTTP_CHUNK_LINE  256u     /* a chunk's size line, its extensions included */
#define HTTP_LENGTH_MAX  (1ull << 50)   /* the largest body length taken (1 PiB) */
#define HTTP_LOG_PATH    64u      /* bytes of a request's target kept for the log, with the NUL */

/* ---- URLs ---------------------------------------------------------------------------- */

struct http_url {
    char     host[HTTP_HOST_MAX + 1];   /* a name or a dotted address, as given (lower case) */
    uint16_t port;                      /* 80 unless the URL says */
    char     path[HTTP_PATH_MAX];       /* from the first '/' on, the query kept, "/" at least */
};

/* "http://host[:port][/path][?query]" (no user@, no fragment kept: '#'
 * ends it). The scheme and host are taken in any case; the path's bytes
 * must be printable ASCII without spaces. ERR_NOT_SUPPORTED: https:// or
 * another scheme; ERR_INVALID_ARGS: not such a URL (no host, a port that
 * isn't 1..65535, a user@, a control character or a space, too long). */
status_t http_url_parse(const char *s, struct http_url *out);
/* A Location header against the URL it answered: an absolute URL
 * (http_url_parse), "//host/path", an absolute path "/x" or a relative one
 * (against base's directory). Errors as http_url_parse's. */
status_t http_url_resolve(const struct http_url *base, const char *loc, struct http_url *out);
/* The file name the URL suggests: its path's last part, without the query,
 * each byte outside [A-Za-z0-9._+-] made '_', at most cap - 1 bytes;
 * "index.html" when the path ends in '/'. Never "", "." or "..". */
void     http_url_filename(const struct http_url *u, char *out, size_t cap);
/* host[:port] as a Host header writes it (the port only when not 80). */
void     http_url_hostport(const struct http_url *u, char *out, size_t cap);

/* ---- heads ------------------------------------------------------------------------------ */

/* Where the head in buf[0..n) ends (after its blank line): OK with *len;
 * ERR_SHOULD_WAIT: not yet, read more; ERR_OUT_OF_RANGE: no end within
 * HTTP_HEAD_MAX bytes (a head too long). */
status_t http_head_end(const uint8_t *buf, size_t n, size_t *len);

/* A response's head, as the client takes it. */
struct http_response {
    unsigned status;                 /* 100..599 */
    unsigned minor;                  /* HTTP/1.minor: 0 or 1 */
    int64_t  length;                 /* Content-Length; -1: none */
    bool     chunked;                /* Transfer-Encoding: chunked */
    bool     close;                  /* the connection ends after it (Connection: close, 1.0) */
    char     reason[48];             /* the reason phrase, printable, cut */
    char     location[HTTP_URL_MAX]; /* Location; "": none */
};
/* The head (http_head_end's len bytes). ERR_INVALID_ARGS: not a response
 * this file takes (the status line, a header's form, a Content-Length
 * that isn't a number or disagrees with another, too many headers, a
 * Location too long); ERR_NOT_SUPPORTED: a Transfer-Encoding other than
 * chunked alone. With chunked, any Content-Length is left out (-1). */
status_t http_response_parse(const uint8_t *head, size_t len, struct http_response *out);

/* What a body is, after a response: true if one follows at all (not for a
 * HEAD request, a 1xx, 204 or 304). */
bool     http_response_has_body(const struct http_response *r, bool head_request);

/* ---- a chunked body ------------------------------------------------------------------- */

struct http_chunks {
    unsigned state;    /* where in the framing (http.c's CH_*) */
    uint64_t left;     /* bytes left of the chunk being read */
    unsigned line;     /* bytes of the size line or a trailer line so far */
    unsigned trailer;  /* bytes of trailers so far (HTTP_HEAD_MAX at most) */
    bool     digits;   /* the size line has a digit */
    bool     done;     /* the last chunk and its trailers have ended */
};
void     http_chunks_init(struct http_chunks *c);
/* Step through in[0..n): *used bytes taken; of them, the body's bytes are
 * in[*data .. *data + *data_n) (none: *data_n 0). Call again with the rest
 * while *used < n and !c->done. ERR_INVALID_ARGS: broken framing (a size
 * that isn't hex, over HTTP_LENGTH_MAX, a size line over HTTP_CHUNK_LINE,
 * no CRLF after a chunk, trailers over HTTP_HEAD_MAX). */
status_t http_chunks_step(struct http_chunks *c, const uint8_t *in, size_t n, size_t *used,
                          size_t *data, size_t *data_n);

/* ---- a request, as the server takes it --------------------------------------------- */

enum http_method { HTTP_GET, HTTP_HEAD };

struct http_request {
    enum http_method method;
    unsigned minor;                  /* HTTP/1.minor */
    bool     close;                  /* the client asked to close after it (or 1.0) */
    bool     range;                  /* a single byte range was asked for */
    bool     suffix;                 /* ... "bytes=-N": the last `last` bytes */
    uint64_t first, last;            /* ... "bytes=first-last" (last UINT64_MAX: to the end) */
    char     target[HTTP_LOG_PATH];  /* the start of the target, printable, for the log only */
};
/* The head of a request. The answer when it can't be served: 0 if it can;
 * else the status to answer with (then close): 400 (not a request this
 * file takes, or one with a body), 501 (a method other than GET or HEAD:
 * *out still has the target for the log), 505 (not HTTP/1.x). A Range
 * header that isn't one byte range is left out (the whole file is served,
 * as RFC 9110 allows). */
unsigned http_request_parse(const uint8_t *head, size_t len, struct http_request *out);
/* The bytes a Range asks for out of a file of `size`: OK with *from and *n;
 * ERR_OUT_OF_RANGE: none of it is in the file (answer 416). Without a
 * range: the whole file. */
status_t http_request_span(const struct http_request *r, uint64_t size, uint64_t *from,
                           uint64_t *n);

/* The reason phrase for a status this code sends ("OK", "Not Found"). */
const char *http_reason(unsigned status);
/* The Content-Type for a file name, by its extension (case-insensitive);
 * application/octet-stream when unknown. */
const char *http_content_type(const char *name);
