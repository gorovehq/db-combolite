#!/usr/bin/env python3
"""GutenFree Diet: fetch a seeded random sample of books from a Gutenberg mirror.

    python3 gf_fetch.py --gutindex          GUTINDEX.ALL, the catalog index (first)
    python3 gf_fetch.py --catalog           pg_catalog.csv: genres, subjects, dates
    python3 gf_fetch.py [--count N] [--seed S] [--lang L] [--jobs J] [--new] [--stream]
                                            a seeded random sample of books
    python3 gf_fetch.py --ebook N [--ebook M ...]   specific books
    (each also takes --mirror URL)

Everything lands in data/ next to this script. Never fetches from
www.gutenberg.org: --mirror is a mirror from MIRRORS.ALL, by default
Project Gutenberg's own pglaf.org mirror. Two that were checked to use the
layout below:

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
book is written as UTF-8 to <out>/<Title>.txt: its pg_catalog.csv title
(else its GUTINDEX one) in characters no shell needs escaped: accents
folded to plain letters, spaces as underscores, only ASCII letters, digits,
_ - and . kept, at most 150 characters, never starting with - or .; a
title another book's file already has gets _<N>. A line per book in
<out>/manifest.tsv gives "N<TAB>url<TAB>bytes<TAB>file". --rename-existing
renames every listed book to the name its title gives now. Books already present are skipped, not fetched again;
with --new they don't count either, so --count new books arrive (a test
run then never sees only data it has stored before).

--jobs downloads that many books at once (default 8). There is no pause
between books (--pause adds one); a mirror that answers 429 or 503 is
waited out (its Retry-After, else doubling from 2 s, five tries), and a
network failure retries the book on the fallback mirror. --stream prints
"BOOK <n>" on its own line, flushed, the moment a book is on disk, for a
reader that ingests as books arrive (gutenfree pipeline).
"""
import argparse
import concurrent.futures
import csv
import os
import random
import re
import sys
import threading
import time
import unicodedata
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
MIRROR = "https://gutenberg.pglaf.org/"
FALLBACK = "https://mirror.cs.odu.edu/gutenberg/"
USER_AGENT = "GutenFreeDiet/0.2 (db-combolite demo; seeded sample, bounded concurrency)"
RETRIES = 5

# "Title, by Author<spaces>79873": an entry's first line ends in its number.
ENTRY = re.compile(r"^(\S.*?)\s{2,}(\d+)C?\s*$")
LANGUAGE = re.compile(r"\[Language:\s*([^\]]+)\]")
START = re.compile(r"^\*\*\* ?START OF (THE|THIS) PROJECT GUTENBERG", re.I | re.M)
END = re.compile(r"^\*\*\* ?END OF (THE|THIS) PROJECT GUTENBERG", re.I | re.M)
# Older books also close with a plain line just above the END marker.
TRAILER = re.compile(r"\n\s*End of (the )?Project Gutenberg.*\s*$", re.I | re.S)


def parse_index(path, with_titles=False):
    """Yields (ebook number, language), or with titles (number, language,
    title line), for every entry in GUTINDEX.ALL."""
    number, lang, title = None, "English", ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = ENTRY.match(line)
            if m and not line.startswith("TITLE and AUTHOR"):
                if number is not None:
                    yield (number, lang, title) if with_titles else (number, lang)
                number, lang, title = int(m.group(2)), "English", m.group(1).strip()
                continue
            m = LANGUAGE.search(line)
            if m and number is not None:
                lang = m.group(1).strip()
    if number is not None:
        yield (number, lang, title) if with_titles else (number, lang)


