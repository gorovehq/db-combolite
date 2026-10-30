# Section 10: Time series

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Time series are sequences of timestamped values: metrics, sensor readings, event streams and audit trails. They are written quickly, mostly appended, and usually queried by time range. Those access patterns call for storage that is different from the other data models.

## Confirmed decisions

### 10.1 Series and points

- Each series is identified by a **series key**: a name plus optional tags. For example: `build_duration{repo=combolite, branch=main}`.
- Each point is a timestamp plus **one or more named values**. Values use the shared type system (Section 9.1).

### 10.2 Timestamps

- Each series picks its timestamp **precision** when it is created: seconds, milliseconds, microseconds or nanoseconds.
- The **application may supply** a point's timestamp, for example sensor time or imported data. When it doesn't, the timestamp comes from the **primary coordinator's clock** (Section 5.2), so timestamps assigned in different processes are consistent with each other (review G8).
- Until the coordinator exists, the clock is the environment's. A point with no timestamp, on a platform with no clock, is refused.
- A point is addressed as *(series entry ID, timestamp)*. Points have no IDs of their own (Section 6.1).

### 10.3 Storage in time-windowed column blocks

- Points are stored in **time-windowed blocks**, for example one block per hour per series. The window size can be configured.
- Inside a block, data is laid out **by column**:
  - timestamps are stored as delta-of-delta;
  - values are stored as the changed bits relative to the previous value.

  These are published techniques. db-combolite studies them and implements its own versions (constraint 5).
- A whole window can be dropped or compacted in one operation.

### 10.4 Retention, downsampling and recipes

- **Retention** can be configured per series: points older than N are deleted.
- **Downsampling** can be configured per series: old raw points are replaced by summaries.
- The default for both is **keep everything**.
- **Operator-defined recipes.** The operator can define how data is downsampled or derived. Recipes are stored procedures (Section 4.5). A recipe works in one of three ways, depending on the operation:
  - **Derive.** The recipe writes its results to a **new derived series** that is linked to its source (Section 6), for example `power_kwh` derived from `power_w`. The raw source series is kept and follows its own retention policy.
  - **Compact.** As part of a compaction, the recipe collapses raw data **inside the collection itself**, for example replacing instantaneous watt readings with a kWh total.
  - **Threshold compaction.** The recipe collapses any run of measurements that stay within a ± threshold into a single aggregate value: mean, average, maximum, minimum and similar.
  - Compact and threshold-compaction recipes discard raw data, so they run only when the operator configures them. Otherwise the default, keep everything, applies.

### 10.5 Late data is configurable by the operator

The operator chooses how points that arrive late are handled. Some operators want late data to show up in queries, and others don't. The available modes are:

- **accept** late points at any time;
- **accept within a window**: accept late points up to a configurable lateness, and reject anything older;
- **reject** all late points;
- **hold and forward**: hold late points in a delta track (Section 1.3), then fold them into the series at reconciliation.

The mode is set per store. A point is late when it is older than the series' newest point. Refusals happen when the change is made. In hold mode the journal is the delta track: a late point is parked when its commit is applied, unseen by reads, and folded in at the next reconciliation; recovery replays it the same way (14.11.1).

### 10.6 Duplicates and collisions

- **True duplicates.** A point whose series, timestamp and values all match a stored point is stored once.
- **Change-of-value storage.** When identical values arrive at successive timestamps, they are collapsed into one point. A new point is stored only when the value changes.
- **Change-of-value storage is off by default** and can be turned on per series: metrics keep every sample unless told otherwise.
- **Timestamp collisions.** If a point arrives with the timestamp of an existing point but with **different** values, only the changed value is kept: the new point's fields replace the stored ones, field by field, and fields it lacks keep their values. Until the audit series exists, the warning goes to an optional callback the application configures. The library also **logs a warning** about the collision, so application developers can find and fix the cause. The warning is written to the system audit series (10.7) and includes the value that was **replaced**. The series itself keeps only the changed value, but the governance record still holds what was overwritten.

### 10.7 The built-in system audit series

The database records its own events in a protected **system audit series**:

- revisions, and the identity that made each one (Section 8.12);
- baton handoffs and crashes (Section 5.4);
- schema changes (Section 4.11);
- key and enclave operations.

Retention and downsampling never apply to this series. Governance tools can query it like any other series. Application code can also use it to report health metrics, for example through an endpoint.

**Per-domain audit series (review G5).**
- Each encrypted enclave has its **own** audit series, stored inside the enclave and encrypted with it.
- The public audit series records events inside an enclave only as **opaque** entries: an opaque domain ID and an event class, with no names or contents.
- Each audit series has its own hash chain (Section 12.7).

## Open questions (do not specify until answered)

None at present.
