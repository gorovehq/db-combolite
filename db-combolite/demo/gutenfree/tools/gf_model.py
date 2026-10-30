#!/usr/bin/env python3
"""GutenFree Diet: download the embedding model the demo uses.

    gf_model.py [--file NAME] [--out DIR]

The model is bge-small-en-v1.5 (BAAI, MIT licence): 33M parameters, 384
dimensions, a 512-token window, English. The GGUF conversion comes from
CompendiumLabs/bge-small-en-v1.5-gguf on Hugging Face (MIT). The default file
is the Q8_0 quantisation (about 37 MB, near-lossless); bge-small-en-v1.5-q4_k_m.gguf
(about 25 MB) suits small devices.

The database never downloads models (spec: that is the application's job);
GutenFree Diet is the application. The file's SHA-256 is checked against the
one Hugging Face publishes for it before the file is kept.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request

REPO = "CompendiumLabs/bge-small-en-v1.5-gguf"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "..", "datastore", "gutenberg", "models"))
USER_AGENT = "GutenFreeDiet/0.1 (db-combolite demo)"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    return urllib.request.urlopen(req, timeout=120)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--file", default="bge-small-en-v1.5-q8_0.gguf")
    ap.add_argument("--out", default=OUT)
    a = ap.parse_args()

    with get(f"https://huggingface.co/api/models/{REPO}/tree/main") as r:
        tree = json.load(r)
    entry = next((e for e in tree if e.get("path") == a.file), None)
    if entry is None or "lfs" not in entry:
        sys.exit(f"gf_model: {a.file} is not in {REPO}")
    want, size = entry["lfs"]["oid"], entry["lfs"]["size"]

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, a.file)
    if os.path.exists(path):
        with open(path, "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() == want:
                print(f"{path}: already here, SHA-256 matches")
                return
    print(f"downloading {a.file} ({size} bytes), expecting SHA-256 {want}")
    h = hashlib.sha256()
    tmp = path + ".part"
    with get(f"https://huggingface.co/{REPO}/resolve/main/{a.file}") as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            f.write(chunk)
    if h.hexdigest() != want:
        os.remove(tmp)
        sys.exit(f"gf_model: SHA-256 mismatch: got {h.hexdigest()}")
    os.replace(tmp, path)
    print(f"{path}: SHA-256 verified")


if __name__ == "__main__":
    main()
