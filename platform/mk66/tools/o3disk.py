#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""o3disk: the PC side of the USB disk scenarios (OS-USBDISK U1d; stdlib only; LINUX ONLY: it reads /sys and /proc/mounts and calls udisksctl).

Windows and macOS are UNTESTED: o3hil.py skips the scenarios that need the disk there ("not run, not failed"), it never tries.

THE RULE (CLAUDE.md, hard rules; maintainer 2026-10-17: any SD card may be written, no marker file and no volume check are required): this tool reads and writes ONLY the unit's own USB disk, whatever
SD card is in the unit. Before anything is read from the card or written to it, `Session.target()` proves all of:
  1. the block device is the unit's USB disk: a /sys/block/sd* whose USB ancestor is 16d0:0e9f with bcdDevice 0002 (the Disk descriptor set; STANDARD and AUDIO have other values and no storage);
  2. the computer sees it as a removable disk (sysfs removable=1, the unit's INQUIRY says so) and the volume is not mounted at "/": a fixed or system disk is refused;
  3. IF the caller passed --expect-volume (optional), the file system's volume serial is that one (found as /dev/disk/by-uuid/XXXX-XXXX pointing at that block device or one of its partitions).
Nothing is inferred from a drive letter or from /dev/sdX alone. The ONLY writes are under `<mount>/o3-usbdisk-test/` (a path outside it is refused in code), and they need the disk to be writable
(sysfs ro=0, the mount rw). The tool never opens the raw block device and never formats, partitions or fscks.

    o3disk.py --self-test       against a fake /sys, a fake /proc/mounts and a temporary directory standing for the mount
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

UNIT_VID = "16d0"
UNIT_PID = "0e9f"
DISK_BCD = "0002"
TEST_DIR = "o3-usbdisk-test"
CHUNK = 1 << 20


class DiskError(Exception):
    pass


def _read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def find_unit_disks(sys_root="/sys"):
    """[{name, dev, vendor, model, ro, blocks, usb_serial}] for every sd* block device under the unit's Disk set."""
    out = []
    block = os.path.join(sys_root, "block")
    if not os.path.isdir(block):
        return out
    for name in sorted(os.listdir(block)):
        if not name.startswith("sd"):
            continue
        node = os.path.realpath(os.path.join(block, name))
        usb = node
        while usb and usb != os.path.dirname(usb) and not os.path.isfile(os.path.join(usb, "idVendor")):
            usb = os.path.dirname(usb)
        if not os.path.isfile(os.path.join(usb, "idVendor")):
            continue
        if _read(os.path.join(usb, "idVendor")) != UNIT_VID or _read(os.path.join(usb, "idProduct")) != UNIT_PID or _read(os.path.join(usb, "bcdDevice")) != DISK_BCD:
            continue
        out.append({
            "name": name,
            "dev": "/dev/" + name,
            "vendor": _read(os.path.join(node, "device", "vendor"), "").strip(),
            "model": _read(os.path.join(node, "device", "model"), "").strip(),
            "ro": _read(os.path.join(node, "ro"), "?"),
            "removable": _read(os.path.join(node, "removable"), "?"),
            "blocks": int(_read(os.path.join(node, "size"), "0") or 0),
            "usb_serial": _read(os.path.join(usb, "serial"), ""),
        })
    return out


def _unescape(field):
    return re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), field)


def mounts_of(name, proc_mounts="/proc/mounts"):
    """[(mount_point, options_list)] of /dev/<name> and its partitions (/dev/<name>1, <name>p1)."""
    text = _read(proc_mounts, "") or ""
    pattern = re.compile(r"^/dev/%s(p?\d+)?$" % re.escape(name))
    found = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 4 and pattern.match(fields[0]):
            found.append((_unescape(fields[1]), fields[3].split(",")))
    return found


def volume_of(name, by_uuid="/dev/disk/by-uuid"):
    """The FAT volume serials (XXXX-XXXX, upper case) whose device is /dev/<name> or one of its partitions."""
    out = []
    if not os.path.isdir(by_uuid):
        return out
    pattern = re.compile(r"^%s(p?\d+)?$" % re.escape(name))
    for entry in os.listdir(by_uuid):
        target = os.path.basename(os.path.realpath(os.path.join(by_uuid, entry)))
        if pattern.match(target):
            out.append(entry.upper())
    return out


