#!/usr/bin/env python3
"""
OpenUT2k4 signing tools (Server/Docs/Validator.md "Release signing" and "Shared ban lists").
Plain Python 3, no third-party modules. Ed25519 keys, kept OUTSIDE the repository. Nothing secret ever goes into a
build, and none of this concerns the players' identity keys (each client makes its own on first start).

Release archives (the project owner), detached signatures in minisign's format: players can check a download with
the stock `minisign` tool or with `verify` below.
  genkey  [--out DIR] [--name release] [--force]
                                  a key, once (default ~/.config/openut2k4/signing/<name>.key mode 0600, <name>.pub)
  pubkey  --key FILE [--hex]      the minisign public key line to publish; --hex: the raw key (TrustedBanIssuer=)
  sign    --key FILE FILE...      FILE.minisig (minisign "ED": BLAKE2b-512 prehash) and FILE.sha256 for each file
  verify  --pub KEY|FILE FILE...  checks FILE.minisig (also: minisign -Vm FILE -P <public key>)

Shared ban lists (server admins; a key made with genkey --name banlist):
  banlist-export --ini FILE --issuer NAME [--out FILE]
                                  an unsigned list from a validator state file (System/OpenUT2k4ValidatorKeys.ini of
                                  the mutator, or <state dir>/ValidatorKeys.ini of our server)
  banlist-sign   --key FILE --in FILE [--out FILE]
                                  adds key= and sig= (Ed25519 over everything before the sig= line)
  banlist-verify --trust HEX [--trust HEX...] FILE
  banlist-import --trust HEX [--trust HEX...] [--trust-ini UT2004.ini] --out-ini System/OpenUT2k4ValidatorShared.ini
                 SOURCE...        fetches files or http(s) URLs, checks each list's signature against the trusted
                                  issuer keys and writes the mutator's shared ban file (run it from cron; UnrealScript
                                  cannot fetch HTTPS or check Ed25519 itself). Our own server fetches natively.

Never commit a key file or paste it into a log or a CI variable dump.
"""

import argparse
import base64
import hashlib
import os
import secrets
import sys
import time

# ---------------------------------------------------------------------------------------------------------------------
# Ed25519 (RFC 8032), plain integers. Slow but fine for signing a few archives.
# ---------------------------------------------------------------------------------------------------------------------

P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P
SQRT_M1 = pow(2, (P - 1) // 4, P)


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * pow(D * y * y + 1, P - 2, P) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P != 0:
        x = x * SQRT_M1 % P
    if (x * x - x2) % P != 0:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * pow(5, P - 2, P) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)
IDENTITY = (0, 1, 1, 0)


def _add(a, b):
    x1, y1, z1, t1 = a
    x2, y2, z2, t2 = b
    aa = (y1 - x1) * (y2 - x2) % P
    bb = (y1 + x1) * (y2 + x2) % P
    cc = t1 * 2 * D * t2 % P
    dd = z1 * 2 * z2 % P
    e, f, g, h = bb - aa, dd - cc, dd + cc, bb + aa
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, pt):
    q = IDENTITY
    while s > 0:
        if s & 1:
            q = _add(q, pt)
        pt = _add(pt, pt)
        s >>= 1
    return q


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % P == 0 and (a[1] * b[2] - b[1] * a[2]) % P == 0


def _compress(pt):
    zinv = pow(pt[2], P - 2, P)
    x, y = pt[0] * zinv % P, pt[1] * zinv % P
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def _decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _h_mod_l(*parts):
    return int.from_bytes(hashlib.sha512(b"".join(parts)).digest(), "little") % L


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_public(seed):
    a, _ = _expand(seed)
    return _compress(_mul(a, G))


def ed25519_sign(seed, msg):
    a, prefix = _expand(seed)
    pub = _compress(_mul(a, G))
    r = _h_mod_l(prefix, msg)
    rs = _compress(_mul(r, G))
    s = (r + _h_mod_l(rs, pub, msg) * a) % L
    return rs + s.to_bytes(32, "little")


