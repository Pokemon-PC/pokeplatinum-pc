#!/usr/bin/env python3
"""Fetches the subprojects the PC build compiles, exactly as the DS build
(meson) would: the public repositories and revisions named in
subprojects/*.wrap, plus yyjson with the repository's patches.

    python tools/pc_port/fetch_subprojects.py

Nothing proprietary is downloaded: NitroSDK/NitroSystem/NitroDWC/NitroWiFi/
libvct are the decompiled sources published by the ntrtwl project, metang is
a Python script, yyjson is MIT-licensed. The Metrowerks compiler (metroskrew)
is NOT fetched and is not needed by the PC build.
"""
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SUB = os.path.join(ROOT, "subprojects")


def wrap(name):
    text = open(os.path.join(SUB, name + ".wrap"), encoding="utf-8").read()
    return dict(re.findall(r"^(\w+)\s*=\s*(.+?)\s*$", text, re.M))


def git_fetch(name):
    w = wrap(name)
    dest = os.path.join(SUB, w.get("directory", name))
    if os.path.isdir(dest):
        print("%s: present" % os.path.basename(dest))
        return
    url, rev = w["url"], w["revision"]
    print("%s: fetching %s @ %s" % (os.path.basename(dest), url, rev))
    if re.fullmatch(r"[0-9a-f]{40}", rev):
        subprocess.check_call(["git", "init", "-q", dest])
        subprocess.check_call(["git", "-C", dest, "fetch", "-q", "--depth", "1", url, rev])
        subprocess.check_call(["git", "-C", dest, "checkout", "-q", "FETCH_HEAD"])
    else:
        subprocess.check_call(["git", "clone", "-q", "--depth", "1", "-b", rev, url, dest])


def yyjson():
    w = wrap("yyjson")
    dest = os.path.join(SUB, w["directory"])
    if os.path.isdir(dest):
        print("%s: present" % w["directory"])
        return
    print("%s: fetching %s" % (w["directory"], w["source_url"]))
    data = urllib.request.urlopen(w["source_url"]).read()
    if hashlib.sha256(data).hexdigest() != w["source_hash"]:
        sys.exit("yyjson: unexpected archive hash")
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        t.extractall(SUB)
    for patch in [p.strip() for p in w.get("diff_files", "").split(",") if p.strip()]:
        # The always-native patch only touches meson.build (not used here).
        subprocess.run(["git", "apply", "--unsafe-paths", "--directory", os.path.relpath(dest, ROOT).replace(os.sep, "/"),
                        os.path.join(SUB, "packagefiles", patch)], cwd=ROOT, check=False)


def main():
    for name in ("NitroSDK", "NitroSystem", "NitroDWC", "NitroWiFi", "libvct", "metang"):
        git_fetch(name)
    yyjson()


if __name__ == "__main__":
    main()
