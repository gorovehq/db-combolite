# Handoff: Combolite Studio (GTK4 / libadwaita client for db-combolite)

## Overview
A desktop client for **db-combolite**, the embeddable C99 multi-model database (tables, document pools, vector collections, links, time series). It lets you browse and edit every model, run CMBQL queries, and explore vector and link data as interactive 2D/3D graphs.

Your task: **wire the real engine into the existing Python package** (`NativeBackend`) and finish the places marked TODO. The UI is already built.

## About the Design Files
There are two kinds of files in this bundle:

- `combolite_gui/` is **working Python code** (GTK 4 + libadwaita via PyGObject). Keep it. Don't rewrite it. It runs today against a built-in `MockBackend`.
- `reference/Combolite Studio Preview.dc.html` is an **HTML mock of the same UI**, used only as a visual and behavioural reference. Open it in a browser. Don't ship it. If something in the GTK code looks different from the mock, the mock shows the intent.

## Fidelity
**High-fidelity.** Colors, type, spacing and interactions in the mock are final. The GTK code implements them using libadwaita defaults plus `style.css`.

## Run it
```sh
# Fedora
sudo dnf install python3-gobject gtk4 libadwaita gtksourceview5 python3-numpy
# Debian / Ubuntu 24.04+
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-4.0 gir1.2-adw-1 gir1.2-gtksource-5 python3-numpy

# Launch app directly:
python3 -m combolite_gui                                          # auto-discovers native engine
COMBOLITE_LIB=/path/to/libcombolite.so python3 -m combolite_gui   # explicit library path

# Or build wheel & install desktop integration:
make wheel
pip install dist/combolite_studio-0.1.0-py3-none-any.whl
make install-user                                                 # registers desktop entry & icons
```
Needs GTK ≥ 4.10 and libadwaita ≥ 1.4. GtkSourceView and numpy are optional (the console falls back to a plain text view, and the graphs fall back to pure-Python math).

## The one job: implement `NativeBackend`
Everything touches the engine through `combolite_gui/backend.py`.

1. `Backend` is the interface. `MockBackend` shows the exact return shapes. Match them.
2. `NativeBackend(lib_path)` loads the C library with `ctypes`. Every method is a stub that raises `NotImplementedError`. The GUI catches that and shows a toast, so you can implement methods **one at a time** and the app keeps working.
3. Raise `BackendError("message")` for user-facing failures (constraint violations, cardinality, bad vector size). The message is shown verbatim in a toast.
4. The engine never allocates. Allocate the arena / page buffers in Python (`ctypes.create_string_buffer`) and keep them on `self` for as long as the handle is open.
5. Return-shape contracts the UI relies on:
   - `table_rows(...)` → `Result(columns, rows, total, elapsed_ms)`. **Column 0 is the primary key.** It's used for cell edits and deletes.
   - `table_schema(name)` → rows start with `(column_name, type, …)`. Types the editor coerces: `INT`, `REAL`, `BOOL`, `TEXT`, `TIMESTAMP`.
   - `link_edges(name)` → columns `source, target, created, weight`. **`weight` (0–1+) drives 3D depth, edge thickness and heat strength.** It's required for the graph to look right.
   - `vector_graph(coll, limit)` → `VectorGraph(ids, vectors, edges)`, where `edges` are index pairs from the engine's neighbour graph. If the collection has no neighbour graph, build k-NN edges client-side.
   - `doc_*` use revision numbers (`1..n`). `doc_put` returns the new revision.
   - Stored procedures: `proc_list / proc_save / proc_delete` using the `Procedure(name, params, body, doc)` dataclass.
6. Update the open-file filter in `window.py::_filters` (currently `*.cmb, *.db, *.combolite`) to the real file extension.

## CMBQL grammar: edit one table
The query language isn't built upstream yet. `combolite_gui/cmbql.py` is a pure-Python lexer plus diagnostics. **The keyword sets at the top (`KEYWORDS`, `STATEMENT_STARTS`, `FUNCTIONS`, `NAME_AFTER`, `CLAUSE_END`) are placeholders.** Replace them with the real grammar. Highlighting, squiggles, did-you-mean fixes, Fix All and the snippet palette (`snippets.py`) all derive from these. `analyze()` returns `Diag(start, end, sev, msg, fix)`, where `fix = (start, end, replacement)`. Run is blocked while any `error` diagnostic exists.

## Screens / Views