def ed25519_verify(pub, msg, sig):
    if len(sig) != 64:
        return False
    a = _decompress(pub)
    r = _decompress(sig[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= L:
        return False
    h = _h_mod_l(sig[:32], pub, msg)
    return _equal(_mul(s, G), _add(r, _mul(h, a)))


# ---------------------------------------------------------------------------------------------------------------------
# Key files and minisign formats
# ---------------------------------------------------------------------------------------------------------------------

KEY_MAGIC = "OUT2K4ED25519"


def repo_root():
    return os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def inside(path, root):
    path = os.path.realpath(path)
    return path == root or path.startswith(root + os.sep)


def read_key(path):
    path = os.path.expanduser(path)
    if inside(path, repo_root()):
        sys.exit("error: the key %s is inside the repository; move it out (and never commit it)" % path)
    fields = {}
    with open(path, "r", encoding="ascii") as f:
        lines = [l.strip() for l in f]
    if not lines or lines[0] != KEY_MAGIC:
        sys.exit("error: %s is not an OpenUT2k4 release key" % path)
    for line in lines[1:]:
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            fields[k] = v
    seed, key_id = bytes.fromhex(fields["seed"]), bytes.fromhex(fields["keyid"])
    if len(seed) != 32 or len(key_id) != 8:
        sys.exit("error: %s: damaged key" % path)
    return seed, key_id


def key_id_text(key_id):
    return "%016X" % int.from_bytes(key_id, "little")


def public_line(seed, key_id):
    return base64.b64encode(b"Ed" + key_id + ed25519_public(seed)).decode("ascii")


def parse_public(arg):
    """A minisign public key: the base64 line, or a .pub file holding it."""
    if os.path.isfile(arg):
        with open(arg, "r", encoding="ascii") as f:
            lines = [l.strip() for l in f if l.strip() and not l.startswith("untrusted comment:")]
        arg = lines[0]
    raw = base64.b64decode(arg)
    if len(raw) != 42 or raw[:2] != b"Ed":
        sys.exit("error: not a minisign Ed25519 public key")
    return raw[2:10], raw[10:]


def blake2b_file(path):
    h = hashlib.blake2b(digest_size=64)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_genkey(args):
    out = os.path.expanduser(args.out)
    if inside(out, repo_root()):
        sys.exit("error: %s is inside the repository (%s); keys must live outside it" % (out, repo_root()))
    if not args.name.replace("-", "").replace("_", "").isalnum():
        sys.exit("error: --name: letters, digits, - and _ only")
    os.makedirs(out, mode=0o700, exist_ok=True)
    os.chmod(out, 0o700)
    key_path = os.path.join(out, args.name + ".key")
    if os.path.exists(key_path) and not args.force:
        sys.exit("error: %s exists (keep it; --force replaces it and players must then trust a new public key)" % key_path)
    seed, key_id = secrets.token_bytes(32), secrets.token_bytes(8)
    data = "%s\n# OpenUT2k4 %s signing key, made %s. PRIVATE: never commit, publish or log it.\nkeyid=%s\nseed=%s\n" % (
        KEY_MAGIC, args.name, time.strftime("%Y-%m-%d"), key_id.hex(), seed.hex())
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, data.encode("ascii"))
    finally:
        os.close(fd)
    os.chmod(key_path, 0o600)
    pub_path = os.path.join(out, args.name + ".pub")
    with open(pub_path, "w", encoding="ascii") as f:
        f.write("untrusted comment: minisign public key %s\n%s\n" % (key_id_text(key_id), public_line(seed, key_id)))
    print("private key: %s (mode 0600; back it up offline)" % key_path)
    print("public key:  %s" % pub_path)
    print("minisign public key line: %s" % public_line(seed, key_id))
    print("raw public key (TrustedBanIssuer=): %s" % ed25519_public(seed).hex())


def cmd_pubkey(args):
    seed, key_id = read_key(args.key)
    if args.hex:
        print(ed25519_public(seed).hex())
        return
    print("untrusted comment: minisign public key %s" % key_id_text(key_id))
    print(public_line(seed, key_id))


def cmd_sign(args):
    seed, key_id = read_key(args.key)
    for path in args.files:
        digest = blake2b_file(path)
        sig = ed25519_sign(seed, digest)
        trusted = "timestamp:%d\tfile:%s\thashed" % (int(time.time()), os.path.basename(path))
        global_sig = ed25519_sign(seed, sig + trusted.encode("utf-8"))
        with open(path + ".minisig", "w", encoding="utf-8") as f:
            f.write("untrusted comment: signature from OpenUT2k4 release key %s\n" % key_id_text(key_id))
            f.write(base64.b64encode(b"ED" + key_id + sig).decode("ascii") + "\n")
            f.write("trusted comment: " + trusted + "\n")
            f.write(base64.b64encode(global_sig).decode("ascii") + "\n")
        with open(path + ".sha256", "w", encoding="ascii") as f:
            f.write("%s  %s\n" % (sha256_file(path), os.path.basename(path)))
        print("signed %s" % path)


def cmd_verify(args):
    key_id, pub = parse_public(args.pub)
    ok = True
    for path in args.files:
        with open(path + ".minisig", "r", encoding="utf-8") as f:
            lines = [l.rstrip("\n") for l in f]
        raw = base64.b64decode(lines[1])
        trusted = lines[2][len("trusted comment: "):]
        global_sig = base64.b64decode(lines[3])
        alg, sig_key, sig = raw[:2], raw[2:10], raw[10:]
        if sig_key != key_id:
            print("%s: signed by another key (%s)" % (path, key_id_text(sig_key)))
            ok = False
            continue
        if alg == b"ED":
            message = blake2b_file(path)
        elif alg == b"Ed":
            with open(path, "rb") as f:
                message = f.read()
        else:
            print("%s: unknown signature algorithm" % path)
            ok = False
            continue
        good = ed25519_verify(pub, message, sig) and ed25519_verify(pub, sig + trusted.encode("utf-8"), global_sig)
        print("%s: %s (%s)" % (path, "signature OK" if good else "BAD SIGNATURE", trusted))
        ok &= good
    sys.exit(0 if ok else 1)


# ---------------------------------------------------------------------------------------------------------------------
# Shared ban lists
# ---------------------------------------------------------------------------------------------------------------------

BANLIST_MAGIC = "OUT2K4BANLIST1"
MAX_LIST_BYTES = 8 << 20
HEX = set("0123456789abcdef")


def _is_hex(s, n):
    return len(s) == n and set(s.lower()) <= HEX


def _is_ip(s):
    parts = s.split(".")
    return len(parts) == 4 and all(p.isdigit() and 0 <= int(p) <= 255 for p in parts)


def _clean(s, n=80):
    return "".join(c for c in s if c not in '|"\r\n' and 32 <= ord(c) < 127)[:n]


def clean_entry(fields, issuer=None):
    """A ban entry's 7 fields checked and cleaned; None when it bans nothing or is malformed."""
    f = (list(fields) + [""] * 7)[:7]
    fp, ip, cidr, legacy = f[0].lower(), f[1], f[2], f[3].lower()
    if fp and not _is_hex(fp, 32):
        return None
    if ip and not _is_ip(ip):
        return None
    if cidr:
        if "/" not in cidr:
            return None
        net, bits = cidr.split("/", 1)
        if not _is_ip(net) or not bits.isdigit() or not 8 <= int(bits) <= 32:
            return None
    if legacy and not _is_hex(legacy, 32):
        return None
    if not (fp or ip or cidr or legacy):
        return None
    return [fp, ip, cidr, legacy, _clean(f[4], 20), _clean(f[5]), _clean(issuer if issuer is not None else f[6], 40)]


def read_ini_values(path, section, key):
    out, cur = [], None
    with open(path, "rb") as fh:
        text = fh.read().decode("latin-1")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1].lower()
        elif cur == section.lower() and "=" in line:
            k, v = line.split("=", 1)
            if k.strip().lower() == key.lower():
                out.append(v.strip().strip('"'))
    return out


