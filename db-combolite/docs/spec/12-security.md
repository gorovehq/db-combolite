# Section 12: Security

Status: **Draft. The decisions below are confirmed; the open questions are not.**

## Purpose

Earlier sections say *what* is protected: enclaves (Section 1.4), joined stores (Section 4.9), the decrypt cache (Section 1.5), keyed content hashes (Section 6.3) and identity keys (Section 5). This section says *how* they are protected, and who may do what.

## Confirmed decisions

### 12.1 Authenticated encryption, selected by a header flag

The algorithm is chosen per file and recorded in a header flag, in the same way as the checksum algorithm (Section 2.9):

- **AES-256-GCM.** Hardware-accelerated on ESP32 and on most desktop and server CPUs.
- **ChaCha20-Poly1305.** Fast in software, for chips that have no AES hardware.

**Nonces are deterministic.**

- **Pages.** The nonce is built from the reserved archive track ID and the page's **write sequence number**. Page sequence numbers are unique across the whole file.
- **Delta records.** The nonce is built from the record's **track ID** (coordinator slot plus companion type) and its **delta sequence number**.
- **Layout.** Both kinds of nonce are 12 bytes: a 4-byte track ID followed by an 8-byte sequence number (Section 14.2.5). The header is authenticated as associated data.

No random number generator is needed for nonces.

**Per-session salts make forks safe.** A copy of a database file that goes on being written beside the original (a cloned database, a restored backup, a snapshot) counts its sequence numbers up from the same point, so on its own it would repeat nonces under the same keys. To prevent this:

- each time a database is opened for writing, the library draws a **random 128-bit session salt**, once;
- page and record subkeys are derived from the domain key **and the session salt** (12.2), so two forks of one file encrypt under different subkeys and their nonces may repeat harmlessly;
- each page and record names the salt it was encrypted under, by a small index into a **salt table** kept in the file. A salt is removed from the table once no page or record uses it, as an old key version is (12.4).

The random source is needed once per session, never per nonce. Without one, an encrypted database opens read-only; a platform that encrypts must supply entropy through the environment's random hook. A replica (Section 19) needs nothing more: it writes under its own session salts like any copy. Layout: 14.2.8.

**Sequence numbers are reserved durably (review R1).** Before any sequence number in a range *[n, n+B)* is used, the high-water mark *n+B* is persisted:

- page sequences: in the working header copy;
- delta sequences: in the owning coordinator's slot.

After a crash, numbering resumes at the persisted high-water mark. A sequence number is therefore never issued twice under the same key, even if the earlier pages were written but never committed.

### 12.2 Key hierarchy

- **Envelope encryption.** Enclave keys are stored in the external keystore (Section 1.4) **encrypted under the master key**. A stolen keystore is useless without the master key.
- **Separate subkeys for each purpose.** From each domain key (an enclave's or a joined store's), and from each of its key versions, the library derives a separate subkey for each job with HKDF-SHA-256, the job's label as the info:

  | Subkey | Label | Session salt (12.1) | Used for |
  |---|---|---|---|
  | page | `cdb page` | yes | encrypting pages and delta records (counter nonces, 14.2.5) |
  | hash | `cdb hash` | no | keyed content hashes (Section 6.3) |
  | index | `cdb index` | no | protecting the domain's indexes, and the tags of its entries in joined stores (12.9) |
  | seal | `cdb seal` | no | encrypting sealed references in joined stores (12.9) |
  | seal nonce | `cdb seal nonce` | no | deriving those references' synthetic nonces |
  | replication | `cdb repl` | no | encrypting sealed changes in replication slices (14.21.5) |
  | replication nonce | `cdb repl nonce` | no | deriving those changes' synthetic nonces |

  Only the page subkey uses a counter nonce, so only it takes the session salt; every copy and session computes the others alike. Distinct labels keep every use apart: a weakness in one use of a key can't affect another use.
- **Joined stores** (Section 4.9) have keys of their own, handled exactly like enclave keys.

### 12.3 Key provider references

The header records a **reference** to each key, never the key material itself. A reference has a URI-like form, for example:

- `vault://secret/cdb/tenant7#v3`
- `env://CDB_MASTER_KEY`
- `hwrot://efuse/block1`

The library ships with a set of providers. Applications can register further providers through a hook, using the same pattern as the embedding hook (Section 8.7).

