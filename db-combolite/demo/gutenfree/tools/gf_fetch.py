#!/usr/bin/env python3
"""GutenFree Diet: fetch a seeded random sample of books from a Gutenberg mirror.

    gf_fetch.py --mirror URL [--count N] [--seed S] [--lang L] [--pause SEC]
    gf_fetch.py --mirror URL --ebook N [--ebook M ...]   specific books
    gf_fetch.py --mirror URL --catalog                   pg_catalog.csv (subjects,
                                                         bookshelves), for genres

Never fetches from www.gutenberg.org: the mirror is required, taken from
MIRRORS.ALL. Two that were checked to use the layout below:

    https://gutenberg.pglaf.org/        Project Gutenberg's own San Diego mirror
    https://mirror.cs.odu.edu/gutenberg/  Old Dominion University (fallback)

Mirror layout: every digit of the ebook number except the last becomes a
directory, then the number itself (#1342 -> 1/3/4/1342/; a one-digit
number -> 0/N/). The text is N-0.txt (UTF-8), else N.txt (ASCII), else
N-8.txt (Latin-1).

Ebook numbers come from GUTINDEX.ALL. The sample is the first --count books
of the candidates shuffled by --seed that the mirror has as text (audio-only
entries have none), so the same seed and mirror give the same books. The
text outside the "*** START OF" / "*** END OF" markers is stripped, and each
book is written as UTF-8 to <out>/<N>.txt, with a line per book in
<out>/manifest.tsv. Books already present are skipped, not fetched again.
"""
import argparse
import os
import random
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "..", "..", "datastore", "gutenberg"))
USER_AGENT = "GutenFreeDiet/0.1 (db-combolite demo; seeded sample, paced)"

# "Title, by Author<spaces>79873": an entry's first line ends in its number.
ENTRY = re.compile(r"^(\S.*?)\s{2,}(\d+)C?\s*$")
LANGUAGE = re.compile(r"\[Language:\s*([^\]]+)\]")
START = re.compile(r"^\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG", re.I | re.M)
END = re.compile(r"^\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", re.I | re.M)
# Older books also close with a plain line just above the END marker.
TRAILER = re.compile(r"\n\s*End of (the )?Project Gutenberg.*\s*$", re.I | re.S)


def parse_index(path):
    """Yields (ebook number, language) for every entry in GUTINDEX.ALL."""
    number, lang = None, "English"
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = ENTRY.match(line)
            if m and not line.startswith("TITLE and AUTHOR"):
                if number is not None:
                    yield number, lang
                number, lang = int(m.group(2)), "English"
                continue
            m = LANGUAGE.search(line)
            if m and number is not None:
                lang = m.group(1).strip()
    if number is not None:
        yield number, lang


def book_dir(mirror, n):
    digits = str(n)
    parts = list(digits[:-1]) if len(digits) > 1 else ["0"]
    return mirror.rstrip("/") + "/" + "/".join(parts) + "/" + digits + "/"


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def get_text(mirror, n, pause):
    """Returns (url, text) for book n, or (None, reason)."""
    base = book_dir(mirror, n)
    for name, enc in ((f"{n}-0.txt", "utf-8"), (f"{n}.txt", "utf-8"), (f"{n}-8.txt", "latin-1")):
        url = base + name
        try:
            raw = fetch(url)
        except urllib.error.HTTPError as e:
            if e.code == 404:
                time.sleep(pause / 4)
                continue
            return None, f"HTTP {e.code}"
        except (urllib.error.URLError, TimeoutError) as e:
            return None, f"network: {e}"
        try:
            return url, raw.decode(enc)
        except UnicodeDecodeError:
            return url, raw.decode("latin-1")
    return None, "no text file"


def strip_markers(text):
    s, e = START.search(text), END.search(text)
    if not s or not e or e.start() <= s.end():
        return None
    body = text[text.index("\n", s.end()) + 1 : e.start()]
    body = TRAILER.sub("", body)
    return body.strip("\r\n") + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mirror", required=True, help="mirror base URL, from MIRRORS.ALL")
    ap.add_argument("--count", type=int, default=25, help="books to fetch (default 25)")
    ap.add_argument("--seed", type=int, default=1, help="sample seed (default 1)")
    ap.add_argument("--lang", default="English", help="language, or 'any' (default English)")
    ap.add_argument("--pause", type=float, default=2.0, help="seconds between books (default 2)")
    ap.add_argument("--ebook", type=int, action="append", help="fetch this ebook number (repeatable)")
    ap.add_argument("--catalog", action="store_true", help="fetch cache/epub/feeds/pg_catalog.csv instead")
    ap.add_argument("--index", default=os.path.join(DATA, "GUTINDEX", "GUTINDEX.ALL.new"))
    ap.add_argument("--out", default=os.path.join(DATA, "books"))
    a = ap.parse_args()

    if "www.gutenberg.org" in a.mirror or "//gutenberg.org" in a.mirror:
        sys.exit("gf_fetch: use a mirror, not www.gutenberg.org")

    if a.catalog:
        url = a.mirror.rstrip("/") + "/cache/epub/feeds/pg_catalog.csv"
        path = os.path.join(DATA, "pg_catalog.csv")
        data = fetch(url)
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
        print(f"{path}: {len(data)} bytes from {url}")
        return
    if a.ebook:
        cands, a.count = a.ebook, len(a.ebook)
    else:
        entries = list(parse_index(a.index))
        cands = sorted({n for n, lang in entries if a.lang == "any" or lang == a.lang})
        print(f"index: {len(entries)} entries, {len(cands)} in {a.lang}")
        random.Random(a.seed).shuffle(cands)

    os.makedirs(a.out, exist_ok=True)
    manifest = os.path.join(a.out, "manifest.tsv")
    have, tried = 0, 0
    for n in cands:
        if have == a.count:
            break
        path = os.path.join(a.out, f"{n}.txt")
        if os.path.exists(path):
            have += 1
            print(f"[{have}/{a.count}] #{n}: already here")
            continue
        tried += 1
        url, text = get_text(a.mirror, n, a.pause)
        if url is None:
            print(f"  skip #{n}: {text}")
            time.sleep(a.pause)
            continue
        body = strip_markers(text)
        if body is None:
            print(f"  skip #{n}: no START/END markers")
            time.sleep(a.pause)
            continue
        with open(path, "w", encoding="utf-8") as f:
            f.write(body)
        with open(manifest, "a", encoding="utf-8") as f:
            f.write(f"{n}\t{url}\t{len(body.encode('utf-8'))}\n")
        have += 1
        print(f"[{have}/{a.count}] #{n}: {len(body.encode('utf-8'))} bytes from {url}")
        time.sleep(a.pause)
    print(f"done: {have} books in {a.out} ({tried} fetched this run)")
    if a.ebook and have < len(a.ebook):
        sys.exit(1)


if __name__ == "__main__":
    main()
