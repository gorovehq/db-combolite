# Spec review 1: cross-section consistency

Date: 2026-10-28. Scope: Sections 1–13 as drafted.
**Status: every proposal below was approved and has been applied to the spec on 2026-10-28.** G2 used option (a). G6 accepted that governance mode always needs a master key.

Findings fall into three groups:

- **Critical (R):** correctness or security problems that would cause real failures if implemented as written.
- **Gaps (G):** the spec is silent or ambiguous on something the sections need.
- **Editorial (E):** stale text or missing cross-references. These are not design changes.

---

## Critical

### R1. Nonces can be reused after a crash (12.1 with 3.6 C-R1)
**Problem.** A page's nonce is built from its logical page ID and its write sequence number. Suppose pages are appended and encrypted with sequence numbers *n…m*, and the process then crashes before commit (C-R1). The sequence counter isn't committed, so after recovery the same numbers are used again for different plaintext under the same key. With AES-GCM, reusing a nonce breaks both confidentiality and authentication. Delta records (1.3) are encrypted too, and the spec gives them no nonce scheme at all.
**Proposal.**
- Reserve sequence numbers durably, in blocks. Before any number in the range *[n, n+B)* is used, persist the high-water mark *n+B*: in the working header copy on host files, and in the coordinator slot for delta tracks. After a crash, numbering resumes from the high-water mark, so no number is ever issued twice.
- Delta records use the nonce *(track ID, delta sequence)* and come under the same reservation rule.
- Add a crash case to the test matrix: "re-encryption after crash never repeats a nonce".

### R2. A transaction can be half-applied after replay (11.3 with 1.3)
**Problem.** A transaction may write deltas to several tracks: `rel.wal`, `vec.wal`, the link store and so on. Replay works track by track, from each track's watermark. If a crash happens after some tracks received their deltas and others didn't, replay applies part of the transaction. That breaks 11.3's "all or nothing".
**Proposal.**
- Every delta carries a **transaction ID**.
- A transaction counts only once its **commit record** has been written; under the **full** durability level, it is acknowledged only after that record's barrier.
- Replay applies a transaction only if its commit record exists, and discards incomplete transactions.

### R3. Several processes writing to the same delta track (1.3 with 5.3–5.7)
**Problem.** Section 5.7 gave coordinator slots a single writer each. The delta tracks (`rel.wal` and the rest) are still shared, so several processes append to the same file. That brings back the shared-append problem that 5.7 was written to remove. Section 5.4 is also ambiguous: it describes the baton as covering "a write pending", while 5.3 says the baton is only the right to reconcile or compact.
**Proposal.**
- **Delta tracks per process.** Each coordinator writes its own tracks, for example `rel.<slot>.wal`, and is their only writer.
- **Merge at reconciliation.** The baton holder merges all the per-process tracks in commit order, using commit timestamps taken from the primary's clock.
- **Writes don't need the baton.** Only reconciliation and compaction do. In 5.4, *x-total* becomes the size of the reconciliation or compaction work.

### R4. On open, the working header copy can be mistaken for a commit (1.9 with 3.2)
**Problem.** The working copy is rewritten at checkpoints (1.9) and carries a valid checksum. When the file opens, the rule picks "the valid copy with the highest sequence". The spec doesn't say how a working-copy checkpoint is distinguished from a commit, so the two copies could tie, or the working copy could be chosen.
**Proposal.**
- Each header copy carries a **role** (committed or working) and a **committed sequence** field.
- A checkpoint writes the working copy *without* increasing the committed sequence. A commit does increase it.
- On open, the copy with the highest committed sequence wins. If both are equal, the copy whose role is "committed" wins.

### R5. Contradiction: what counts as the catalog's redundant copy (3.5 vs 4.7)
**Problem.** Section 3.5 says the catalog keeps a full second copy. Section 4.7 says the A/B root slots *are* that redundant copy, and that no other copy exists. But the root slots duplicate only the **root**. The catalog **body**, which grows by appending, has no second copy. Without its catalog nothing in the database can be interpreted, so damage to one body page would leave tier 1 of 3.5 unable to repair it.
**Proposal.** Write every page of the catalog body **twice** (mirrored appends), as 3.5 says. The A/B slots remain the root's redundancy.

### R6. Catalog root updates are missing from the commit sequence (3.2 with 4.7)
**Problem.** A reconciliation that changes the catalog, for example a schema change, has to update the root slot at location 0. Section 3.2 doesn't say when that happens. If the root slot is written but the header commit never happens, the root points to catalog state that was never committed.
**Proposal.**
- The commit order becomes: data pages → anchor map → **catalog root slot**, carrying the new commit sequence → header commit, with a barrier after each step.
- On open, use the newest root slot whose sequence is ≤ the committed header sequence.
- Add the matching crash cases to the test matrix.

---

## Gaps