**`file://` keys are allowed for development only.** Whenever one is in use, the library **warns loudly and repeatedly**, so that key files don't end up in repositories:

- it warns every time the database is opened;
- it warns more strongly when the key file is inside a git working tree and is not ignored;
- it **refuses** `file://` keys when the configuration marks the database as production.

### 12.4 Key rotation

- **Master key.** Rotating the master key re-wraps the enclave keys under the new master key. Pages are not touched, so the rotation is cheap and immediate.
- **Rotation timers.** The master key is subject to a **rotation timer**, so that operators follow best practice on key rotation.
  - The rotation interval is **configurable**, with a default.
  - What happens when rotation is overdue is also **configurable**, so that it matches the operator's standards. Possible responses are to warn, to refuse writes, or to refuse to open.
  - **Enclave keys and joined-store keys** follow the same norm: each has a rotation timer with a configurable interval and a configurable overdue response.
- **Enclave keys.** Every page records the version of the key that encrypted it. After an enclave key is rotated:
  - new writes use the new key;
  - older pages are re-encrypted during maintenance compaction, within the scrub budget (Section 3.5).

  The database never has to be rewritten all at once.

### 12.5 Identities, RBAC and signing

- **Identities.** A **principal identity** is an asymmetric **key pair**. An operator registers it in the catalog. Each principal belongs to a tenant and is authorized for specific enclaves. A coordinator's **session identity** (Section 5.1) is ephemeral, and the session authenticates as a principal.
- **RBAC.** Role-based access control is built in. Configuration sets how strict it is, anywhere from off to as strict as possible. The library doesn't police operators: each operator decides how strict to be.
  - **Roles** are granted to identities.
  - **Permissions** are `read`, `write`, `execute` (procedures) and `admin`. They can be granted on the whole database, a store, an enclave or a procedure.
  - **An enclave's roles are defined inside that enclave** (Section 4.4), so its access rules don't leak.
  - **Principals and roles are catalog objects** (14.3.7), so they survive a reopen.
    - A **principal** is recorded once, in the public catalog: its name, tenant and public key. Public keys are public by nature; anyone with the file can list principals, but not what they may do inside an enclave.
    - A **role** is a named set of (scope, permissions). Principals are granted roles, so changing what a role allows changes it for every holder at once.
    - A role, and its grants, live in the catalog of the domain its scopes lie in: public roles (which alone may cover the whole database) in the public catalog, an enclave's roles in the enclave's catalog, a joined store's in its own.
    - **History** is kept in the audit series: creating, changing or dropping a role, granting or revoking one, and registering or retiring a principal are each events in the hash-chained audit series of the domain concerned (12.7). The catalog holds only the current state.
- **Signing.** Revisions and audit events can be signed with the identity's key. The policy is configurable:
  - sign every revision;
  - sign audit-series events only (**the default**);
  - sign nothing.

#### 12.5.1 What RBAC does and does not protect

db-combolite is a serverless library, so RBAC checks run **inside the application's own process**. A hostile application that holds an enclave key can bypass them. Operators must not overestimate what RBAC gives them:

- **Confidentiality comes from keys.** Data that a role must never see belongs in an enclave whose key that role never receives. Without the key, the data can't be read, whatever the process does.
- **RBAC is policy on top of keys.** It enforces read, write, execute and admin rules for applications that cooperate, and it prevents mistakes. It is **not** a security boundary against a hostile process that already holds the keys.

### 12.6 Secrets in memory

- Keys and decrypted buffers are **wiped from memory** as soon as they are no longer needed.
- Keys never appear in logs or in the audit series. Only key references and versions are recorded.
- Device-level protections, such as ESP32 flash encryption and secure boot, sit below db-combolite and complement it.

### 12.7 Tamper-evident mode

- **Keyed integrity tags.** Public stores use a **keyed integrity tag** instead of a plain checksum, so a page can't be edited without the edit being detected. The tag key is an **integrity key derived from the master key** (review G6). A database in governance mode therefore always needs a master key, even if it has no enclaves.
- **Hash-chained audit series.** Every audit series (Section 10.7) is **hash-chained**, including each enclave's own series: every entry includes the hash of the entry before it. Editing or deleting an entry breaks the chain.
- **On by default for every database whenever governance mode is active.**

### 12.8 Stores in enclaves

