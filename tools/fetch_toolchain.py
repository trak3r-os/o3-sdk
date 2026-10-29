#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fetch_toolchain.py: download the Arm GNU Toolchain 15.3.Rel1 (arm-none-eabi) for this host into .toolchains/, after checking its SHA-256.

    python3 -I tools/fetch_toolchain.py [--dir DIR] [--host HOST] [--force] [--print-root]
    python3 -I tools/fetch_toolchain.py --from-dir DIR          # the archive was downloaded by hand into DIR: check it and unpack it
    python3 -I tools/fetch_toolchain.py --self-test

What it does, in this order, and stops at the first failure:
  1. picks the archive for this host (x86_64 Linux, aarch64 Linux, macOS on Apple silicon: the three Arm publishes for 15.3.Rel1; any other
     host is told so and the manual download is named);
  2. downloads it from Arm's own package registry (HTTPS, certificate checked, a size limit) into a scratch directory under .toolchains/,
     or takes it from --from-dir;
  3. checks its SHA-256 against the value pinned below (Arm's published .sha256asc, fetched when this was written, see PINS): a mismatch
     deletes the file and nothing is unpacked;
  4. unpacks it into a scratch directory, refusing a member that would land outside it (absolute path, `..`, a link out) and an archive
     whose single top directory is not the expected one, and checks that bin/arm-none-eabi-gcc is there;
  5. moves it to .toolchains/arm-gnu-toolchain-15.3.rel1-<host>-arm-none-eabi/ in one rename, so a failed run leaves no half-made toolchain.
It prints the directory (the `ARM_GNU_ROOT` of the build). tools/ci.sh and cmake/arm-none-eabi-toolchain.cmake look in .toolchains/ when
ARM_GNU_ROOT is not set, so after this the cross build needs no option. An explicit ARM_GNU_ROOT still wins everywhere.

An installed toolchain is left alone (nothing is downloaded) unless --force. --dir is the .toolchains directory (default: the one at the
root of this checkout, git-ignored). The standard library only; run it with `python3 -I`. Exit status: 0 installed (or already there),
1 a usage error or a host Arm publishes no archive for, 2 the download failed or the file is not the pinned one (checksum or size),
3 the archive is damaged or unsafe, or is not the toolchain.
"""
import argparse
import hashlib
import http.server
import io
import os
import platform
import re
import shutil
import sys
import tarfile
import tempfile
import threading
import urllib.error
import urllib.request

VERSION = "15.3.rel1"
REGISTRY = "https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/" + VERSION
SDK_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_BYTES = 400 * 1024 * 1024  # the real archives are 140-160 MB; a download over this is not one of them
GCC_VERSION_RE = re.compile(r"15\.3\.")

# The pinned archives: host -> (SHA-256, size in bytes). Each SHA-256 is the first field of Arm's own `<archive>.sha256asc` file at
#   REGISTRY/<archive>.sha256asc
# fetched on 2026-10-29 (the x86_64 one is also the value track3r-os pins in docs/toolchain.md and tools/ci.sh); the size is the registry's
# Content-Length of the same day. Arm publishes no darwin-x86_64 archive for 15.3.Rel1 (404), so that host has no entry. To pin another
# release: change VERSION and fetch its three .sha256asc files; never copy a value from anywhere else.
PINS = {
    "x86_64-linux": ("563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa", 160131532),
    "aarch64-linux": ("06979e0c8171de58e5dc2a2b2019330a290f30930f27728af98a83e1a7369b3a", 153660136),
    "darwin-arm64": ("376808a59ca209c1413236f1c6a509e33da4b29857ab28642b9927cf3048af55", 143657708),
}
# Arm's archive names carry the host as they spell it.
ARM_HOST = {"x86_64-linux": "x86_64", "aarch64-linux": "aarch64", "darwin-arm64": "darwin-arm64"}


class FetchError(Exception):
    def __init__(self, message, status):
        super().__init__(message)
        self.status = status


def host_id(system=None, machine=None):
    """The key of PINS for this host, or None. `system` and `machine` are platform.system() and platform.machine() by default."""
    system = (system or platform.system()).lower()
    machine = (machine or platform.machine()).lower()
    if system == "linux" and machine in ("x86_64", "amd64"):
        return "x86_64-linux"
    if system == "linux" and machine in ("aarch64", "arm64"):
        return "aarch64-linux"
    if system == "darwin" and machine in ("arm64", "aarch64"):
        return "darwin-arm64"
    return None


def archive_stem(host):
    return "arm-gnu-toolchain-%s-%s-arm-none-eabi" % (VERSION, ARM_HOST[host])


def archive_name(host):
    return archive_stem(host) + ".tar.xz"


def parse_sha256asc(text):
    """The digest of a `<hex>  <file>` line (Arm's .sha256asc), or None."""
    for line in text.splitlines():
        match = re.match(r"^([0-9a-fA-F]{64})\s+\*?(\S+)\s*$", line)
        if match:
            return match.group(1).lower()
    return None


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def installed(directory):
    return os.access(os.path.join(directory, "bin", "arm-none-eabi-gcc"), os.X_OK)


def download(url, dest, limit, out):
    """`url` into `dest` (a new file), at most `limit` bytes. FetchError(…, 2) on any failure."""
    request = urllib.request.Request(url, headers={"User-Agent": "o3sdk-fetch-toolchain"})
    print("fetch_toolchain: downloading %s" % url, file=out)
    try:
        with urllib.request.urlopen(request, timeout=60) as response, open(dest, "wb") as f:
            length = response.headers.get("Content-Length")
            if length is not None and length.isdigit() and int(length) > limit:
                raise FetchError("the server announces %s bytes, more than the %d this archive can be" % (length, limit), 2)
            total = 0
            shown = 0
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise FetchError("the download is over %d bytes: not the toolchain" % limit, 2)
                f.write(chunk)
                if total - shown >= 32 << 20:
                    shown = total
                    print("fetch_toolchain:   %d MiB" % (total >> 20), file=out)
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise FetchError("cannot download %s: %s" % (url, error), 2)


def check_archive(path, sha256, size, out):
    """The file at `path` is the pinned one: its size (when known) and SHA-256."""
    actual_size = os.path.getsize(path)
    if size is not None and actual_size != size:
        raise FetchError("%s is %d bytes, the pinned archive is %d" % (os.path.basename(path), actual_size, size), 2)
    actual = sha256_of(path)
    if actual != sha256:
        raise FetchError("SHA-256 mismatch for %s: got %s, pinned %s" % (os.path.basename(path), actual, sha256), 2)
    print("fetch_toolchain: SHA-256 %s verified" % sha256, file=out)


def resolve_member(path, links, depth=0):
    """`path` (relative to the unpack directory, '/' separated) as it will be on disk once the symlinks seen so far exist: every symlink on the way,
    the last component included, is followed, '..' included. `links` maps a resolved link path to its target text. The resolved path ('' is the
    unpack directory itself), or None when it climbs above the unpack directory or loops."""
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                return None
            parts.pop()
            continue
        parts.append(part)
        here = "/".join(parts)
        if here in links:
            if depth > 40:
                return None
            parts.pop()
            target = resolve_member("/".join(parts + [links[here]]), links, depth + 1)
            if target is None:
                return None
            parts = [x for x in target.split("/") if x]
    return "/".join(parts)


def safe_extract(archive, dest, top):
    """Unpacks `archive` (tar.xz) into the existing empty directory `dest`; its members must all sit under the one directory `top`.
    Links are resolved the way the file system will, through the symlinked directories the archive itself makes earlier (resolve_member), so a
    link that goes up through one cannot pass; nothing, not even a file written through a link, may land outside `top`.
    FetchError(…, 3) for a member that escapes, a link that points outside the toolchain, a different top directory, or a damaged file."""
    real = os.path.realpath(dest)

    def in_top(resolved):
        return resolved is not None and (resolved == top or resolved.startswith(top + "/"))

    try:
        with tarfile.open(archive, "r:xz") as tar:
            members = tar.getmembers()
            if not members:
                raise FetchError("the archive is empty", 3)
            links = {}
            for m in members:
                name = m.name
                if name.startswith("/") or ".." in name.split("/") or not (name == top or name.startswith(top + "/")):
                    raise FetchError("the archive has a member outside %s/: %r" % (top, name), 3)
                if m.isdev() or m.isfifo():
                    raise FetchError("the archive has a device or a pipe: %r" % name, 3)
                if name == top:
                    continue
                parent = resolve_member(os.path.dirname(name), links)
                if not in_top(parent):
                    raise FetchError("the member %r sits outside the toolchain once the archive's links are followed" % name, 3)
                here = parent + "/" + os.path.basename(name)
                if m.issym():
                    if os.path.isabs(m.linkname) or not in_top(resolve_member(parent + "/" + m.linkname, links)):
                        raise FetchError("the link %r points outside the toolchain: %r" % (name, m.linkname), 3)
                    links[here] = m.linkname
                    continue
                if m.islnk():
                    if os.path.isabs(m.linkname) or not in_top(resolve_member(m.linkname, links)):
                        raise FetchError("the hard link %r points outside the toolchain: %r" % (name, m.linkname), 3)
                elif not in_top(resolve_member(here, links)):  # a file or directory that would be written through a link of the archive's
                    raise FetchError("the member %r would be written outside the toolchain through a link" % name, 3)
            if hasattr(tarfile, "data_filter"):
                tar.extractall(real, members=members, filter="data")
            else:
                tar.extractall(real, members=members)
    except (tarfile.TarError, EOFError, OSError) as error:
        raise FetchError("cannot unpack %s: %s" % (os.path.basename(archive), error), 3)


def install(host, tools_dir, sha256, size, out, source_dir=None, url_base=REGISTRY, force=False, limit=MAX_BYTES):
    """Installs the toolchain of `host` under `tools_dir`; returns its directory. `sha256`/`size` is the pin (size may be None)."""
    stem = archive_stem(host)
    final = os.path.join(tools_dir, stem)
    if installed(final) and not force:
        print("fetch_toolchain: %s is already installed" % final, file=out)
        return final
    if os.path.lexists(final) and not installed(final) and not force:
        raise FetchError("%s exists but holds no bin/arm-none-eabi-gcc: not touched (remove it, or --force replaces it)" % final, 1)
    os.makedirs(tools_dir, exist_ok=True)
    work = tempfile.mkdtemp(prefix=".fetch-", dir=tools_dir)  # a new empty directory of our own: the archive and the unpacked tree live here
    try:
        archive = os.path.join(work, archive_name(host))
        if source_dir is not None:
            source = os.path.join(source_dir, archive_name(host))
            if not os.path.isfile(source):
                raise FetchError("%s is not in %s (download it from %s/%s)" % (archive_name(host), source_dir, REGISTRY, archive_name(host)), 2)
            shutil.copyfile(source, archive)
        else:
            download(url_base + "/" + archive_name(host), archive, limit, out)
        check_archive(archive, sha256, size, out)
        tree = os.path.join(work, "tree")
        os.mkdir(tree)
        safe_extract(archive, tree, stem)
        os.unlink(archive)
        unpacked = os.path.join(tree, stem)
        if not installed(unpacked):
            raise FetchError("the archive has no bin/arm-none-eabi-gcc: not the toolchain", 3)
        if force and os.path.lexists(final):
            shutil.rmtree(final)
        os.rename(unpacked, final)
        print("fetch_toolchain: installed %s" % final, file=out)
        return final
    finally:
        shutil.rmtree(work, ignore_errors=True)


def run(args, out=None, err=None):
    out = out or sys.stdout
    err = err or sys.stderr
    host = args.host or host_id()
    if host not in PINS:
        print("fetch_toolchain: Arm publishes no %s archive for %s: this host (%s, %s) has none to fetch. Install an arm-none-eabi toolchain 15.3.rel1 "
              "yourself and pass -DARM_GNU_ROOT=<dir that holds bin/> (docs/NEW-APP.md, section 0)." % (VERSION, "this host" if args.host is None else args.host,
              platform.system(), platform.machine()), file=err)
        return 1
    tools_dir = os.path.abspath(args.dir or os.path.join(SDK_ROOT, ".toolchains"))
    sha256, size = PINS[host]
    try:
        final = install(host, tools_dir, sha256, size, out, source_dir=args.from_dir, force=args.force)
    except FetchError as error:
        print("fetch_toolchain: %s" % error, file=err)
        return error.status
    if args.print_root:
        print(final)
    else:
        print("fetch_toolchain: ARM_GNU_ROOT=%s" % final, file=out)
        print("fetch_toolchain: tools/ci.sh and the cross build find it in .toolchains/ without ARM_GNU_ROOT", file=out)
    return 0


# ---- self-test: no network, no real toolchain ---------------------------------------------------------------------------------

def make_archive(path, top, extra=(), links=(), omit_gcc=False, link_out=False, hard_link_out=False, outside_name=None, top_dir_name=None):
    """A small tar.xz that looks like the toolchain: <top>/bin/arm-none-eabi-gcc (an executable script), <top>/lib/x. `extra` are (name, bytes), `links` (name, target) symlinks."""
    with tarfile.open(path, "w:xz") as tar:
        def add_file(name, data, mode=0o644):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = mode
            tar.addfile(info, io.BytesIO(data))

        root = top_dir_name or top
        directory = tarfile.TarInfo(root)
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        tar.addfile(directory)
        if not omit_gcc:
            add_file(root + "/bin/arm-none-eabi-gcc", b"#!/bin/sh\necho arm-none-eabi-gcc 15.3.1\n", 0o755)
        add_file(root + "/lib/x", b"data")
        for name, data in extra:
            add_file(name, data)
        for name, target in links:
            link = tarfile.TarInfo(name)
            link.type = tarfile.SYMTYPE
            link.linkname = target
            tar.addfile(link)
        if link_out:
            link = tarfile.TarInfo(root + "/bin/evil")
            link.type = tarfile.SYMTYPE
            link.linkname = "../../../../etc/passwd"
            tar.addfile(link)
        if hard_link_out:
            link = tarfile.TarInfo(root + "/bin/evil2")
            link.type = tarfile.LNKTYPE
            link.linkname = "../../../../etc/passwd"
            tar.addfile(link)
        if outside_name:
            add_file(outside_name, b"escaped")


def tree_files(root):
    found = []
    for base, _dirs, files in os.walk(root):
        for name in files:
            found.append(os.path.relpath(os.path.join(base, name), root))
    return sorted(found)


class _Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    host = "x86_64-linux"
    stem = archive_stem(host)
    sink = io.StringIO()

    # The pins: every one is 64 hex digits, a plausible size, and the names Arm gives them.
    for key, (digest, size) in PINS.items():
        expect(re.fullmatch(r"[0-9a-f]{64}", digest) is not None and 100 << 20 < size < MAX_BYTES, "pin of %s is well formed" % key)
        expect(archive_name(key).startswith("arm-gnu-toolchain-15.3.rel1-") and archive_name(key).endswith("-arm-none-eabi.tar.xz"), "name of %s" % key)
    expect(archive_name("x86_64-linux") == "arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz", "the x86_64 archive name")
    expect(archive_name("darwin-arm64") == "arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz", "the macOS archive name")
    expect(PINS["x86_64-linux"][0] == "563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa", "the x86_64 pin is Arm's published value")

    # Host detection.
    expect(host_id("Linux", "x86_64") == "x86_64-linux" and host_id("Linux", "aarch64") == "aarch64-linux", "linux hosts")
    expect(host_id("Darwin", "arm64") == "darwin-arm64", "apple silicon")
    expect(host_id("Darwin", "x86_64") is None and host_id("Windows", "AMD64") is None and host_id("Linux", "riscv64") is None, "hosts without an archive")

    # The checksum file's format.
    digest = "ab" * 32
    expect(parse_sha256asc("%s  arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz\n" % digest) == digest, "sha256asc parsed")
    expect(parse_sha256asc("%s *file\n" % digest.upper()) == digest, "binary-mode marker and upper case")
    expect(parse_sha256asc("not a checksum\n") is None and parse_sha256asc("") is None, "sha256asc refused")

    with tempfile.TemporaryDirectory() as tmp:
        downloads = os.path.join(tmp, "downloads")
        os.mkdir(downloads)
        good = os.path.join(downloads, archive_name(host))
        make_archive(good, stem)
        good_sha = sha256_of(good)
        good_size = os.path.getsize(good)

        # A good archive from a download directory: installed, executable, no scratch left behind.
        tools = os.path.join(tmp, "toolchains")
        final = install(host, tools, good_sha, good_size, sink, source_dir=downloads)
        expect(final == os.path.join(tools, stem) and installed(final), "installed under .toolchains/<stem>")
        expect(sorted(os.listdir(tools)) == [stem], "no scratch directory left: %s" % os.listdir(tools))
        expect(tree_files(final) == ["bin/arm-none-eabi-gcc", "lib/x"], "the tree is the archive's")
        expect(os.access(os.path.join(final, "bin", "arm-none-eabi-gcc"), os.X_OK), "the compiler keeps its executable bit")

        # Installed already: nothing is read (the source is gone) unless --force.
        os.unlink(good)
        again = io.StringIO()
        expect(install(host, tools, good_sha, good_size, again, source_dir=downloads) == final and "already installed" in again.getvalue(), "an installed toolchain is left alone")
        make_archive(good, stem, extra=[(stem + "/lib/y", b"new")])
        new_sha, new_size = sha256_of(good), os.path.getsize(good)
        install(host, tools, new_sha, new_size, sink, source_dir=downloads, force=True)
        expect(tree_files(final) == ["bin/arm-none-eabi-gcc", "lib/x", "lib/y"], "--force installs again over the old one")

        # A wrong checksum, a wrong size, a missing file: refused, and .toolchains/ is as it was.
        def refused(label, status, **kwargs):
            before = sorted(os.listdir(tools2))
            try:
                install(host, tools2, kwargs.pop("sha", new_sha), kwargs.pop("size", new_size), sink, **kwargs)
            except FetchError as error:
                expect(error.status == status, "%s: status %d, wanted %d (%s)" % (label, error.status, status, error))
            else:
                failures.append("%s: not refused" % label)
            expect(sorted(os.listdir(tools2)) == before, "%s: nothing left in .toolchains/ (%s)" % (label, os.listdir(tools2)))

        tools2 = os.path.join(tmp, "toolchains2")
        os.mkdir(tools2)
        refused("wrong checksum", 2, sha="0" * 64, source_dir=downloads)
        refused("wrong size", 2, size=new_size + 1, source_dir=downloads)
        refused("no archive in the directory", 2, source_dir=os.path.join(tmp, "nowhere"))

        # An archive that matches its pin but is not safe or not the toolchain: refused with status 3 (the pin only says who made it).
        def hostile(label, **kwargs):
            bad_dir = os.path.join(tmp, "bad-" + label.replace(" ", "-"))
            os.mkdir(bad_dir)
            bad = os.path.join(bad_dir, archive_name(host))
            make_archive(bad, stem, **kwargs)
            refused(label, 3, sha=sha256_of(bad), size=os.path.getsize(bad), source_dir=bad_dir)

        hostile("symlink out", link_out=True)
        hostile("hard link out", hard_link_out=True)
        hostile("member outside the top directory", outside_name="elsewhere.txt")
        hostile("member with dotdot", outside_name=stem + "/../escape.txt")
        hostile("absolute member", outside_name="/tmp/o3sdk-fetch-escape")
        hostile("other top directory", top_dir_name="some-other-dir")
        hostile("no compiler", omit_gcc=True)
        # A link that goes up through a symlinked directory the archive made earlier: lexically it stays inside, on disk it climbs out.
        hostile("link up through a directory link", links=[(stem + "/a", "."), (stem + "/b", "a/../..")])
        hostile("link up through two links", links=[(stem + "/lib/l1", ".."), (stem + "/l2", "lib/l1/..")])
        hostile("link into a link that leaves", links=[(stem + "/lib/up", "../.."), (stem + "/c", "lib/up")])
        hostile("link loop", links=[(stem + "/p", "q"), (stem + "/q", "p"), (stem + "/r", "p")])
        # The links a real toolchain has (sideways, up inside, through a symlinked directory) are accepted.
        ok_dir = os.path.join(tmp, "links-ok")
        os.mkdir(ok_dir)
        ok_archive = os.path.join(ok_dir, archive_name(host))
        make_archive(ok_archive, stem, links=[(stem + "/bin/gcc-alias", "arm-none-eabi-gcc"), (stem + "/bin/up", "../lib/x"), (stem + "/lib2", "lib"),
                                              (stem + "/lib2/y", "x"), (stem + "/lib/self", ".")])
        tools_ok = os.path.join(tmp, "toolchains-links-ok")
        try:
            via_links = install(host, tools_ok, sha256_of(ok_archive), os.path.getsize(ok_archive), sink, source_dir=ok_dir)
            expect(installed(via_links) and os.path.islink(os.path.join(via_links, "bin", "up")) and os.path.exists(os.path.join(via_links, "lib", "y")),
                   "an archive with ordinary links is installed")
        except FetchError as error:
            failures.append("an archive with ordinary links was refused: %s" % error)
        expect(not os.path.exists(os.path.join(tmp, "escape.txt")) and not os.path.exists("/tmp/o3sdk-fetch-escape"), "nothing escaped the scratch directory")
        # A damaged archive.
        bad_dir = os.path.join(tmp, "damaged")
        os.mkdir(bad_dir)
        damaged = os.path.join(bad_dir, archive_name(host))
        with open(good, "rb") as f:
            data = f.read()
        with open(damaged, "wb") as f:
            f.write(data[: len(data) // 2])
        refused("truncated archive", 3, sha=sha256_of(damaged), size=os.path.getsize(damaged), source_dir=bad_dir)
        with open(damaged, "wb") as f:
            f.write(b"this is not an archive")
        refused("not an archive", 3, sha=sha256_of(damaged), size=os.path.getsize(damaged), source_dir=bad_dir)

        # The download path, over a loopback server: the archive arrives and is checked; too large is refused while it streams; a 404 is a failure.
        served = os.path.join(tmp, "served")
        os.mkdir(served)
        shutil.copyfile(good, os.path.join(served, archive_name(host)))
        handler = lambda *a, **k: _Quiet(*a, directory=served, **k)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            base = "http://127.0.0.1:%d" % server.server_address[1]
            tools3 = os.path.join(tmp, "toolchains3")
            via = install(host, tools3, new_sha, new_size, sink, url_base=base)
            expect(installed(via) and sorted(os.listdir(tools3)) == [stem], "downloaded, checked and installed")
            tools4 = os.path.join(tmp, "toolchains4")
            for label, limit, url in (("over the size limit", 100, base), ("not found", MAX_BYTES, base + "/missing")):
                try:
                    install(host, tools4, new_sha, new_size, sink, url_base=url, limit=limit)
                except FetchError as error:
                    expect(error.status == 2, "%s: status 2 (%s)" % (label, error))
                else:
                    failures.append("%s: not refused" % label)
                expect(os.listdir(tools4) == [], "%s: nothing left (%s)" % (label, os.listdir(tools4)))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(5)

        # The command line: a host with no archive is exit 1 and says what to do; --from-dir with the real pin refuses the stand-in archive.
        class A:
            host = "darwin-x86_64"
            dir = os.path.join(tmp, "toolchains5")
            from_dir = None
            force = False
            print_root = False

        out, err = io.StringIO(), io.StringIO()
        expect(run(A, out, err) == 1 and "ARM_GNU_ROOT" in err.getvalue() and not os.path.exists(A.dir), "an unsupported host is exit 1 and creates nothing")
        A.host = host
        A.from_dir = downloads
        out, err = io.StringIO(), io.StringIO()
        expect(run(A, out, err) == 2 and "pinned" in err.getvalue(), "the stand-in archive does not match the real pin")
        expect(not os.path.isdir(os.path.join(A.dir, stem)), "and nothing is installed")

    if failures:
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1
    print("fetch_toolchain self-test: ok")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Download the Arm GNU Toolchain 15.3.Rel1 into .toolchains/, checking its pinned SHA-256.")
    parser.add_argument("--dir", help="the .toolchains directory (default: .toolchains/ at the root of this checkout)")
    parser.add_argument("--host", choices=sorted(PINS) + ["darwin-x86_64"], help="the host to fetch for (default: this one)")
    parser.add_argument("--from-dir", metavar="DIR", help="take the archive from DIR (downloaded by hand) instead of the network; it is checked the same way")
    parser.add_argument("--force", action="store_true", help="install again even if the toolchain is there")
    parser.add_argument("--print-root", action="store_true", help="print only the toolchain directory when done")
    parser.add_argument("--self-test", action="store_true", help="run the checks of this tool (offline) and exit")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
