# ADR-0001: Record architecture decisions (workspace)

- **Status:** Accepted
- **Date:** 2026-11-21
- **Supersedes / Superseded by:** —

## Context

`db-combolite` is a multi-package workspace (core engine, accelerator, tests, studio, demo) with
vendored third-party submodules and a shared toolchain. Cross-package decisions — the monorepo
layout, the submodule/vendoring policy, package boundaries (e.g. the accelerator is never part of
the core), and the shared build/format standards — need a durable, discoverable record separate
from the per-package specifications and the engineering ruleset in `CLAUDE.md`.

## Decision

Keep lightweight **Architecture Decision Records** at the workspace level in `docs/adr/`
(Nygard format), for decisions that span or sit above individual packages. Package-specific
decisions live in that package's own ADR log (e.g. `combolite-core/docs/adr/`).

## Alternatives considered

- **Only `CLAUDE.md` + package docs** — `CLAUDE.md` is the standing ruleset, not a dated decision
  log; package docs don't capture cross-package choices.
- **Per-package ADRs only** — no home for workspace-spanning decisions.

## Consequences

- Workspace decisions become reviewable, versioned artifacts, indexed in `docs/adr/README.md`.
- Small ongoing cost: author a workspace ADR for cross-package decisions (noted in `CONTRIBUTING.md`).
