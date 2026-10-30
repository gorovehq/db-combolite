# Attributions

## Authorship

db-combolite is designed and owned by Casey Annis (unklStewy).

The specification and implementation were developed with the help of
Claude, an AI assistant made by Anthropic, working through Claude Code.
Every design decision in the specification was confirmed by the owner.

## Third-party components

| Component | Use | License | Location |
|---|---|---|---|
| [Unity](https://github.com/ThrowTheSwitch/Unity) v2.6.1 | Unit-test framework, used only in tests | MIT | `db-combolite/third-party/unity/` |
| [XZ Utils](https://tukaani.org/xz/) v5.8.4, liblzma only | LZMA2 compression (spec 15), built when `CDB_WITH_XZ` is on | 0BSD | `db-combolite/third-party/xz/` |

Each third-party component keeps its own license file in its directory.