def stream(seed, size):
    """The deterministic test content: `size` bytes of a SHA-256 counter stream of `seed` (reproducible, incompressible), as chunks."""
    counter = 0
    left = size
    while left > 0:
        block = b"".join(hashlib.sha256(("%s:%d:%d" % (seed, counter, i)).encode()).digest() for i in range(CHUNK // 32))
        counter += 1
        yield block[:left]
        left -= min(left, CHUNK)


def sha256_file(path, drop_cache=True):
    """SHA-256 of a file read from its start; with `drop_cache` the page cache of the file is dropped first (posix_fadvise DONTNEED), so that the bytes come off the medium."""
    h = hashlib.sha256()
    total = 0
    with open(path, "rb", buffering=0) as f:
        if drop_cache and hasattr(os, "posix_fadvise"):
            os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            h.update(chunk)
            total += len(chunk)
    return h.hexdigest(), total


class Session:
    """One disk under test. `log` prints a line; `sys_root`, `proc_mounts`, `by_uuid` and `run` are seams for the self-test."""

    def __init__(self, expect_volume=None, log=print, sys_root="/sys", proc_mounts="/proc/mounts", by_uuid="/dev/disk/by-uuid", run=subprocess.run, scratch=None):
        self.expect_volume = (expect_volume or "").upper()
        self.log = log
        self.sys_root = sys_root
        self.proc_mounts = proc_mounts
        self.by_uuid = by_uuid
        self.run = run
        self.scratch = scratch
        self.disk = None
        self.mount = None

    # -- the unit's disk
    def enumerate(self, within=30.0, ro=None):
        """Waits for the unit's Disk set to show a block device with a medium; returns its record. `ro` True / False checks the write-protect state the computer sees."""
        deadline = time.monotonic() + within
        while True:
            disks = [d for d in find_unit_disks(self.sys_root) if d["blocks"] > 0]
            if len(disks) == 1:
                break
            if len(disks) > 1:
                raise DiskError("more than one unit disk (%s): refusing to guess" % ", ".join(d["name"] for d in disks))
            if time.monotonic() >= deadline:
                raise DiskError("no block device of the unit's Disk set (USB %s:%s bcdDevice %s) with a medium within %.0f s" % (UNIT_VID, UNIT_PID, DISK_BCD, within))
            time.sleep(0.5)
        self.disk = disks[0]
        d = self.disk
        self.log("    disk %s: vendor %r model %r, %d blocks (%.1f MiB), ro=%s" % (d["name"], d["vendor"], d["model"], d["blocks"], d["blocks"] / 2048.0, d["ro"]))
        if ro is not None and (d["ro"] == "1") != bool(ro):
            raise DiskError("the computer sees ro=%s, %s expected" % (d["ro"], "1" if ro else "0"))
        return d

    def refresh_disk(self):
        for d in find_unit_disks(self.sys_root):
            if self.disk and d["name"] == self.disk["name"]:
                self.disk = d
                return d
        raise DiskError("the unit's disk is gone")

    # -- the identification, before any file is touched
    def check_disk(self, what):
        """The unit's disk is a removable one, and (when --expect-volume was given) has that volume serial; raises DiskError otherwise. `what` is the verb of the refusal."""
        if self.disk is None:
            raise DiskError("no disk: an enumerate step comes first")
        self.refresh_disk()  # re-prove it is still the unit's disk (VID/PID), not another disk that took its sdX name
        if self.disk.get("removable") != "1":
            raise DiskError("the unit's disk %s is not removable (removable=%s): a fixed or system disk is never touched, nothing was %s" % (self.disk["name"], self.disk.get("removable"), what))
        if self.expect_volume:
            volumes = volume_of(self.disk["name"], self.by_uuid)
            if self.expect_volume not in volumes:
                raise DiskError("the unit's disk %s has volume %s, not %s: nothing was %s" % (self.disk["name"], "/".join(volumes) or "(none)", self.expect_volume, what))

    def target(self):
        """The mount point of the unit's disk, proven to be the unit's removable disk (and the --expect-volume one, when given); raises DiskError otherwise."""
        self.check_disk("done")
        mounts = mounts_of(self.disk["name"], self.proc_mounts)
        if not mounts:
            raise DiskError("the disk %s is not mounted" % self.disk["name"])
        mount, options = mounts[0]
        if os.path.realpath(mount) == "/":
            raise DiskError("the disk %s is mounted at /: that is a system disk, nothing was done" % self.disk["name"])
        self.mount = (mount, options)
        return mount

    def mount_card(self, within=30.0):
        """Mounts the disk (udisksctl, no root) when it is not mounted, then proves it with target()."""
        if self.disk is None:
            raise DiskError("no disk: an enumerate step comes first")
        deadline = time.monotonic() + within
        tried = False
        while True:
            if mounts_of(self.disk["name"], self.proc_mounts):
                return self.target()
            if not tried:
                tried = True
                self.check_disk("mounted")  # (the removable check, and the volume serial when one was named, before a read-write mount)
                node = self.partition_node()
                if shutil.which("udisksctl") is None:
                    raise DiskError("%s is not mounted and udisksctl is not here: mount it (the file manager) and run again" % node)
                self.run(["udisksctl", "mount", "-b", node], capture_output=True, text=True)
            if time.monotonic() >= deadline:
                raise DiskError("the disk %s did not mount within %.0f s" % (self.disk["name"], within))
            time.sleep(0.5)

    def partition_node(self):
        name = self.disk["name"]
        for suffix in ("1", "p1"):
            if os.path.exists(os.path.join(self.sys_root, "block", name, name + suffix)):
                return "/dev/" + name + suffix
        return "/dev/" + name

    def unmount(self):
        if self.mount is None:
            raise DiskError("not mounted by this session")
        self.run(["sync"], check=False)
        for _ in range(10):
            r = self.run(["udisksctl", "unmount", "-b", self.partition_node()], capture_output=True, text=True)
            if r.returncode == 0 or not mounts_of(self.disk["name"], self.proc_mounts):
                self.mount = None
                return
            time.sleep(0.5)
        raise DiskError("the card did not unmount: %s" % (getattr(r, "stderr", "") or "").strip()[:200])

    def remount(self, within=30.0):
        """Unmount and mount again: the computer's cache of the volume is dropped, the unit sees no eject (the same SCSI session)."""
        self.unmount()
        return self.mount_card(within)

    def eject(self, within=20.0):
        """The computer ejects the medium (udisksctl power-off sends START STOP UNIT): the block device is gone or reads 0 blocks."""
        if self.mount is not None:
            self.unmount()
        r = self.run(["udisksctl", "power-off", "-b", "/dev/" + self.disk["name"]], capture_output=True, text=True)
        deadline = time.monotonic() + within
        while True:
            now = [d for d in find_unit_disks(self.sys_root) if d["name"] == self.disk["name"]]
            if not now or now[0]["blocks"] == 0:
                return
            if time.monotonic() >= deadline:
                raise DiskError("the disk %s is still there with %d blocks %.0f s after the eject (udisksctl: %s)" % (self.disk["name"], now[0]["blocks"], within, (getattr(r, "stderr", "") or "").strip()[:160]))
            time.sleep(0.5)

    # -- reading
    def read_check(self, size_mb):
        """Copies files off the card (at most `size_mb` MiB in all, the largest first) to a scratch directory and checks every copy against a second, cache-dropped read of its source."""
        mount = self.target()
        files = []
        for root, dirs, names in os.walk(mount):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d != "System Volume Information"]
            for n in names:
                if not n.startswith("."):
                    p = os.path.join(root, n)
                    try:
                        files.append((os.path.getsize(p), p))
                    except OSError:
                        pass
        files.sort(reverse=True)
        if not files:
            raise DiskError("the card has no file to read")
        chosen, total = [], 0
        for size, p in files:
            if size == 0:
                continue
            if total and total + size > size_mb * (1 << 20):
                continue
            chosen.append((size, p))
            total += size
            if total >= size_mb * (1 << 20):
                break
        scratch = self.scratch or tempfile.mkdtemp(prefix="o3disk-")
        started = time.monotonic()
        try:
            for size, p in chosen:
                dst = os.path.join(scratch, "copy.bin")
                h = hashlib.sha256()
                with open(p, "rb", buffering=0) as src, open(dst, "wb") as out:
                    if hasattr(os, "posix_fadvise"):
                        os.posix_fadvise(src.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
                    while True:
                        chunk = src.read(CHUNK)
                        if not chunk:
                            break
                        h.update(chunk)
                        out.write(chunk)
                copy_sum, copy_size = sha256_file(dst, drop_cache=False)
                again_sum, again_size = sha256_file(p)
                if copy_sum != h.hexdigest() or copy_sum != again_sum or copy_size != size or again_size != size:
                    raise DiskError("%s: the copy and two reads of the card disagree (%d bytes, %s / %s / %s)" % (p, size, h.hexdigest()[:12], copy_sum[:12], again_sum[:12]))
        finally:
            if self.scratch is None:
                shutil.rmtree(scratch, ignore_errors=True)
        seconds = max(time.monotonic() - started, 1e-6)
        self.log("    read %d file(s), %.1f MiB off the card, every copy matches two reads of the card: %.2f MiB/s" % (len(chosen), total / float(1 << 20), total / float(1 << 20) / seconds))
        return total

    # -- writing (the only place the tool writes)
    def writable(self):
        """The computer sees a writable disk (sysfs ro=0) and the volume is mounted rw."""
        d = self.refresh_disk()
        mount = self.target()
        return d["ro"] == "0" and "rw" in self.mount[1] and os.path.isdir(mount)

    def test_path(self, name):
        mount = self.target()
        base = os.path.realpath(os.path.join(mount, TEST_DIR))
        path = os.path.realpath(os.path.join(base, name))
        if os.path.dirname(path) != base or not re.fullmatch(r"o3-usbdisk-[A-Za-z0-9_.-]+", name):
            raise DiskError("refusing to write %r: only %s/o3-usbdisk-* on the card" % (name, TEST_DIR))
        return base, path

    def write_roundtrip(self, size_mb, seed="o3usbdisk"):
        """Writes a deterministic file of `size_mb` MiB into <mount>/o3-usbdisk-test/, syncs, remounts (the cache is gone), reads it back and compares its checksum; then deletes it."""
        if not self.writable():
            raise DiskError("the disk is not writable (ro=%s, mount options %s): the write step needs the writable build" % (self.disk["ro"], ",".join(self.mount[1])))
        name = "o3-usbdisk-%dm.bin" % size_mb
        base, path = self.test_path(name)
        os.makedirs(base, exist_ok=True)
        want = hashlib.sha256()
        started = time.monotonic()
        with open(path, "wb") as f:
            for chunk in stream(seed, size_mb << 20):
                want.update(chunk)
                f.write(chunk)
            f.flush()
            os.fsync(f.fileno())
        wrote = time.monotonic() - started
        self.log("    wrote %d MiB to the card in %.1f s (%.2f MiB/s), sha256 %s" % (size_mb, wrote, size_mb / max(wrote, 1e-6), want.hexdigest()[:16]))
        self.remount()
        base, path = self.test_path(name)
        started = time.monotonic()
        got, size = sha256_file(path)
        read = time.monotonic() - started
        if size != size_mb << 20 or got != want.hexdigest():
            raise DiskError("%s read back differs: %d bytes sha256 %s, wrote %d bytes %s" % (name, size, got[:16], size_mb << 20, want.hexdigest()[:16]))
        self.log("    read it back after a remount: %.2f MiB/s, the checksum matches" % (size_mb / max(read, 1e-6)))
        os.remove(path)
        try:
            os.rmdir(base)
        except OSError:
            pass
        return size


# ---- self-test ----------------------------------------------------------------------------------------------------------------------

def self_test():
    problems = []

    class Done:
        def __init__(self, code=0, err=""):
            self.returncode = code
            self.stderr = err

    with tempfile.TemporaryDirectory() as tmp:
        sysr = os.path.join(tmp, "sys")
        node = os.path.join(sysr, "devices", "pci0", "usb1", "1-2", "1-2:1.2", "host6", "target6:0:0", "6:0:0:0", "block", "sdz")
        os.makedirs(node)
        os.makedirs(os.path.join(node, "sdz1"))
        usb = os.path.join(sysr, "devices", "pci0", "usb1", "1-2")
        for k, v in (("idVendor", "16d0"), ("idProduct", "0e9f"), ("bcdDevice", "0002"), ("serial", "ABC123")):
            with open(os.path.join(usb, k), "w") as f:
                f.write(v + "\n")
        os.makedirs(os.path.join(sysr, "devices", "pci0", "usb1", "1-2", "1-2:1.2", "host6", "target6:0:0", "6:0:0:0"), exist_ok=True)
        dev = os.path.join(sysr, "devices", "pci0", "usb1", "1-2", "1-2:1.2", "host6", "target6:0:0", "6:0:0:0")
        for k, v in (("vendor", "Track3r "), ("model", "SD CARD        ")):
            with open(os.path.join(dev, k), "w") as f:
                f.write(v + "\n")
        os.symlink(dev, os.path.join(node, "device"))  # (the real /sys/block/sdz/device is a link to the SCSI device)
        for k, v in (("ro", "0"), ("size", "4096"), ("removable", "1")):
            with open(os.path.join(node, k), "w") as f:
                f.write(v + "\n")
        os.makedirs(os.path.join(sysr, "block"))
        os.symlink(node, os.path.join(sysr, "block", "sdz"))
        # a second, foreign disk (another USB device, same names): never matched
        other = os.path.join(sysr, "devices", "pci0", "usb1", "1-3", "x", "block", "sdy")
        os.makedirs(other)
        for k, v in (("idVendor", "0781"), ("idProduct", "5567"), ("bcdDevice", "0100")):
            with open(os.path.join(sysr, "devices", "pci0", "usb1", "1-3", k), "w") as f:
                f.write(v + "\n")
        for k, v in (("ro", "0"), ("size", "999")):
            with open(os.path.join(other, k), "w") as f:
                f.write(v + "\n")
        os.symlink(other, os.path.join(sysr, "block", "sdy"))
        disks = find_unit_disks(sysr)
        if [d["name"] for d in disks] != ["sdz"] or disks[0]["vendor"] != "Track3r" or disks[0]["blocks"] != 4096 or disks[0]["ro"] != "0" or disks[0]["removable"] != "1" or disks[0]["usb_serial"] != "ABC123":
            problems.append("find_unit_disks: %r" % (disks,))
        # the same IDs with another bcdDevice (STANDARD / AUDIO sets) are not the disk
        with open(os.path.join(usb, "bcdDevice"), "w") as f:
            f.write("0001\n")
        if find_unit_disks(sysr):
            problems.append("a device with another bcdDevice was taken for the Disk set")
        with open(os.path.join(usb, "bcdDevice"), "w") as f:
            f.write("0002\n")

        mount = os.path.join(tmp, "mnt")
        os.makedirs(mount)
        proc = os.path.join(tmp, "mounts")
        with open(proc, "w") as f:
            f.write("/dev/sda1 / ext4 rw 0 0\n/dev/sdz1 %s vfat rw,nosuid 0 0\n/dev/sdzz1 /other vfat rw 0 0\n" % mount.replace(" ", "\\040"))
        if mounts_of("sdz", proc) != [(mount, ["rw", "nosuid"])]:
            problems.append("mounts_of: %r" % (mounts_of("sdz", proc),))
        if mounts_of("sdy", proc):
            problems.append("mounts_of took another disk")
        byuuid = os.path.join(tmp, "by-uuid")
        os.makedirs(byuuid)
        os.symlink("../../sdz1", os.path.join(byuuid, "ABCD-1234"))
        os.symlink("../../sda1", os.path.join(byuuid, "1111-2222"))
        if volume_of("sdz", byuuid) != ["ABCD-1234"]:
            problems.append("volume_of: %r" % (volume_of("sdz", byuuid),))

        lines = []
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            return Done()

        s = Session("ABCD-1234", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run, scratch=os.path.join(tmp, "scratch"))
        os.makedirs(s.scratch)
        # no marker file is needed (OS-ANYCARD): the card below has none, and the unit's removable disk is accepted
        s.enumerate(within=0.2)
        if os.path.exists(os.path.join(mount, ".o3-test-card")):
            problems.append("the fake card carries a marker: the self-test no longer shows that none is needed")
        try:
            if s.target() != mount:
                problems.append("target() is not the mount")
        except DiskError as error:
            problems.append("target() refused a card with no marker: %s" % error)
        wrong = Session("1111-2222", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run)
        wrong.enumerate(within=0.2)
        try:
            wrong.target()
            problems.append("a card of another volume than --expect-volume was accepted")
        except DiskError:
            pass
        nameless = Session("", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run)  # --expect-volume is optional
        nameless.enumerate(within=0.2)
        try:
            if nameless.target() != mount:
                problems.append("a session without --expect-volume found another mount")
        except DiskError as error:
            problems.append("a session without --expect-volume was refused: %s" % error)
        # a fixed disk (removable=0) is refused, with or without a volume named; so is a mount at /
        with open(os.path.join(node, "removable"), "w") as f:
            f.write("0\n")
        for sess in (Session("", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run), Session("ABCD-1234", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run)):
            sess.enumerate(within=0.2)
            try:
                sess.target()
                problems.append("a non-removable disk was accepted")
            except DiskError as error:
                if "not removable" not in str(error):
                    problems.append("wrong refusal for a non-removable disk: %s" % error)
            try:
                sess.mount_card(within=0.2)
                problems.append("a non-removable disk was mounted")
            except DiskError:
                pass
        with open(os.path.join(node, "removable"), "w") as f:
            f.write("1\n")
        sysmount = os.path.join(tmp, "mounts-root")
        with open(sysmount, "w") as f:
            f.write("/dev/sdz1 / vfat rw 0 0\n")
        rooted = Session("", log=lines.append, sys_root=sysr, proc_mounts=sysmount, by_uuid=byuuid, run=run)
        rooted.enumerate(within=0.2)
        try:
            rooted.target()
            problems.append("a disk mounted at / was accepted")
        except DiskError:
            pass
        for ro, ok in ((False, True), (True, False)):
            try:
                s.enumerate(within=0.2, ro=ro)
                if not ok:
                    problems.append("ro=%s accepted on a ro=0 disk" % ro)
            except DiskError:
                if ok:
                    problems.append("ro=%s refused on a ro=0 disk" % ro)
        # a file to read
        with open(os.path.join(mount, "A.BIN"), "wb") as f:
            f.write(b"".join(c for c in stream("t", 3 << 20)))
        if s.read_check(8) != 3 << 20:
            problems.append("read_check read the wrong amount")
        # the write guard
        try:
            s.test_path("../escape.bin")
            problems.append("a path outside the test directory was accepted")
        except DiskError:
            pass
        try:
            s.test_path("notmine.bin")
            problems.append("a name outside o3-usbdisk-* was accepted")
        except DiskError:
            pass
        # a round trip over the fake mount (the remount is a no-op: the run seam answers 0, the mount table is unchanged)
        s.mount_card = lambda within=30.0: s.target()
        s.unmount = lambda: None
        if s.write_roundtrip(2) != 2 << 20:
            problems.append("write_roundtrip")
        if os.path.exists(os.path.join(mount, TEST_DIR, "o3-usbdisk-2m.bin")):
            problems.append("the test file was left on the card")
        # a read-only disk refuses the write
        with open(os.path.join(node, "ro"), "w") as f:
            f.write("1\n")
        try:
            s.write_roundtrip(1)
            problems.append("a write went to a read-only disk")
        except DiskError as error:
            if "not writable" not in str(error):
                problems.append("wrong refusal for a read-only disk: %s" % error)
        # two unit disks: refuse to guess
        two = os.path.join(sysr, "devices", "pci0", "usb1", "1-2", "second", "block", "sdx")
        os.makedirs(two)
        with open(os.path.join(two, "size"), "w") as f:
            f.write("10\n")
        os.symlink(two, os.path.join(sysr, "block", "sdx"))
        try:
            Session("ABCD-1234", log=lines.append, sys_root=sysr, proc_mounts=proc, by_uuid=byuuid, run=run).enumerate(within=0.2)
            problems.append("two unit disks were not refused")
        except DiskError as error:
            if "more than one" not in str(error):
                problems.append("wrong error for two disks: %s" % error)
    if problems:
        print("o3disk self-test FAILED:\n  " + "\n  ".join(problems), file=sys.stderr)
        return 1
    print("o3disk self-test passed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--self-test":
        sys.exit(self_test())
    print(__doc__)
    sys.exit(2)
