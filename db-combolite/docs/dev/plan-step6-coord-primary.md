# Plan: liveness step 6, `coord/primary` and the slot count

Status, 2026-11-07: **written, not approved, not started.** Drafted by session combolite-25 and reviewed by session combolite-88. Basis: `main` at `ef3def4` (= `fix_pipeline-unopenable` at `4b0bedc`). Spec: 5.2, 5.9.5, 14.5.1. "PROPOSAL" marks a detail the spec does not settle.

## Owner decisions so far

| # | Question | Decision |
|---|---|---|
| D1 | Per-slot memory without alloc hooks (mcu) | **Caller buffer**: new `cdb_db_config` fields `coord_work`, `coord_work_size` (at the end; update `binding.py` and its size check). Host falls back to env alloc. |
| D2 | Fallback (no locks): does the elected primary write `coord/primary`? | **Yes**, unlocked; trusted while its slot heartbeats. |
| D3 | Wait for a torn or missing record | **Capped at the dead timeout**, then `CDB_E_BUSY`. Changes 14.5.1 wording: spec text on a `docs_` branch first. |
| D4 | The PROPOSALS in sections 2 and 4 | **Open.** |
| — | Reviewer's two changes (below) | **Open.** |
| — | Start the work | **Not approved.** |

## Reviewer's proposed changes (open)

