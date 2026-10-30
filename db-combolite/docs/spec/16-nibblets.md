# Section 16: Small records (nibblets)

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Before this section, the document engine gave every revision a page of its own. Tables are document pools (Section 9.7), so a 50-byte row would take a whole page: a million small rows at 4 KB pages would be about 4 GB, even with history off.

**Nibblets** keep small records together in shared, densely packed pages. A record moves to a page of its own only once it grows past a threshold. Space, memory, I/O and CPU all stay in proportion to the data, which keeps to the thinnest provisioning the project aims for.

Nibblets are a **general facility**. Any part of the engine that would otherwise give a small record a whole page can use them; 16.5 lists the users.

## Confirmed decisions

### 16.1 What becomes a nibblet

- **Threshold.** A record whose stored form is at most **a quarter of the page size** is a nibblet. A larger one gets its own page, plus an overflow chain past one page (14.15), as before.
- **Moving between forms.** Each record is placed by its own size whenever it is written. A record that grows past the threshold moves to a page of its own when it is next written, and one that shrinks below it returns to a nibblet.
- **Mixed files.** A record's address says whether it is a nibblet or a page of its own (14.18.2), so files written before nibblets existed stay readable.

### 16.2 Nibblet pages are shared across stores

- **Sharing.** A nibblet page may hold small records from any store and of any kind (16.5), so a database of many tiny stores stays dense.
- **Back-reference.** Every nibblet record carries its owner's store, its kind and the key its owner's index finds it under. A record can therefore be moved, and its owner's index fixed up (16.4), without the mover knowing that kind's format.
- **Encryption domains are never mixed.** Records of different encryption domains (enclaves, Section 12) never share a page. Each domain has its own nibblet pages, encrypted under its own key, and the public domain is domain 0.
- **Dropping a store.** A dropped store's nibblets stay in place until a space-reclaim pass removes them, as the store's other pages do today. Reading a dropped store never finds them.

### 16.3 Addressing: a slot directory

- **Layout.** A nibblet page starts with a slot directory, and records are packed from the end of the page toward it.
- **Address.** A record's address is **(page, slot)**.
- **Stable slots.** Records can be moved inside the page to close gaps without their slot changing, so the owner's index is untouched.
- **Removal.** Deleting a record frees its slot. A page whose last record goes is itself freed.

### 16.4 Placement: best fit, and merging at reconciliation

- **Free-space index.** Free space is tracked in a database-wide **free-space index**, keyed by (encryption domain, free bytes, page). The public domain's index root is kept in the catalog root slot (14.18.3).
- **Best fit.** A new record goes to the **fullest page it fits in**: the page with the smallest free space that is at least the record's size. Space freed by deletes and shrinkage is reused this way. When no page fits, a new nibblet page is started.
- **Merging at reconciliation.**
  - Each reconciliation merges nibblet pages that are less than half full.
  - The records of the emptier page are moved into another page of the same domain with room for them. Each moved record's owner index is updated through its back-reference, and the emptied page is freed.
  - A budget of pages per reconciliation bounds the work, as the neighbour-graph refresh does (spec 6.8).
- **Crash safety.** Placement and merging change pages only through the pager's copy-on-write. A crash therefore leaves either the old arrangement or the new one, and recovery replays committed changes the same way.

### 16.5 Users of nibblets

| Record kind | Owner's index and key | Built in |
|---|---|---|
| 1 Document and table revisions (14.9.2) | revisions index: (entry, revision) | M17 |
| 2 Time-series blocks of sparse series (14.11.1); a block graduates to its own page as it fills | blocks index: (series, first timestamp) | M17 |
| 3 Column chunks of small tables (14.10.2) | the row group's chunk list | M18 |
| 4 The last, partly filled page of an overflow chain (14.15) | the chain's owner | later (backlog) |

Some data already packs small records, so it gains nothing from nibblets and does not use them:

- **Vector blocks** hold many vectors each.
- **B+tree nodes** (every index, the catalog and the link store) pack entries into leaves, and each node needs a page of its own.

## Open questions (do not specify until answered)

None at present.