**Window shell** (`window.py`)
- `Adw.NavigationSplitView`. The sidebar is 240–320 px wide and collapses below 720 sp. The content area has an `Adw.ToolbarView` header and a 28 px mono status bar. The status bar shows the file path, the size/page/format, and a `MOCK` or `NATIVE` pill.
- The sidebar has a store filter (Ctrl+K). "Overview" and "Query Console" are pinned at the top. Below them, stores are grouped under small caps headings: TABLES, DOCUMENT POOLS, VECTOR COLLECTIONS, LINKS, TIME SERIES. Each row has a coloured model icon, the name, and a mono count.
- Welcome page: open, new, and "Explore Demo Data".
- Toasts report every error and save.

**Overview**: stat cards per model (count, store count), a stores list (click to open), and file info.

**Table view**: tabs Data | Schema | Indexes.
- Paged grid, 100 rows per page.
- Double-click a cell to edit it inline. Edits are committed with `table_update`. On failure the cell reverts and a toast appears.
- Insert-row form dialog.
- Multi-row delete with confirmation.
- Filter box.
- NULL cells are dimmed and italic.

**Document view**: a master/detail split.
- Left: BM25 search box and a list of titles with `id · r<rev>`.
- Right: a JSON editor (mono), a revision dropdown, and Save Revision (Ctrl+S). Save creates a new revision. There are also Copy, Delete and New.

**Vector view**: tabs Search | Graph.
- Search: an entry id or a JSON literal, k, exact/approximate, and a results list with distance bars. Each row has locate-in-graph, edit and delete.
- Graph: see below.

**Links view**: tabs Edges | Graph. The edges grid supports add and delete. The graph is described below.

**Time series view**: an area chart with a hover crosshair, a range dropdown, min/max/avg/points cards, and Append Point.

**Query console**: see the console section below.

### Graph canvas (`graphs.py`), shared by vector and link graphs
- **2D | 3D** toggle (animated blend). **Aa** toggles all labels.
- **3D depth**:
  - Links: depth is the mean normalised weight of a node's links, so heavier means nearer.
  - Vectors: depth is the cosine similarity to the selected node (or to the centroid when nothing is selected), and it re-animates when the selection changes.
  - Nearer nodes are larger, more opaque and have a faint halo. Perspective focal length is 1100 and the depth range is 420 world units.
- **Mouse**:
  - Scroll or pinch zooms. In 2D it zooms about the pointer.
  - In 3D, drag orbits and Shift+drag pans. In 2D, drag pans.
  - Click selects and glides the node to the centre.
  - Double-click on the background fits the view.
  - Dragging a node works in 2D and 3D.
  - The +, − and fit buttons sit at the bottom right.
- **Rubber-band physics** (`ForceLayout`): edges are springs, and heavier links have shorter rest lengths. The integrator is under-damped on purpose, so released nodes jiggle. Stretched bands thin out and turn amber, and slack bands sag sideways. The vector graph uses "home" anchors, so nodes snap back to their PCA positions after a drag.
- **Path-strength heat map**:
  - Selecting or dragging a node runs a strongest-path search to every node within 6 hops. Strengths multiply along the path.
  - Heat is `t = 1 + log10(strength) / 2`, clamped to 0–1.
  - The ramp goes blue `#7aa7f0` → teal `#3cc4c9` → green `#6fc08a` → amber `#d9a85a` → pink `#e88aa3`.
  - Unreachable nodes fade to 14 %.
  - Edges on the strongest-path tree are tinted by their child's heat. The source's direct edges are thickest, at `1.4 + 3.6 × strength` px.
  - A legend strip along the bottom shows the gradient with tick labels (≤0.01, 0.03, 0.1, 0.3, 1.0).
- **Hover context box**: a popover appears for both nodes and edges. `panel.describe(kind, index)` supplies the rows, and the heat or strength row is appended automatically.
- **Selection HUD** (bottom left): the id, a neighbour summary, and actions (Find similar / Edit / Delete, or Link from here… / Unlink all).

**Query console** (`console.py`, `cmbql.py`, `snippets.py`)
- Left tool rail with 6 flyout palettes: Query, Clause `{}`, Logic `?`, Math `Σ`, Directives `#`, Procs `ƒ`. Snippets insert at the cursor, and `|` marks where the caret lands. Edit the table in `snippets.py` to add more.
- Live syntax highlighting using `GtkTextTag`s: keyword, string, number, comment, operator, param, function, and known store names.
- Squiggles (red = error, amber = warning, dotted = info), a hover tooltip, and a problems list with a **Fix** button per item. **Fix All** is Ctrl+. and Run is Ctrl+Enter.
- Chip in the toolbar: "No problems", or "N errors · M warnings".
- **Save as Procedure** stores the editor text. It detects `$params` automatically, requires an identifier name, and refuses to save when there are errors. The Procs flyout lists procedures with Call (inserts `call name($a)`), Body (pastes the query) and Delete.