An **encryption domain** is either the public domain (ID 0), an enclave, or a joined store (12.9). Every store belongs to exactly one domain at a time, and so does every page (Section 2).

- **Joining at creation.** Creating a store (a document pool, table, vector collection or time-series store) can name an enclave. The store is in that enclave's domain from its first write: its data pages, index nodes, overflow chains, revisions, nibblet pages and derived indexes (full text, neighbour graph, deduplication) are all written in that domain, under the enclave's page subkey (12.2).
- **Moving later.** An existing store can be moved into an enclave, out of one, or between two. The move runs in the background, like key rotation (12.4):
  - an explicit call, made with both domains open, moves the store's record from the old domain's catalog to the new one's and records the domain it is moving out of (14.3.3). The store keeps its ID. The old domain's held log is drained first (14.4.10);
  - new writes go to the new domain at once, and the store's nibblet records are re-placed in the new domain's nibblet pages;
  - maintenance compaction walks the store's structures and re-encrypts the pages still in the old domain, within the scrub budget (Section 3.5); the walk resumes where it stopped. Every page has the same room in every domain, because its payload always starts with a store ID slot (14.2.7), so each page is re-encrypted as it is;
  - until the move completes, reading or writing the store needs both domains open, because each page's header names the domain it was written in;
  - a **finish** call completes the re-encryption at once, for an operator who needs single-key access sooner or wants to retire the old key;
  - starting and finishing a move are audit events in the public audit series (store and domain IDs only) and in the audit series of each enclave or joined store involved (12.7), so a move out to the public domain is always on record.
- **Shared pages stay within a domain.** Pages that several stores share, such as nibblet pages (Section 16), only ever hold records of one domain. Each domain has its own nibblet free-space index.
- **The enclave's catalog.** An enclave keeps its own catalog, encrypted under its key, as Section 4.4 requires: its stores' records, names, schemas, field dictionaries, link types, procedures and roles. The public catalog lists only each enclave's opaque ID, key references and rotation state (the enclave directory, 14.3.4). Without the key, the names, schemas and contents of an enclave can't be learned, and finding a store in an enclave by name needs the enclave open.
- **Each enclave has its own link store** for links whose ends are both inside it. A link between domains goes in a joined store (12.9).
- **Wrapped keys stay in the keystore.** A key reference (12.3) points at the enclave's key **wrapped under the master key** in the external keystore. The library fetches the wrapped key and unwraps it with the master key; the archive holds only references, never key material (Section 1.4).
- **What stays readable.** Page headers and delta record headers of an enclave or joined store are authenticated but not encrypted, and they carry **only the domain ID**. The store ID and entry ID of an encrypted page or record are kept inside its encrypted payload (14.2.7, 14.4.2): a page's readable store ID holds 0 and its store ID slot is encrypted, and a record's readable store field holds its domain ID. An outsider with the file can learn which domain wrote how much and when, but not which store or entry changed, nor any name, schema, content or key.
- **Collisions with hidden IDs.** When a process scans another process's journal for clashing writes (5.9.4), it skips the encrypted records of every domain its own transaction did not write: they cannot overlap its write set. It decrypts the records of the domains it did write, whose keys it holds because it wrote there, and compares their store and entry IDs. No process needs another tenant's key, and no write clashes needlessly.
- **Replication.** Replication between copies of a database (Section 19) exports a domain's changes only while the domain is open, and are sealed in the slice under the domain's key, never sent in plaintext. The receiving copy must hold the same domain key to import them; until it does, it holds them, as recovery holds records (below).
- **Held changes.** Applying a domain's changes writes its encrypted pages, so it needs the domain's key. When the process applying committed changes lacks it (recovery with the enclave closed, or a reconciling process (5.9) that never opened that enclave), the domain's changes are **held** in the domain's **held log**, inside the archive (14.4.10):
  - **Split by domain.** A transaction that changed several domains applies in the domains the process can apply, and is held in the others. Atomicity holds within each domain.
  - **Commit order.** Once a domain has held changes, every later change of that domain is held behind them, even by a process that could apply it, until the log is drained.
  - **Visibility.** A process with the domain open reads the domain's held log as part of its view, so opening never waits and reads see every committed change.
  - **Draining.** A process that holds the key drains the log at its next reconciliation: it applies the held changes in order, then the domain's new ones. Reconciliation never hands work to another process; whatever the reconciler can't apply, it holds.
  - **Closing.** A process closes a domain only after its own journal holds none of that domain's unreconciled changes; closing reconciles first when it does. Rotation (14.4.7) re-encrypts a process's own records, which needs their keys.
