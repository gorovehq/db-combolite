# Combolite Studio — GTK4 skeleton for db-combolite

Python + GTK4 + libadwaita desktop client. Every view runs today against a mock backend; swap in the real engine by filling in `NativeBackend`.

## Run

```sh
# Fedora
sudo dnf install python3-gobject gtk4 libadwaita gtksourceview5
# Debian / Ubuntu 24.04+
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-4.0 gir1.2-adw-1 gir1.2-gtksource-5

python3 -m combolite_gui                       # mock data
python3 -m combolite_gui path/to/file.cmb      # open a file directly
COMBOLITE_LIB=./build/libcombolite.so python3 -m combolite_gui   # native backend
```

Needs GTK ≥ 4.10 and libadwaita ≥ 1.4. GtkSourceView is optional (the console falls back to a plain text view).

## Layout

- `combolite_gui/backend.py`: the `Backend` interface, plus `MockBackend` and a stub `NativeBackend`. This is the only file that touches the engine.
- `combolite_gui/window.py`: the main window, with a sidebar of stores grouped by model, a content stack, the status bar, and actions/shortcuts.
- `combolite_gui/views.py`: one view per model. Every view lets you browse and edit:
  - **Tables**: paged grid with inline cell editing (double-click), insert-row form, multi-row delete, plus Schema and Indexes tabs.
  - **Document pools**: BM25 search, a JSON editor with a revision picker, Save (Ctrl+S), New, and Delete.
  - **Vector collections**: k-NN search (exact or approximate) with insert, edit and delete. A **Graph** tab shows a PCA map of the neighbour graph; click a node to see its neighbours and run Find similar, Edit or Delete.
  - **Links**: an edges grid with add and delete. A **Graph** tab shows a force-directed link visualiser where you can drag nodes and see in/out degree, then Link from here or Unlink all. The engine enforces cardinality.
  - **Time series**: a chart with a hover crosshair, range picker and Append point.
  - **Overview** and the **CMBQL console**.
- `combolite_gui/cmbql.py`: the CMBQL lexer and diagnostics (no GTK). Edit the keyword tables at the top to match the real grammar. It flags unterminated strings and comments, bad numbers, unmatched brackets, unknown or mistyped keywords, unknown store names (with did-you-mean), `= null`, repeated keywords, empty conditions and missing `limit`. Each diagnostic can carry a quick-fix.
- `combolite_gui/console.py`: the query console. It has live syntax highlighting, squiggles with hover tooltips, a problems list, per-item Fix and Fix All (Ctrl+.), and Run, which is blocked while errors remain.
- `combolite_gui/snippets.py`: the console's left tool rail (Query, Clause, Logic, Math, Directives, Procs). Each button opens a flyout of snippets that insert at the cursor, with `|` marking where the cursor lands. Edit the table to change the palette.
- Stored procedures: **Save as Procedure** stores the editor text with its `$params` through `Backend.proc_save`. The **Procs** flyout can insert `call name($a)` or paste the procedure's body as a template, and can delete procedures. Implement `proc_list`, `proc_save` and `proc_delete` in `NativeBackend`.
- `combolite_gui/graphs.py`: `GraphCanvas`, which handles pan, zoom, select, node dragging and neighbour highlighting. Also contains `ForceLayout` and `project_2d` (PCA). It uses numpy if it's installed and falls back to pure Python otherwise.
- 2D/3D and labels: the graph tabs have a **2D | 3D** switch and an **Aa** labels toggle. 3D uses depth. In the **link graph**, link weight sets the depth, so reinforced links and nodes sit nearer the viewer (and are thicker and brighter). In the **vector graph**, cosine similarity to the selected entry (or the centroid) sets the depth, and re-selecting a node animates the re-ranking. Selecting or dragging a node paints a path-strength heat map (up to 6 hops) across both graphs. Edge strengths multiply along the strongest path. The ramp uses the app palette, going from blue (weak) through teal, green and amber to pink (strong), and nodes with no path fade out. Immediate relations are drawn thicker in proportion to their strength. Scroll or pinch zooms, and the +/− buttons do too. Drag orbits in 3D or pans in 2D, and Shift+drag pans in 3D. Clicking a node glides it to the centre, and double-clicking the background fits the view. Edges are rubber bands, so you can drag any node (in 2D or 3D) and its neighbours follow with a spring jiggle. Stretched bands thin out and turn amber, slack bands sag, and vector nodes snap back to their PCA positions on release. Hovering a node or a link opens a context box, and `panel.describe(kind, index)` controls what it shows. `link_edges` rows now carry a `weight` column, and `link_add` takes a `weight`.
- `combolite_gui/dialogs.py`: confirm and form dialogs that work on libadwaita 1.4 and 1.5+.
- `combolite_gui/style.css`: the accent colour, per-model colours and the app's CSS classes.

## Wiring the engine

Implement `NativeBackend` methods one by one. Any method you haven't implemented raises `NotImplementedError`, and the UI shows that as a toast instead of crashing. If you raise `BackendError("…")`, the user sees your message.
The core doesn't allocate, so keep caller-owned buffers on `self` for as long as the handle lives.

## Shortcuts

- Ctrl+O: open
- Ctrl+N: new
- Ctrl+W: close
- F5: refresh
- Ctrl+K: filter stores
- Ctrl+E: console
- Ctrl+Enter: run query
- Ctrl+Shift+T: toggle theme