## Interactions & Behavior
- Debounce: diagnostics refresh 120 ms after the last keystroke.
- Anim timers run at about 60 fps (16 ms) and stop when idle.
- Cross-fade between content pages is 120 ms.
- Destructive actions use a confirm dialog with the red `destructive` response. Cancel is the default.
- Shortcuts:
  - Ctrl+O open
  - Ctrl+N new
  - Ctrl+W close
  - F5 refresh
  - Ctrl+K filter stores
  - Ctrl+E console
  - Ctrl+Enter run
  - Ctrl+. fix all
  - Ctrl+S save document
  - Ctrl+Shift+T toggle theme

## State Management
- The `Backend` object is the single source of truth. Views hold only presentational state (current page offset, selected doc, graph camera).
- `StoreView.call()` and `mutate()` wrap backend calls. They catch `BackendError` and `NotImplementedError` and show a toast.
- `GraphCanvas` keeps nodes as dicts (`id, label, x, y, r, color, z, z_to`). `z` is depth from 0 to 1, where 1 is nearest, and `z_to` is the animation target.

## Design Tokens
Defined in `combolite_gui/style.css` and the `PALETTE` dict in `console.py`.

| Token | Value |
|---|---|
| Accent (teal) | `#2aa9ae` fill, `#3cc4c9` text/line, `#041a1b` text on accent |
| Model: table | `#7aa7f0` |
| Model: document | `#d9a85a` |
| Model: vector | `#b291ea` |
| Model: link | `#6fc08a` |
| Model: time series | `#e88aa3` |
| Dark surfaces (mock) | window `#1b1c1f`, sidebar `#232428`, editor `#18191c` |
| Hairlines | `rgba(255,255,255,0.06–0.12)` dark, `alpha(currentColor, 0.08)` in CSS |
| Error / warning | `#f06a7a` / `#e8b04a` dark; `#c01c28` / `#a06a00` light |
| Syntax (dark) | kw `#b291ea`, str `#6fc08a`, num `#d9a85a`, comment `#7c7f87`, op `#3cc4c9`, param `#e88aa3`, fn `#4fb8e0`, name `#7aa7f0` |
| Mono font | JetBrains Mono (falls back to Cascadia / Fira Code / DejaVu Sans Mono) |
| UI font | system (libadwaita default). The mock uses IBM Plex Sans. |
| Radii | cards 14 px, small cards 10, buttons 8, pills 99 |
| Stat value | 1.9 em, weight 800, tight tracking |
| Section heading | 0.72 em, weight 800, 0.09 em tracking, 50 % opacity |

Theme: dark by default (`Adw.ColorScheme.PREFER_DARK`). Light is supported, and every colour in the cairo drawing code reads `get_color()` or `PALETTE[dark]`.

## Assets
No bitmap assets. Icons are the Adwaita symbolic icon names used in the code. The tool-rail glyphs (Q, {}, ?, Σ, #, ƒ) are text.

## Files
- `combolite_gui/backend.py`: the `Backend` interface, `MockBackend`, and the **`NativeBackend` stub (start here)**
- `combolite_gui/window.py`, `app.py`, `__main__.py`: the shell, actions and shortcuts
- `combolite_gui/views.py`: Overview, Table, Document, Vector, Links and Time series views
- `combolite_gui/graphs.py`: the graph canvas, physics, PCA, path-strength search, and the panel overlays
- `combolite_gui/console.py`, `cmbql.py`, `snippets.py`: the query console, lexer/diagnostics, and the snippet palette
- `combolite_gui/dialogs.py`: the confirm and form dialogs
- `combolite_gui/style.css`: the app CSS
- `USAGE.md`: the original short readme
- `reference/Combolite Studio Preview.dc.html` (+ `support.js`): the HTML visual reference

## Known gaps / suggestions
- The console's **Explain** button is a TODO.
- The Time series view's chart is drawn with cairo and downsampling is not implemented. Add it if series get large.
- Vector graph limit defaults to 220 nodes. The pairwise physics is O(n²), so use numpy and keep graphs under about 500 nodes.
- This code was written without running it on a machine with GTK. Expect a few small PyGObject typing/property fixes on first run.
