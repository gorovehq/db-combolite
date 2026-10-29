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

**Sequence numbers are reserved durably (review R1).** Before any sequence number in a range *[n, n+B)* is used, the high-water mark *n+B* is persisted:

- page sequences: in the working header copy;
- delta sequences: in the owning coordinator's slot.

After a crash, numbering resumes at the persisted high-water mark. A sequence number is therefore never issued twice under the same key, even if the earlier pages were written but never committed.

### 12.2 Key hierarchy

- **Envelope encryption.** Enclave keys are stored in the external keystore (Section 1.4) **encrypted under the master key**. A stolen keystore is useless without the master key.
- **Separate subkeys for each purpose.** From each enclave key, the library derives a separate subkey for each job:
  - encrypting pages;
  - computing keyed content hashes (Section 6.3);
  - protecting the enclave's indexes.

  A weakness in one use of a key therefore can't affect another use.
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

## Open questions (do not specify until answered)

None at present.
