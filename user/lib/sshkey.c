/* The SSH server's keys (<sshkey.h>): fingerprints as OpenSSH prints
 * them (RFC 4253 section 6.6's key blob, RFC 4648's base64 without the
 * padding, as OpenSSH's sshkey_fingerprint does), and the files read. */
#include <os.h>
#include <sha256.h>
#include <sshkey.h>

#define KEY_TYPE "ssh-ed25519"

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* A big-endian length then the bytes (RFC 4251's string), at p: past it. */
static uint8_t *put_string(uint8_t *p, const void *s, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24);
    p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
    memcpy(p + 4, s, n);
    return p + 4 + n;
}

void ssh_fingerprint(const uint8_t pk[SSH_PK_BYTES], char out[SSH_FP_MAX])
{
    uint8_t blob[4 + sizeof(KEY_TYPE) - 1 + 4 + SSH_PK_BYTES];
    uint8_t *p = put_string(blob, KEY_TYPE, sizeof(KEY_TYPE) - 1);
    put_string(p, pk, SSH_PK_BYTES);
    uint8_t d[SHA256_BYTES];
    sha256(blob, sizeof(blob), d);
    char *o = out + 7;
    memcpy(out, "SHA256:", 7);
    /* 32 bytes: ten whole groups of three, then two left (three digits). */
    for (unsigned i = 0; i < SHA256_BYTES; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < SHA256_BYTES)
            v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < SHA256_BYTES)
            v |= d[i + 2];
        unsigned digits = i + 2 < SHA256_BYTES ? 4 : i + 1 < SHA256_BYTES ? 3 : 2;
        for (unsigned k = 0; k < digits; k++)
            *o++ = b64[(v >> (18 - 6 * k)) & 63];
    }
    *o = 0;
}

unsigned ssh_count_keys(const char *text, size_t n)
{
    static const char want[] = KEY_TYPE " ";
    const size_t wl = sizeof(want) - 1;
    unsigned count = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            end++;
        /* "ssh-ed25519 " and at least one byte of key after it */
        if (end - at > wl && !memcmp(text + at, want, wl) && text[at + wl] != ' ' &&
            text[at + wl] != '\r')
            count++;
        at = end + 1;
    }
    return count;
}

/* The file at path, whole, into a new buffer (*out, malloc'd: the
 * caller's) of *n bytes; at most max. */
static status_t read_whole(const char *path, size_t max, uint8_t **out, size_t *n)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    uint64_t size = 0;
    st = file_stat(&f, &size, NULL);
    if (st == OK && size > max)
        st = ERR_OUT_OF_RANGE;
    uint8_t *buf = st == OK ? jam_malloc(size ? size : 1) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    size_t done = 0;
    if (st == OK)
        st = file_read(&f, 0, buf, size, &done);
    file_close(&f);
    if (st != OK) {
        jam_free(buf);
        return st;
    }
    *out = buf;
    *n = done;
    return OK;
}

status_t ssh_keys_in_file(const char *path, unsigned *count)
{
    uint8_t *buf;
    size_t n;
    status_t st = read_whole(path, SSH_KEYS_MAX, &buf, &n);
    if (st != OK)
        return st;
    *count = ssh_count_keys((const char *)buf, n);
    jam_free(buf);
    return OK;
}

/* Straight into out, no copy on the heap: out may be a secret key. */
status_t ssh_file_read(const char *path, uint8_t *out, size_t n)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    uint64_t size = 0;
    size_t done = 0;
    st = file_stat(&f, &size, NULL);
    if (st == OK && size != n)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = file_read(&f, 0, out, n, &done);
    if (st == OK && done != n)
        st = ERR_OUT_OF_RANGE;
    file_close(&f);
    return st;
}
