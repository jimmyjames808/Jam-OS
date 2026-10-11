/* The SSH server's keys as Jam OS keeps them (docs/M13-PLAN.md section 15,
 * "X2 notes"), for init (which makes the host key and decides whether
 * bin/sshd runs) and the shell's `ssh` (which shows them):
 *
 *   /data/etc/ssh/ed25519.pk      the host's Ed25519 public key, 32 bytes
 *   /data/etc/ssh/.ed25519.sk     its secret key, 64 bytes: the seed, then
 *                                 the public key (tinyssh's files, which
 *                                 Monocypher's key pair matches)
 *   /data/etc/authorized_keys     the keys that may log in, OpenSSH's
 *                                 format; tinyssh takes a line that starts
 *                                 "ssh-ed25519 <key>" (no options before
 *                                 it), and so does the count below
 *
 * A fingerprint is OpenSSH's: "SHA256:" and the base64 (no padding) of the
 * SHA-256 of the key's wire form (the string "ssh-ed25519", then the
 * string of the 32 bytes), what `ssh` on the Mac prints at a first
 * connection and `ssh-keygen -l` shows. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

#define SSH_DIR         "/data/etc/ssh"
#define SSH_HOST_PK     SSH_DIR "/ed25519.pk"
#define SSH_HOST_SK     SSH_DIR "/.ed25519.sk"
#define SSH_AUTH_KEYS   "/data/etc/authorized_keys"
#define SSH_PK_BYTES    32
#define SSH_SK_BYTES    64
#define SSH_FP_MAX      51        /* "SHA256:", 43 base64 digits, a NUL */
#define SSH_KEYS_MAX    65536     /* an authorized_keys file read whole, at most */

/* The fingerprint of Ed25519 public key pk, into out. */
void     ssh_fingerprint(const uint8_t pk[SSH_PK_BYTES], char out[SSH_FP_MAX]);
/* How many lines of text (n bytes) are keys tinyssh would take. */
unsigned ssh_count_keys(const char *text, size_t n);
/* The keys in the file at path (ssh_count_keys) into *count. ERR_NOT_FOUND:
 * no file; ERR_OUT_OF_RANGE: bigger than SSH_KEYS_MAX; else the file's
 * errors. */
status_t ssh_keys_in_file(const char *path, unsigned *count);
/* The whole file at path into out, which it must fill exactly: n bytes.
 * ERR_NOT_FOUND: no file; ERR_OUT_OF_RANGE: another size; else the
 * file's errors. */
status_t ssh_file_read(const char *path, uint8_t *out, size_t n);
