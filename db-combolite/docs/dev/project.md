# The project

Moved from `CLAUDE.md`.

## What db-combolite is

A lightweight, portable, embeddable database (SQLite-class footprint) that supports these data models natively in one store:

- **Relational** tables and queries
- **Document pools** (schemaless documents)
- **Vector** storage and similarity retrieval
- **Matrix ordinality / cardinality**: ordered and counted relationships between entries
- **Stored procedures** and **cross-model queries** (one query can join relational, document and vector data)
- **Active and passive keying** of entries
- **Cardinal → Vector → Ordinal mapping** and **Neuron Mapping**

The confirmed meanings of these terms are in `docs/spec/06-keys-links.md`:

- **Active keys** are assigned explicitly by the user or application. **Passive keys** are derived by the database (content hash, auto-id or embedding).
- **Cardinal→Vector→Ordinal** maps an entry's identity to its embedding and then to its similarity rank. It is a stored structure that is kept up to date.
- **Cardinality** means relationship cardinality (1:1, 1:N, N:M).
- **Neuron mapping** is a graph of links between entries whose weights are learned.

### Hard constraints (these drive every design decision)

1. **Serverless, portable and embeddable.** It ships as a library, and at rest the store is a single file. There is never a daemon, broker or server process. Sessions, CRUD, multi-tenancy, reconciliation and coordination between processes are all done by the library code running inside each application. Processes coordinate only through the files themselves. Each library instance runs an in-process **coordinator** that plays the role a server would. The first coordinator to start is primary, and later ones are subordinate (see spec 1.8.0). Scope is a **single host**. There is no clustering, and no multi-machine access to one database.
2. **Microcontroller support is a long-term goal, not a current target.** The eventual aim is to package the library for PlatformIO, ESP-IDF and Arduino. The current focus is defining the **storage file format and its mechanics** as a standard. Don't add MCU build targets or a HAL yet. Do avoid format decisions that would block a constrained implementation later, such as unbounded page sizes or a format that can only be read by mapping the whole file into memory.
3. **Scales up to production.** It needs crash safety (journal or WAL), integrity checks and concurrent readers.
4. **Not a competitor to Postgres, MariaDB, Oracle, MySQL or MSSQL.** Turn down any feature that only makes sense for a server-class RDBMS.
5. **Clean-room spec and implementation.** The file format, storage engine and query layer are new designs optimized for this project's use case. Ideas from existing systems (SQLite's pager, LMDB, HNSW and so on) can be studied and cited as background in design docs. Don't vendor, port, wrap or copy their code, and don't make the format compatible with theirs. `third-party/` is not for storage or index engines. Approved dependencies:
   - **Unity**, for tests.
   - **mbedTLS**, for cryptography, behind a crypto interface of our own. Never hand-roll cryptography.
   - **ICU**, for language-aware collation on hosts that are not microcontrollers (spec 14.8.6).
   - **xz (liblzma)**, for LZMA2 compression of column chunks (spec 9.12). It is vendored in `third-party/xz` from a pinned release whose signature has been checked, never 5.6.0 or 5.6.1 (CVE-2024-3094), and its allocations go through the environment's allocator.
   - **CUDA** (the runtime and **cuBLAS**) and **Vulkan** (the loader and headers; `glslc` to generate the checked-in SPIR-V), for the optional accelerator library `combolite_accel` only (spec 18.5). They are system installs, never vendored, and never reach the core: the core sees only the `cdb_env` accelerator hook. Each goes in `ATTRIBUTIONS.md` when first used.

### Role in the larger effort

db-combolite is the storage component of a future **portable governance framework for agentic development**, so a new project can start with governance and agent memory already in place. It must also stay **distributable on its own**. Keep governance-specific logic out of the core engine. Governance is a *consumer* (a schema or package on top of the engine).

The first consumer is a project-governance store with these categories: **Architecture, Development, Security, Deployment, Contribution, CI/CD**. It also stores agentic memory: facts, links, retrieval by embedding.
