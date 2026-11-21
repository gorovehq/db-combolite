# Architecture Decision Records (ADRs) — workspace

This directory records **cross-package / workspace-level** architecture decisions for the
`db-combolite` monorepo (structure, submodule/vendoring policy, package boundaries, shared
toolchain). Package-specific decisions live in that package's own ADR log (e.g.
`combolite-core/docs/adr/`).

One short, immutable record per decision: context, decision, alternatives, consequences.
Append-only — supersede, don't rewrite. Number sequentially (`NNNN-kebab-title.md`); status is
`Proposed` / `Accepted` / `Superseded by ADR-XXXX` / `Deprecated`. Use the template in
`combolite-core/docs/adr/README.md`.

## Index

| ADR | Title | Status |
|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions (workspace) | Accepted |