- **Accelerator mirrors.** A vector collection in an enclave is mirrored on an accelerator (Section 18) only while its enclave is open. Closing the enclave evicts the mirror and zeroes its device memory, as the decrypted page cache is wiped (12.6).

### 12.9 Joined stores and links between domains

- **What a joined store is.** A joined store is an encryption domain of its own, created explicitly by an operator, like an enclave: it has an object ID and a key reference, and its key is wrapped in the keystore and rotated like an enclave key (12.2, 12.4).
- **Links between domains.** A link whose two ends are in different domains is stored in a joined store. A link type whose ends may be in different domains names the joined store its links go into.
- **Opaque pointers.** Inside a joined store, an end that lies in an enclave is an **opaque pointer** with two parts (14.13.2):
  - **a tag**: the keyed hash (HMAC-SHA-256, all 32 bytes) of the joined store's ID and the entry reference under that enclave's index subkey (12.2), used in the index key, so that equal references match and a pair can be looked up, and the same entry can't be matched across two joined stores;
  - **the sealed reference**: the entry reference encrypted under the enclave's seal subkey, so that only holders of the enclave key can follow it. Its nonce is **synthetic**: a keyed hash of the joined store's ID and the reference, under the enclave's seal nonce subkey. It is never stored, needs no random source (12.1), and is the same only when everything it protects is the same.

  A joined store therefore reveals neither the names nor the structure of the enclaves it points into. An end in the public domain is stored as is.
- **Link types that cross domains** live in the joined store's catalog. A link type names the joined store its links go into and the **member domains** whose entries its links may join; a link with an end in any other domain is refused.
- **Membership.** Each member domain's own catalog lists the joined stores that link into it (the public catalog does so for the public domain). Without a member's key, nobody learns which joined stores it belongs to.
- **Keys needed.**
  - Defining a cross-domain link type needs the joined store and every member domain open.
  - Adding, reinforcing or removing a link needs the joined store and the domain of each enclave end open: the writer checks the end exists and computes its tag and sealed reference.
  - Traversing from a public end needs the joined store open, and yields opaque pointers; following one needs its enclave open. Traversing from an enclave end needs that enclave open too, to compute the tag.
- **Deletes.** Deleting an entry, or dropping or moving a store, in a domain is refused with `CDB_E_SECURITY` while any joined store that links into that domain is closed. Every delete rule (tombstone, cascade, refuse; 14.13.1) then holds across domains exactly as within one.
- **Compound procedures.** A stored procedure that touches more than one domain is stored in a joined store (Section 4.9).

### 12.10 Stored procedures in the catalog

Stored procedures are catalog objects (14.3.6), with the source text, compiled form, dependencies and compiler version that Sections 4.5 and 4.10 list.

- **Bodies.** Source and bytecode are stored inline in the record when they are small, and in overflow chains (14.15) otherwise, as document bodies are.
- **Dependencies** are recorded per store and field: each is a store, a field of it or the whole store, and whether the procedure reads it, writes it or both; the procedures it calls are recorded too. They feed the quick-query index (4.2), placement (below), and the schema checks that find the procedures using a field.
- **History.** Replacing a procedure keeps every earlier version (source, bytecode, dependencies) as a matter of record, like schema history (4.11): it is discarded only when the configuration allows it and the operator asks.
- **Placement** follows Section 4.9:
  - a procedure whose dependencies are all in the public domain is in the public catalog;
  - one whose dependencies are all in one enclave is in that enclave's catalog;
  - a compound procedure, touching more than one domain, is in a joined store, named when the procedure is created, whose member domains (12.9) include every domain it touches.
- **Moving with stores.** Placement is kept right as stores move. A store move (12.8) re-places every procedure that depends on the store, in the same archive commit. A move that would leave a compound procedure in a joined store that does not list the store's new domain is refused with `CDB_E_STATE`, before anything changes.

## Open questions (do not specify until answered)

None at present. The byte layouts for 12.1 and 12.8–12.10 are in Section 14 (14.2.7, 14.2.8, 14.3.4–14.3.7, 14.4.10, 14.13.2, 14.21.5).