1. **Re-election writes the in-memory count.** Every live process adopted `nslots` at join. A new primary writes the count it holds, never one read back from the record, which may be torn if the old primary died mid-rewrite (the plan's own kill sweep does that). Keeps 5.2's "the number never changes while in use".
2. **The rank delay doesn't block an application call.** Instead of sleeping settle×rank inside `beat_body`, record the earliest time to try again and check at each beat. A long sleep in an application call is the class of stall that caused the original bug.

Also: `merge_journals` changes (loop to the highest listed slot, cursors sized by journals present) run on every reconciliation, so measure the demo pipeline before and after.

## 1. Changes, file by file

- **`cdb_coord.h`**: drop `CDB_COORD_MAX` and the inline `peers[64]`, `held[64]`. Add `CDB_COORD_SLOTS_MAX 65534u`. `cdb_coord` gains `nslots` (published), `hi` (one past the highest slot with a file, from the last listing), `primary_lock`, and a per-slot block (peers, held, slot bitmap). `cdb_coord_config.max_coords`: 0 = profile default (host 64, mcu 2), capped at 65,534. New: `cdb_coord_work_size(nslots)`, `cdb_coord_peer_at(c, s)` for tests, `cdb_coord_primary_slot(c)`.
- **`cdb_coord.c`**:
  - `scan`: `struct names` keeps a bitmap in the block, not a `uint64_t`; `on_name` parses 1–5 digits (2 today).
  - Slot loops run to `c->hi`. `PRIORITY_LIMIT` becomes `2*nslots`. The fallback join's `lost_slots` mask becomes the bitmap.
  - New `read_primary`, `write_primary`, `take_primary` (lock mode) and the rank election in `beat_body`.
  - `hold_all` also takes `coord/primary` and removes it at the last close.
- **`cdb_db.c`**: `merge_journals` keeps `struct jcursor cur[CDB_COORD_MAX]` (about 120 B each) on the stack. It moves to cursors for slots that have journal files only, from the work block or env alloc, and reads only listed slot files (today 0..max_coords, which would be 65,534 opens). The loops near lines 979 and 2550 also move to `c->hi`. Open passes the coord work buffer through.
- **`cdb_db.h`**: `cdb_db` shrinks (peers move out); add the work pointer and an owned flag.
- **`cdb_env.h`**: no change.
- **Fault env**: `CDB_FAULTENV_MAX_FILES` 16→18, `MAX_LOCKS` 16→20.
- **`binding.py`**: the D1 config fields and size check.
- **Tests**: four places in `test_coord.c` index `.peers[]`/`.held[]`; they move to `cdb_coord_peer_at`.

## 2. `coord/primary` layout (14.5.1, as specified)

Magic `\x89CDBPRI\n` · byte order · epoch 1 · primary slot u16 · nslots u16 · 2 reserved · db_id 16 · session 16 · 460 reserved · CRC32C at 508. One 512-byte record, written in place, then synced. Checked against 14.5.1: matches.

PROPOSALS (D4):
- Byte order is the archive's, as in slot files.
- Wrong magic, CRC, db_id or epoch: torn or missing.
- Torn or missing while locked: re-read each settle interval up to the dead timeout, then `CDB_E_BUSY` (D3, decided).
- `nslots` must be at least one past the highest slot with files; otherwise `CDB_E_CORRUPT`.
- The primary syncs the coord directory after creating the file.
- Nonzero reserved bytes: `CDB_E_MALFORMED` (14.6.5).

## 3. Slot count, sizing, mcu

- Join, lock mode, with `coord/lock` held shared:
  - Try `coord/primary` exclusive.
  - BUSY: read the record (section 2) and adopt its `nslots`.
  - Got it, valid old record and a live slot (probing listed slot files' locks): re-election, keep the count (but see reviewer change 1).
  - Got it, nobody alive: `nslots = max(config, highest slot with files + 1)`, capped at 65,534; write the record.
  - Then take the lowest free slot below `nslots`, as today.
- Sizing: the per-slot block is allocated once, when the count is known: about 180 B per slot, 11 KB at 64 and 12 MB at 65,534. Merge cursors are sized by the journals present.
- Host without a work buffer: env alloc, freed at leave. mcu or no alloc hooks: caller buffer (D1); missing or too small is `CDB_E_OUT_OF_BUDGET` at open, sized by `cdb_coord_work_size`.

## 4. Re-election (lock mode)

- In `beat_body`, on the engine's thread, never the responder's: a subordinate test-locks `coord/primary`; BUSY means the primary lives.
- With the lock: rank = live subordinates with a lower priority value. Rank 0 keeps it and is primary at once. Otherwise release, wait settle×rank (see reviewer change 2), try once more. A winner rewrites the record with its slot and the same `nslots`, sets role 2 in its slot record, and keeps its clock (`follow_clock` keeps it at or ahead of the old primary's). A loser re-reads the record.
- PROPOSAL: rank counts only subordinates live at this process's last scan; an extra settle×rank covers a stale view.
- Fallback: election stays append → settle → choose (5.5); the elected primary also writes `coord/primary` unlocked (D2, decided).
- PROPOSAL: a fallback joiner that finds a valid record adopts its `nslots` and never claims primary while that primary's slot heartbeats.
- Tests: three processes with the primary killed (lowest-priority ticks first, rank 0 wins; same `nslots`; commits keep merging by stamp); primary killed with and without the baton; stale primary bit when it crashed alone (5.2); a host-thread test with real `flock`.

## 5. New write sequences and sweeps

Sequences:
- (a) The first open creates `coord/primary`: open+lock, write, sync, sync dir.
- (b) Re-election rewrites it in place.
- (c) The last close removes it under `hold_all`, before `rmdir`.
- (d) The count is raised over a high pending slot.

Sweeps, reusing `liveness_sweep` and `liveness_kill_sweep` in `test_coord.c`:
- Power cut, all four modes, in lock, fallback and keyed runs:
  - first open alone then commit; one later open sees the commit and a valid record
  - last close removing `coord/primary`; no working directory, no locks
  - a pending journal in slot k ≥ config, reopened with a smaller `max_coords`; `nslots = k+1` and the journal merges exactly once
- Kill:
  - the primary killed at each op of commit+reconcile while two others live; the survivors re-elect, commit, reconcile and close clean
  - the primary killed mid-rewrite during a re-election (two deaths)

Negative checks expected to be caught:
- A re-elected primary publishes its own config count: slot k ends up outside the range and never merges.
- The "highest slot + 1" raise dropped: same.
- The record's CRC check skipped: TORN mode yields a garbage `nslots`.
- The settle×rank delay dropped: the rank test fails.

## 6. Risks, scope, size

- **Risks:**
  - memory at large counts and scan cost (mitigated by looping to `hi` and reading only listed files)
  - mixed lock and fallback processes (the record is authoritative)
  - fault env file and lock caps
  - tests' direct struct access
- **Out of scope:**
  - raw-flash coordinator region
  - device-class values (64 and 2 are placeholders)
  - changing the count in use (the spec forbids it)
  - the 5.9.4 estimate wait (left over from step 5)
  - the demo 4-writer rerun
  - spec text for `mutex_trylock`
- **Size:** `cdb_coord.c` about +450/−150, `cdb_db.c` about +100, tests about +600; 2–3 sessions.
- **Order:**
  - first, spec text on a `docs_` branch for D3 and whatever D4 confirms
  - S1: sizing and work block, masks removed, no behaviour change
  - S2: `coord/primary` at join and close
  - S3: re-election
  - S4: sweeps and negative checks

  Each commit at `build-cuda` 67/67.