def parse_banlist(text, trusted):
    """(issuer, date, entries) of a signed list whose key is trusted; raises ValueError otherwise."""
    text = text.replace("\r\n", "\n")
    if not text.startswith(BANLIST_MAGIC + "\n"):
        raise ValueError("not an OpenUT2k4 ban list")
    at = text.rfind("\nsig=")
    if at < 0:
        raise ValueError("unsigned list")
    body, sig_hex = text[:at + 1], text[at + 5:].strip()
    fields = {}
    entries = []
    for line in body.split("\n")[1:]:
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k == "ban":
            entries.append(v.split("|"))
        else:
            fields[k] = v
    key_hex = fields.get("key", "").lower()
    if not _is_hex(key_hex, 64):
        raise ValueError("no issuer key in the list")
    if key_hex not in trusted:
        raise ValueError("issuer key %s is not trusted" % key_hex[:16])
    if not _is_hex(sig_hex, 128) or not ed25519_verify(bytes.fromhex(key_hex), body.encode("utf-8"), bytes.fromhex(sig_hex)):
        raise ValueError("BAD SIGNATURE")
    issuer = _clean(fields.get("issuer", trusted[key_hex]), 40)
    good = [e for e in (clean_entry(x, issuer) for x in entries) if e]
    return issuer, fields.get("date", ""), good, len(entries) - len(good)