### G1. What the header can hold (1.10 vs 4.4, 4.7, 2.6)
Section 1.10 puts **per-store encryption state**, a quick-query index pointer and **the** dedup-index pointer in the header. That creates three problems:
- each epoch has a **fixed header length** (4.7), so the header can't hold per-store lists that grow;
- per-store state for enclaves must stay inside the enclave (4.4);
- dedup indexes are **per encryption domain** (2.6), so there is no single "the" dedup index.

**Proposal.** The header holds only fixed-size items:
- magic, epoch, byte order, geometry and address width;
- algorithm flags and feature flags;
- the database's unique ID;
- the role and committed sequence (R4) and the sequence high-water marks (R1);
- the companion slots (a fixed count per epoch) and their watermarks;
- the location of the anchor-map root.

Everything that applies per store or per domain moves to the catalog: the public catalog for public stores, the enclave's own catalog for enclaves.

### G2. Dedup granularity (1.10 vs 6.3)
Section 1.10 describes deduplicating "repeated byte sequences", which is sub-entry, block-level dedup. Section 6.3 makes the **whole-entry** content hash the dedup key. Which applies?
- **(a)** Whole entries and whole attachments only. This is simple and matches 6.3.
- **(b)** Also split large blobs into chunks at content-defined boundaries and deduplicate the chunks. That saves more space but costs more.

Recommendation: (a) now, with (b) able to be added later as a page type.

### G3. Do time-series points get their own universal IDs? (6.1 vs 10.3)
Under 6.1, every entry, time-series points included, has a permanent ID that is never reused. Giving every point an 8-byte ID would undo the compact columnar storage of 10.3. It would also exhaust 16-bit IDs on compact files (13.4) almost at once.
**Proposal.** The **series** is the entry. A point is addressed by *(series entry ID, timestamp)*, and links can point to a point using that pair.

### G4. Two kinds of identity (5.1, 5.5 vs 12.5)
Section 5 uses an "identity key" for each writing process, including as a tie-breaker. Section 12.5 defines identities as key pairs that an operator registers, which is the basis for RBAC and for attribution.
**Proposal.** Separate the two:
- **Session identity:** ephemeral, one per coordinator. Used for coordination.
- **Principal identity:** registered by an operator. Used for attribution, signing and RBAC.

A session authenticates as a principal.

### G5. The audit series vs enclave secrecy (10.7 vs 4.4)
Revisions, schema changes and key operations *inside* an enclave would be written to the single system audit series. If that series is public, it leaks the enclave's activity.
**Proposal.**
- Each enclave keeps its own audit series inside the enclave.
- The public series records enclave events only as opaque entries.
- Each series has its own hash chain (12.7).

### G6. Tamper-evident public stores need a key (12.7)
A keyed integrity tag on a public page needs a key, and public stores have no enclave key.
**Proposal.** Derive an integrity key from the master key. A consequence: in governance mode every database needs a master key, even one with no enclaves. Is that acceptable?

### G7. The validation map at reseal (13.5 vs 1.3)
The validation map lives in a companion, and companions are removed when the database is resealed. As written, every reseal loses it.
**Proposal.** At reseal, persist the map into the archive as a page type in the **compatible** class. Because it is compatible, older readers can ignore or drop it.

### G8. Who supplies time-series timestamps (10.2)
Section 10.2 says timestamps come from the primary's clock. Sensor and imported data, though, carry their own timestamps.
**Proposal.** Application-supplied timestamps are accepted. The primary's clock is used only when no timestamp is given.

### G9. The page-type list is incomplete (2.6)
Types that later sections introduced and that are still missing:
- catalog root slot (4.7);
- neighbour-graph block (6.4, 7.4);
- full-text index node (8.10);
- blob/attachment (8.4);
- validation map (13.5);
- coordinator slot (5.7), on raw-flash regions.

The "Header" row should also say "header slot (A/B, or a ring on flash)".

### G10. The crash matrix covers only three sequences (3.6)
Sequences still to add, during the byte-layout pass:
- catalog root update (R6);
- transaction commit record (R2);
- sequence reservation (R1);
- key-rotation re-encryption;
- baton resume and renew (5.4);
- unseal and reseal;
- missing-companion handling (1.12.5);
- `danger_mode` working on a copy.

---

## Editorial (no design change)

- **E1.** The file-listing example in 1.3 (flat `<project>.rel.wal` files next to the archive) has been superseded by the working directory in 1.12.1. Replace it, or point to 1.12.1.
- **E2.** Section 1.9 should mention the slot ring on raw flash (1.11.3).
- **E3.** Section 2.3 should mention that tamper-evident mode replaces the checksums on public pages with keyed tags (12.7).
- **E4.** Section 4.12 says IDs match the address width. It should note the 16-bit exception for compact files (13.4).
- **E5.** The status line in Section 1 still says offsets will be set "once the open questions are answered and Section 2 is settled". It should say they are deferred to the byte-layout pass.
- **E6.** Section 13.3 says the header is "authenticated", but on a page protected only by an unkeyed checksum it is merely covered by a checksum. The wording should say so.
- **E7.** The note in Section 5's open questions about the "future security section" should point to Section 12.
