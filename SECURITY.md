# Security Policy

`db-combolite` is the multi-package workspace for the Combolite crash-safe, crypto-bearing C99 database engine. We take security
vulnerabilities seriously and ask that they be reported through a private, coordinated
channel so that a fix can be prepared before any public disclosure.

---

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public GitHub issues, discussions,
or pull requests.** A public report discloses the flaw before a fix exists and puts
downstream users at risk.

Instead, report privately through **GitHub Security Advisories**:

1. Go to the repository's **Security** tab.
2. Select **Report a vulnerability** to open a private security advisory.
3. Provide as much detail as you can — affected component, version/commit, reproduction
   steps, and any proof-of-concept.

This creates a private advisory visible only to you and the maintainers. If you are unable
to use the private advisory flow, open a minimal, non-sensitive issue asking a maintainer
to initiate private contact — **without** including any vulnerability details.

---

## Scope

In scope:

- The C core library (`src/`, `include/`) — the engine, its on-disk archive (`.cdbarch`)
  and working-directory (`.cdbwork`) formats, journaling, serialization, and cryptographic
  code paths (HKDF, AES-GCM, SHA-256 / HMAC-SHA-256).

Out of scope:

- Vendored third-party submodules (Mbed TLS, xz, Unity) — report those upstream to their
  respective projects.
- Build tooling, examples, and documentation, except where a flaw in them directly
  compromises a built library.
- Issues already recorded under **Known Issues** in the [README](README.md) (e.g. the
  tracked key-wrap nonce item scheduled for remediation); these are known and need no new
  report.

---

## Supported Versions

`combolite-core` is pre-1.0 and developed on a milestone basis. Security fixes are applied
to the current `main` / latest milestone line on a best-effort basis. There is no formal
back-port support matrix for earlier pre-1.0 snapshots; please reproduce against current
`main` where practical.

---

## Response Expectations

The project is maintained on a best-effort basis. We aim to:

- **Acknowledge** a valid private report promptly after it is received.
- **Triage** it — confirm, assess severity, and determine affected versions — and keep you
  updated as the assessment progresses.
- Agree on a remediation timeline and a coordinated disclosure date with you.

These are good-faith intentions, not a contractual service-level agreement.

---

## Coordinated Disclosure

We follow coordinated (responsible) disclosure:

1. The report is handled privately while a fix is developed **under embargo**.
2. Once a fix is ready, we publish a security advisory and a patched release together.
3. Where warranted, we request a **CVE** through the GitHub advisory and **credit** the
   reporter in the advisory (unless you prefer to remain anonymous).

We ask that reporters keep details private until a fix is released and the advisory is
published. We appreciate your help in keeping `combolite-core` and its users safe.
