"""Signing and checking the ports formats with build/host/jamos-sign
(tools/jamos-sign.c: Monocypher's Ed25519, the code the PC checks with).

The ports key is made once on the Mac, `build/host/jamos-sign keygen
--ports`, into ~/.config/jamos/ports.key (the secret, never in the
repository) and ports.pub (built into the boot image as /boot/ports.pub
by the Makefile's PORTS_KEY). JAMOS_PORTS_KEY names another secret key
file. The self-test signs with a throwaway key of its own (test_key).
"""
import hashlib
import os
import subprocess

from . import pkgformat as fmt

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_KEY = os.path.join(os.path.expanduser("~"), ".config", "jamos", "ports.key")


class SignError(Exception):
    pass


def tool():
    path = os.environ.get("JAMOS_SIGN_TOOL", os.path.join(TOP, "build", "host", "jamos-sign"))
    if not os.access(path, os.X_OK):
        raise SignError("no %s: run make first" % path)
    return path


def default_key():
    return os.environ.get("JAMOS_PORTS_KEY", DEFAULT_KEY)


def sign(text, first, key):
    """text (bytes, its last line a bare "signature") signed with the
    secret key file key, as the format whose first line is first."""
    if not os.path.isfile(key):
        raise SignError("no ports key at %s: make one once with `build/host/jamos-sign keygen "
                        "--ports` (or name one with --key or JAMOS_PORTS_KEY)" % key)
    p = subprocess.run([tool(), "sign", key, fmt.SIGN_FORMAT[first]], input=text,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise SignError("can't sign: %s" % p.stderr.decode(errors="replace").strip())
    return p.stdout


def check(text, first, pub):
    """Whether the public key file pub signed text, as the format whose
    first line is first (the first line checked too)."""
    p = subprocess.run([tool(), "verify", pub, fmt.SIGN_FORMAT[first]], input=text,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p.returncode == 0


def public(key, pub):
    """Write the public key file pub for the secret key file key."""
    p = subprocess.run([tool(), "pub", key], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise SignError("can't read %s: %s" % (key, p.stderr.decode(errors="replace").strip()))
    with open(pub, "wb") as f:
        f.write(p.stdout)


def test_key(d, label="ports self-test"):
    """A throwaway key pair in directory d, (key, pub), made from a seed
    everyone can compute (the SHA-256 of a public phrase), so two machines
    sign the same bytes alike. For tests and test vectors only: its secret
    is public, and no image ever carries its public half."""
    os.makedirs(d, exist_ok=True)
    seed = hashlib.sha256(("jamos %s key: public, for tests only" % label).encode()).hexdigest()
    key, pub = os.path.join(d, "test.key"), os.path.join(d, "test.pub")
    with open(key, "w") as f:
        f.write("ed25519-secret %s\n" % seed)
    os.chmod(key, 0o600)
    public(key, pub)
    return key, pub