def load_titles(index_path):
    """{ebook: title}: pg_catalog.csv's, else the GUTINDEX entry's (before ", by")."""
    titles = {}
    try:
        for n, _lang, title in parse_index(index_path, with_titles=True):
            titles[n] = title.split(", by ")[0]
    except OSError:
        pass
    path = os.path.join(DATA, "pg_catalog.csv")
    try:
        with open(path, encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                try:
                    titles[int(row["Text#"])] = " ".join(row["Title"].split())
                except (KeyError, ValueError):
                    pass
    except OSError:
        pass
    return titles


UNSAFE = re.compile(r"[^A-Za-z0-9_.-]")


def file_name(n, title, taken):
    """<Title>.txt for book n in shell-safe characters, unique among taken."""
    text = unicodedata.normalize("NFKD", title or "").encode("ascii", "ignore").decode("ascii")
    base = UNSAFE.sub("", re.sub(r"\s+", "_", text.strip()))
    base = re.sub(r"_+", "_", base)[:150].strip("._-") or str(n)
    name = base + ".txt"
    if name in taken:
        name = f"{base}_{n}.txt"
    return name


def read_manifest(path):
    """[(n, url, bytes, file)] in order; file "" for the older <n>.txt lines."""
    rows = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if parts and parts[0].isdigit():
                    rows.append((int(parts[0]), parts[1] if len(parts) > 1 else "",
                                 parts[2] if len(parts) > 2 else "", parts[3] if len(parts) > 3 else ""))
    except OSError:
        pass
    return rows


def rename_existing(out, titles):
    """Renames every listed book to the name its title gives now (older
    <n>.txt files too) and rewrites the manifest."""
    manifest = os.path.join(out, "manifest.tsv")
    rows = read_manifest(manifest)
    taken, new_rows, moved = set(), [], 0
    for n, url, size, name in rows:
        old = os.path.join(out, name if name else f"{n}.txt")
        new = file_name(n, titles.get(n), taken)
        taken.add(new)
        if os.path.exists(old) and old != os.path.join(out, new):
            os.replace(old, os.path.join(out, new))
            moved += 1
        new_rows.append((n, url, size, new))
    with open(manifest + ".part", "w", encoding="utf-8") as f:
        for r in new_rows:
            f.write("\t".join(str(x) for x in r) + "\n")
    os.replace(manifest + ".part", manifest)
    print(f"renamed {moved} books to their titles; manifest rewritten")


def book_dir(mirror, n):
    digits = str(n)
    parts = list(digits[:-1]) if len(digits) > 1 else ["0"]
    return mirror.rstrip("/") + "/" + "/".join(parts) + "/" + digits + "/"


def fetch(url):
    """The body at url. 429 and 503 are waited out: the server's Retry-After,
    else 2, 4, 8 ... seconds; other errors are raised."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    for attempt in range(RETRIES):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code not in (429, 503) or attempt == RETRIES - 1:
                raise
            after = e.headers.get("Retry-After", "")
            wait = float(after) if after.isdigit() else 2.0 * (2 ** attempt)
            print(f"  {url}: HTTP {e.code}, waiting {wait:.0f} s", flush=True)
            time.sleep(wait)


def get_text(mirror, n, pause, fallback=None):
    """Returns (url, text) for book n, or (None, reason). A network failure
    tries the fallback mirror once."""
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
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            if fallback and fallback != mirror:
                return get_text(fallback, n, pause)
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
    ap.add_argument("--mirror", default=MIRROR, help=f"mirror base URL, from MIRRORS.ALL (default {MIRROR})")
    ap.add_argument("--count", type=int, default=25, help="books to fetch (default 25)")
    ap.add_argument("--seed", type=int, default=1, help="sample seed (default 1)")
    ap.add_argument("--lang", default="English", help="language, or 'any' (default English)")
    ap.add_argument("--pause", type=float, default=0.0, help="seconds each download thread waits after a book (default 0)")
    ap.add_argument("--jobs", type=int, default=8, help="books downloaded at once (default 8)")
    ap.add_argument("--new", action="store_true", help="count only books not already in --out")
    ap.add_argument("--stream", action="store_true", help='print "BOOK <n> <file>" as each book lands')
    ap.add_argument("--rename-existing", action="store_true", help="rename <N>.txt books to their titles, then stop")
    ap.add_argument("--ebook", type=int, action="append", help="fetch this ebook number (repeatable)")
    ap.add_argument("--catalog", action="store_true", help="fetch cache/epub/feeds/pg_catalog.csv instead")
    ap.add_argument("--gutindex", action="store_true", help="fetch GUTINDEX.ALL (the catalog index) instead")
    ap.add_argument("--index", default=os.path.join(DATA, "GUTINDEX.ALL"))
    ap.add_argument("--out", default=os.path.join(DATA, "books"))
    a = ap.parse_args()

    if "www.gutenberg.org" in a.mirror or "//gutenberg.org" in a.mirror:
        sys.exit("gf_fetch: use a mirror, not www.gutenberg.org")

    if a.catalog or a.gutindex:
        if a.gutindex:
            url = a.mirror.rstrip("/") + "/GUTINDEX.ALL"
            os.makedirs(os.path.dirname(a.index), exist_ok=True)
            path = a.index
        else:
            url = a.mirror.rstrip("/") + "/cache/epub/feeds/pg_catalog.csv"
            os.makedirs(DATA, exist_ok=True)
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
    titles = load_titles(a.index)
    if a.rename_existing:
        rename_existing(a.out, titles)
        return
    rows = read_manifest(manifest)
    present = {r[0] for r in rows}
    taken = {r[3] for r in rows if r[3]}
    lock = threading.Lock()
    state = {"have": 0, "tried": 0}

    def say(msg):
        with lock:
            print(msg, flush=True)

    def one(n):
        """Fetches book n; True when it is now on disk and counts."""
        with lock:
            if state["have"] >= a.count:
                return False
            state["tried"] += 1
        url, text = get_text(a.mirror, n, a.pause, FALLBACK)
        body = strip_markers(text) if url is not None else None
        if body is None:
            say(f"  skip #{n}: {text if url is None else 'no START/END markers'}")
            return False
        with lock:
            if state["have"] >= a.count:  # others finished the sample meanwhile
                return False
            name = file_name(n, titles.get(n), taken)
            taken.add(name)
            present.add(n)
            with open(os.path.join(a.out, name), "w", encoding="utf-8") as f:
                f.write(body)
            with open(manifest, "a", encoding="utf-8") as f:
                f.write(f"{n}\t{url}\t{len(body.encode('utf-8'))}\t{name}\n")
            state["have"] += 1
            print(f"[{state['have']}/{a.count}] #{n}: {len(body.encode('utf-8'))} bytes from {url} -> {name}", flush=True)
            if a.stream:
                print(f"BOOK {n} {name}", flush=True)
        if a.pause:
            time.sleep(a.pause)
        return True

    todo = []
    for n in cands:
        if n in present or os.path.exists(os.path.join(a.out, f"{n}.txt")):
            if not a.new:
                state["have"] += 1
                say(f"[{state['have']}/{a.count}] #{n}: already here")
                if a.stream:
                    say(f"BOOK {n}")
                if state["have"] == a.count:
                    break
            continue
        todo.append(n)
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        # Submit in waves of `jobs`, so a sample that fills early stops
        # fetching instead of draining a long queue.
        it = iter(todo)
        running = set()
        while state["have"] < a.count:
            while len(running) < a.jobs:
                n = next(it, None)
                if n is None:
                    break
                running.add(pool.submit(one, n))
            if not running:
                break
            done, running = concurrent.futures.wait(running, return_when=concurrent.futures.FIRST_COMPLETED)
    say(f"done: {state['have']} books in {a.out} ({state['tried']} fetched this run, {time.time() - t0:.1f} s)")
    if a.ebook and state["have"] < len(a.ebook):
        sys.exit(1)


if __name__ == "__main__":
    main()