def cmd_banlist_export(args):
    bans = read_ini_values(args.ini, "OpenUT2k4Validator.OUTKeyStore", "Bans")
    lines = [BANLIST_MAGIC, "issuer=" + _clean(args.issuer, 40), "date=" + time.strftime("%Y-%m-%d %H:%M")]
    n = 0
    for b in bans:
        e = clean_entry(b.split("|"))
        if e:
            lines.append("ban=" + "|".join(e))
            n += 1
    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print("%d bans -> %s (unsigned: sign it with banlist-sign)" % (n, args.out))
    else:
        sys.stdout.write(text)


def cmd_banlist_sign(args):
    seed, _ = read_key(args.key)
    with open(args.inp, "r", encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")
    if not text.startswith(BANLIST_MAGIC + "\n"):
        sys.exit("error: not an OpenUT2k4 ban list")
    lines = [l for l in text.split("\n") if l and not l.startswith("sig=") and not l.startswith("key=")]
    lines.insert(1, "key=" + ed25519_public(seed).hex())
    body = "\n".join(lines) + "\n"
    sig = ed25519_sign(seed, body.encode("utf-8"))
    out = args.out or args.inp
    with open(out, "w", encoding="utf-8") as f:
        f.write(body + "sig=" + sig.hex() + "\n")
    print("signed %s (%d bans)" % (out, sum(1 for l in lines if l.startswith("ban="))))


def _trusted(args):
    trusted = {}
    for t in args.trust or []:
        hexkey = t.split()[0].lower()
        if not _is_hex(hexkey, 64):
            sys.exit("error: --trust needs a 64 hex digit Ed25519 public key (pubkey --hex)")
        trusted[hexkey] = t.split(None, 1)[1] if " " in t else hexkey[:16]
    for ini in getattr(args, "trust_ini", None) or []:
        for line in read_ini_values(ini, "OpenUT2k4Validator.OUTValidator", "TrustedBanIssuer"):
            parts = line.split(None, 1)
            if parts and _is_hex(parts[0].lower(), 64):
                trusted[parts[0].lower()] = parts[1] if len(parts) > 1 else parts[0][:16]
    if not trusted:
        sys.exit("error: no trusted issuer keys (--trust or --trust-ini)")
    return trusted


def _fetch(source):
    if source.startswith("http://") or source.startswith("https://"):
        import urllib.request
        with urllib.request.urlopen(source, timeout=30) as r:
            data = r.read(MAX_LIST_BYTES + 1)
    else:
        with open(source, "rb") as f:
            data = f.read(MAX_LIST_BYTES + 1)
    if len(data) > MAX_LIST_BYTES:
        raise ValueError("list larger than %d bytes" % MAX_LIST_BYTES)
    return data.decode("utf-8")


def cmd_banlist_verify(args):
    trusted = _trusted(args)
    ok = True
    for src in args.files:
        try:
            issuer, date, entries, bad = parse_banlist(_fetch(src), trusted)
            print("%s: signature OK, issuer %s, %s, %d bans (%d malformed skipped)" % (src, issuer, date, len(entries), bad))
        except (ValueError, OSError) as e:
            print("%s: REJECTED: %s" % (src, e))
            ok = False
    sys.exit(0 if ok else 1)


def cmd_banlist_import(args):
    trusted = _trusted(args)
    section = "OpenUT2k4Validator.OUTSharedBans"
    old_bans = read_ini_values(args.out_ini, section, "Bans") if os.path.exists(args.out_ini) else []
    old_sources = read_ini_values(args.out_ini, section, "Sources") if os.path.exists(args.out_ini) else []
    bans, sources, failed = [], [], 0
    for src in args.sources:
        try:
            issuer, date, entries, bad = parse_banlist(_fetch(src), trusted)
            bans += ["|".join(e) for e in entries]
            sources.append("%s|%s|%s|%d|%s" % (_clean(src, 200), issuer, date, len(entries), time.strftime("%Y-%m-%d %H:%M")))
            print("%s: imported %d bans from %s (%s)%s" % (src, len(entries), issuer, date, ", %d malformed skipped" % bad if bad else ""))
        except (ValueError, OSError) as e:
            failed += 1
            print("%s: REJECTED (%s); keeping its previous entries" % (src, e))
            for line in old_sources:
                parts = line.split("|")
                if parts and parts[0] == _clean(src, 200):
                    sources.append(line)
                    bans += [b for b in old_bans if b.split("|")[-1] == parts[1]]
    seen, unique = set(), []
    for b in bans:
        if b not in seen:
            seen.add(b)
            unique.append(b)
    lines = ["; written by Client/Tools/openut2k4_signing.py banlist-import %s - do not edit, re-import instead" % time.strftime("%Y-%m-%d %H:%M"),
             "[%s]" % section] + ["Sources=" + s for s in sources] + ["Bans=" + b for b in unique]
    tmp = args.out_ini + ".tmp"
    with open(tmp, "wb") as f:
        f.write(("\r\n".join(lines) + "\r\n").encode("latin-1", "replace"))
    os.replace(tmp, args.out_ini)
    print("%s: %d shared bans from %d sources (%d rejected). The mutator reads it when the next map starts." % (args.out_ini, len(unique), len(sources), failed))
    sys.exit(1 if failed else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("genkey")
    p.add_argument("--out", default="~/.config/openut2k4/signing")
    p.add_argument("--name", default="release")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_genkey)
    p = sub.add_parser("pubkey")
    p.add_argument("--key", required=True)
    p.add_argument("--hex", action="store_true")
    p.set_defaults(func=cmd_pubkey)
    p = sub.add_parser("sign")
    p.add_argument("--key", required=True)
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_sign)
    p = sub.add_parser("verify")
    p.add_argument("--pub", required=True)
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_verify)
    p = sub.add_parser("banlist-export")
    p.add_argument("--ini", required=True)
    p.add_argument("--issuer", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_banlist_export)
    p = sub.add_parser("banlist-sign")
    p.add_argument("--key", required=True)
    p.add_argument("--in", dest="inp", required=True)
    p.add_argument("--out")
    p.set_defaults(func=cmd_banlist_sign)
    p = sub.add_parser("banlist-verify")
    p.add_argument("--trust", action="append")
    p.add_argument("--trust-ini", action="append")
    p.add_argument("files", nargs="+")
    p.set_defaults(func=cmd_banlist_verify)
    p = sub.add_parser("banlist-import")
    p.add_argument("--trust", action="append")
    p.add_argument("--trust-ini", action="append")
    p.add_argument("--out-ini", required=True)
    p.add_argument("sources", nargs="+")
    p.set_defaults(func=cmd_banlist_import)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
