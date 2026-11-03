"""One view per data model, plus overview and query console. All views browse + edit."""
from __future__ import annotations

import json
import math
import re
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
import cairo
from gi.repository import Adw, Gio, GObject, Gtk, Pango

try:
    gi.require_version("GtkSource", "5")
    from gi.repository import GtkSource
except (ValueError, ImportError):
    GtkSource = None

from .backend import MODEL_INFO, BackendError, Model, Result
from .dialogs import confirm, form
from .graphs import MODEL_RGB, ForceLayout, GraphPanel, project_2d, similarity_depths

ACCENT = (0.24, 0.77, 0.79)
V = Gtk.Orientation.VERTICAL
EDIT_FAILED = object()


# ---------------------------------------------------------------- helpers
class RowItem(GObject.Object):
    __gtype_name__ = "CmbRowItem"

    def __init__(self, values):
        super().__init__()
        self.values = values


def fmt(v):
    if v is None:
        return "NULL"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:.4g}"
    return str(v)


def short(n):
    for unit, div in (("M", 1_000_000), ("k", 1_000)):
        if n >= div:
            return f"{n / div:.1f}{unit}".replace(".0", "")
    return str(n)


def coerce(text, typ):
    """Text from an editor -> typed value for the engine."""
    t = text.strip()
    typ = (typ or "").upper()
    if t.upper() == "NULL" or (t == "" and typ not in ("TEXT", "")):
        return None
    if typ in ("INT", "INTEGER", "BIGINT"):
        return int(t.replace(",", "").replace("_", ""))
    if typ in ("REAL", "FLOAT", "DOUBLE"):
        return float(t)
    if typ in ("BOOL", "BOOLEAN"):
        if t.lower() in ("true", "t", "1", "yes"):
            return True
        if t.lower() in ("false", "f", "0", "no"):
            return False
        raise ValueError("expected true or false")
    return text


def toolbar(*children):
    box = Gtk.Box(spacing=6, css_classes=["view-toolbar"])
    for c in children:
        box.append(c)
    return box


def spacer():
    return Gtk.Box(hexpand=True)


def icon_button(icon, tip, cb=None):
    b = Gtk.Button(icon_name=icon, tooltip_text=tip, css_classes=["flat"])
    if cb:
        b.connect("clicked", lambda *_: cb())
    return b


def _is_numeric(rows, i):
    for r in rows[:20]:
        if r[i] is not None:
            return isinstance(r[i], (int, float)) and not isinstance(r[i], bool)
    return False


class ResultGrid(Gtk.ScrolledWindow):
    """Result -> Gtk.ColumnView. With on_edit, cells become Gtk.EditableLabel
    (double-click / Enter to edit). on_edit(row_values, col, text) returns the new
    typed value, or EDIT_FAILED to revert."""

    def __init__(self, on_edit=None):
        super().__init__(vexpand=True, hexpand=True)
        self.on_edit = on_edit
        self.model = Gio.ListStore(item_type=RowItem)
        self.view = Gtk.ColumnView(model=Gtk.MultiSelection(model=self.model),
                                   show_row_separators=True, show_column_separators=True,
                                   reorderable=True, css_classes=["data-grid"])
        self.set_child(self.view)

    def _factory(self, i, numeric, editable):
        f = Gtk.SignalListItemFactory()

        def setup(_f, li):
            if editable:
                w = Gtk.EditableLabel(css_classes=["cell", "editable-cell"])
                w.set_alignment(1.0 if numeric else 0.0)
                w.connect("notify::editing", self._commit, i)
            else:
                w = Gtk.Label(xalign=1.0 if numeric else 0.0, ellipsize=Pango.EllipsizeMode.END,
                              max_width_chars=48, css_classes=["cell"])
            w._item = None
            li.set_child(w)

        def bind(_f, li):
            w, item = li.get_child(), li.get_item()
            w._item = item
            v = item.values[i]
            w.set_text(fmt(v))
            (w.add_css_class if v is None else w.remove_css_class)("null-cell")

        def unbind(_f, li):
            li.get_child()._item = None

        f.connect("setup", setup)
        f.connect("bind", bind)
        f.connect("unbind", unbind)
        return f

    def _commit(self, w, _pspec, i):
        if w.get_editing() or w._item is None or not self.on_edit:
            return
        item = w._item
        old, text = item.values[i], w.get_text()
        if text == fmt(old):
            return
        new = self.on_edit(item.values, i, text)
        if new is EDIT_FAILED:
            w.set_text(fmt(old))
            return
        vals = list(item.values)
        vals[i] = new
        item.values = tuple(vals)
        w.set_text(fmt(new))
        (w.add_css_class if new is None else w.remove_css_class)("null-cell")

    def set_result(self, res: Result, editable_cols=()):
        cols = self.view.get_columns()
        while cols.get_n_items():
            self.view.remove_column(cols.get_item(0))
        for i, name in enumerate(res.columns):
            c = Gtk.ColumnViewColumn(title=name, resizable=True, expand=(i == len(res.columns) - 1),
                                     factory=self._factory(i, _is_numeric(res.rows, i), i in editable_cols))
            self.view.append_column(c)
        self.model.remove_all()
        self.model.splice(0, 0, [RowItem(r) for r in res.rows])

    def selected_rows(self):
        sel = self.view.get_model().get_selection()
        return [self.model.get_item(sel.get_nth(k)).values for k in range(sel.get_size())]


class StoreView(Gtk.Box):
    def __init__(self, toast):
        super().__init__(orientation=V)
        self.toast = toast
        self.backend = None
        self.store = None

    def load(self, backend, store):
        self.backend, self.store = backend, store
        self.reload()

    def reload(self):
        pass

    def call(self, fn, *args):
        """Returns fn's value, or None after toasting an error."""
        try:
            return fn(*args)
        except (BackendError, NotImplementedError) as e:
            self.toast(str(e))
            return None

    def mutate(self, fn, *args):
        """Like call() for writes: returns True on success."""
        try:
            fn(*args)
            return True
        except (BackendError, NotImplementedError) as e:
            self.toast(str(e))
            return False

    @property
    def win(self):
        return self.get_root()


def tabbed(stack, *pages):
    for widget, name, title in pages:
        stack.add_titled(widget, name, title)
    return Gtk.StackSwitcher(stack=stack)


# ---------------------------------------------------------------- tables
class TableView(StoreView):
    PAGE = 100

    def __init__(self, toast):
        super().__init__(toast)
        self.offset = 0
        self.cols, self.types = [], []
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vexpand=True)
        self.grid = ResultGrid(on_edit=self._edit)
        self.schema, self.indexes = ResultGrid(), ResultGrid()
        self.filter = Gtk.SearchEntry(placeholder_text="Filter rows…", width_chars=24)
        self.filter.connect("search-changed", lambda *_: self._goto(0))

        self.prev = icon_button("go-previous-symbolic", "Previous page", lambda: self._goto(self.offset - self.PAGE))
        self.next = icon_button("go-next-symbolic", "Next page", lambda: self._goto(self.offset + self.PAGE))
        self.page_lbl = Gtk.Label(css_classes=["mono", "dim-label"])
        self.time_lbl = Gtk.Label(css_classes=["mono", "dim-label"])
        pager = Gtk.Box(spacing=6, css_classes=["pager"])
        for w in (self.time_lbl, spacer(), Gtk.Label(label="Double-click a cell to edit", css_classes=["dim-label"]),
                  Gtk.Separator(orientation=V), self.prev, self.page_lbl, self.next):
            pager.append(w)
        data = Gtk.Box(orientation=V)
        data.append(self.grid)
        data.append(pager)

        switcher = tabbed(self.stack, (data, "data", "Data"), (self.schema, "schema", "Schema"),
                          (self.indexes, "indexes", "Indexes"))
        self.append(toolbar(
            switcher, spacer(), self.filter,
            icon_button("list-add-symbolic", "Insert row", self._insert),
            icon_button("user-trash-symbolic", "Delete selected rows", self._delete),
        ))
        self.append(self.stack)

    def load(self, backend, store):
        self.offset = 0
        super().load(backend, store)

    def _goto(self, offset):
        self.offset = max(0, offset)
        self.reload()

    def reload(self):
        if not self.store:
            return
        schema = self.call(self.backend.table_schema, self.store.name)
        if schema:
            self.schema.set_result(schema)
            self.cols = [r[0] for r in schema.rows]
            self.types = [r[1] for r in schema.rows]
        idx = self.call(self.backend.table_indexes, self.store.name)
        if idx:
            self.indexes.set_result(idx)
        res = self.call(self.backend.table_rows, self.store.name, self.offset, self.PAGE, self.filter.get_text())
        if res:
            self.grid.set_result(res, editable_cols=set(range(1, len(res.columns))))
            total = res.total if res.total is not None else len(res.rows)
            first = self.offset + 1 if res.rows else 0
            self.page_lbl.set_text(f"{first:,}–{self.offset + len(res.rows):,} of {total:,}")
            self.time_lbl.set_text(f"{len(res.rows)} rows · {res.elapsed_ms:.1f} ms")
            self.prev.set_sensitive(self.offset > 0)
            self.next.set_sensitive(self.offset + self.PAGE < total)

    def _edit(self, values, i, text):
        col = self.cols[i] if i < len(self.cols) else str(i)
        try:
            v = coerce(text, self.types[i] if i < len(self.types) else "TEXT")
        except ValueError as e:
            self.toast(f"{col}: {e}")
            return EDIT_FAILED
        if not self.mutate(self.backend.table_update, self.store.name, values[0], col, v):
            return EDIT_FAILED
        self.toast(f"Updated {col} on row {values[0]}")
        return v

    def _insert(self):
        fields = [(c, f"{c} · {t}", None) for c, t in zip(self.cols, self.types) if c != self.cols[0]]
        form(self.win, f"Insert into {self.store.name}", fields, "Insert", self._do_insert,
             body="Type NULL (or leave blank) for null. The primary key is assigned by the engine.")

    def _do_insert(self, vals):
        try:
            typed = {c: coerce(vals[c], t) for c, t in zip(self.cols, self.types) if c in vals}
        except ValueError as e:
            self.toast(str(e))
            return
        pk = self.call(self.backend.table_insert, self.store.name, typed)
        if pk is not None:
            self.toast(f"Inserted row {pk}")
            self.reload()

    def _delete(self):
        rows = self.grid.selected_rows()
        if not rows:
            self.toast("Select rows first (Ctrl/Shift-click for several)")
            return
        pks = [r[0] for r in rows]

        def go():
            n = self.call(self.backend.table_delete, self.store.name, pks)
            if n is not None:
                self.toast(f"Deleted {n} row{'s' if n != 1 else ''}")
                self.reload()

        confirm(self.win, f"Delete {len(pks)} row{'s' if len(pks) != 1 else ''}?",
                f"From {self.store.name}. Link delete rules apply. This can't be undone.", "Delete", go)


# ---------------------------------------------------------------- documents
class DocumentView(StoreView):
    def __init__(self, toast):
        super().__init__(toast)
        self.doc_id = None
        self._loading = False
        self._revs = []
        paned = Gtk.Paned(position=320, vexpand=True, shrink_start_child=False, resize_start_child=False)

        left = Gtk.Box(orientation=V, css_classes=["doc-list-pane"])
        top = Gtk.Box(spacing=6, margin_top=10, margin_start=10, margin_end=10)
        self.search = Gtk.SearchEntry(placeholder_text="Full-text search (BM25)…", hexpand=True)
        self.search.connect("search-changed", lambda *_: self._populate())
        top.append(self.search)
        top.append(icon_button("document-new-symbolic", "New document", self._new))
        self.count_lbl = Gtk.Label(xalign=0, css_classes=["dim-label", "caption"], margin_start=14, margin_top=8, margin_bottom=4)
        self.list = Gtk.ListBox(css_classes=["navigation-sidebar"])
        self.list.connect("row-selected", self._on_doc)
        left.append(top)
        left.append(self.count_lbl)
        left.append(Gtk.ScrolledWindow(vexpand=True, child=self.list, hscrollbar_policy=Gtk.PolicyType.NEVER))

        right = Gtk.Box(orientation=V)
        self.doc_title = Gtk.Label(xalign=0, hexpand=True, ellipsize=Pango.EllipsizeMode.END, css_classes=["heading"])
        self.revs = Gtk.DropDown.new_from_strings([])
        self.revs.connect("notify::selected", self._on_rev)
        save = Gtk.Button(label="Save Revision", css_classes=["suggested-action"], tooltip_text="Save as a new revision (Ctrl+S)")
        save.connect("clicked", lambda *_: self._save())
        right.append(toolbar(self.doc_title, Gtk.Label(label="Revision", css_classes=["dim-label"]), self.revs,
                             icon_button("edit-copy-symbolic", "Copy JSON", self._copy),
                             icon_button("user-trash-symbolic", "Delete document", self._delete), save))
        self.buf = Gtk.TextBuffer()
        text = Gtk.TextView(buffer=self.buf, monospace=True, css_classes=["code-view"],
                            left_margin=18, right_margin=18, top_margin=14, bottom_margin=14)
        sc = Gtk.ShortcutController(propagation_phase=Gtk.PropagationPhase.CAPTURE)
        sc.add_shortcut(Gtk.Shortcut(trigger=Gtk.ShortcutTrigger.parse_string("<Control>s"),
                                     action=Gtk.CallbackAction.new(lambda *_: self._save() or True)))
        text.add_controller(sc)
        right.append(Gtk.ScrolledWindow(vexpand=True, child=text))

        paned.set_start_child(left)
        paned.set_end_child(right)
        self.append(paned)

    def reload(self):
        self._populate()

    def _populate(self, select_id=None):
        while (c := self.list.get_first_child()):
            self.list.remove(c)
        q = self.search.get_text().strip()
        hits = (self.call(self.backend.fts_search, self.store.name, q) if q
                else self.call(self.backend.doc_list, self.store.name)) or []
        self.count_lbl.set_text(f"{len(hits)} matches" if q else f"Showing {len(hits)} of {self.store.count:,}")
        target = None
        for h in hits:
            row = Gtk.ListBoxRow()
            row.doc_id = h.id
            box = Gtk.Box(orientation=V, spacing=2, margin_top=4, margin_bottom=4)
            box.append(Gtk.Label(label=h.title, xalign=0, ellipsize=Pango.EllipsizeMode.END))
            sub = f"{h.id} · score {h.score:.2f}" if h.score is not None else f"{h.id} · r{h.rev}"
            box.append(Gtk.Label(label=sub, xalign=0, css_classes=["mono", "dim-label", "caption"]))
            row.set_child(box)
            self.list.append(row)
            if h.id == select_id:
                target = row
        target = target or self.list.get_row_at_index(0)
        if target:
            self.list.select_row(target)
        else:
            self.doc_id = None
            self.doc_title.set_text("")
            self.buf.set_text("")

    def _on_doc(self, _lb, row):
        if row is None:
            return
        self.doc_id = row.doc_id
        self._revs = self.call(self.backend.doc_revisions, self.store.name, self.doc_id) or [1]
        self._loading = True
        self.revs.set_model(Gtk.StringList.new([f"r{r}" for r in self._revs]))
        self.revs.set_selected(len(self._revs) - 1)
        self._loading = False
        self._show(self._revs[-1])

    def _on_rev(self, *_):
        if not self._loading and self.doc_id and self._revs:
            self._show(self._revs[self.revs.get_selected()])

    def _show(self, rev):
        doc = self.call(self.backend.doc_get, self.store.name, self.doc_id, rev)
        if doc is not None:
            self.doc_title.set_text(str(doc.get("title", self.doc_id)))
            self.buf.set_text(json.dumps(doc, indent=2, ensure_ascii=False))

    def _text(self):
        return self.buf.get_text(self.buf.get_start_iter(), self.buf.get_end_iter(), False)

    def _copy(self):
        self.get_clipboard().set(self._text())
        self.toast("Copied JSON")

    def _save(self):
        if not self.doc_id:
            return
        try:
            doc = json.loads(self._text())
        except json.JSONDecodeError as e:
            self.toast(f"Invalid JSON: {e.msg} (line {e.lineno})")
            return
        n = self.call(self.backend.doc_put, self.store.name, self.doc_id, doc)
        if n:
            self.toast(f"Saved {self.doc_id} as r{n}")
            self._populate(self.doc_id)

    def _new(self):
        doc_id = self.call(self.backend.doc_create, self.store.name, {"title": "Untitled", "subjects": []})
        if doc_id:
            self.search.set_text("")
            self._populate(doc_id)
            self.toast(f"Created {doc_id}")

    def _delete(self):
        if not self.doc_id:
            return
        doc_id = self.doc_id

        def go():
            if self.mutate(self.backend.doc_delete, self.store.name, doc_id):
                self.toast(f"Deleted {doc_id}")
                self._populate()

        confirm(self.win, f"Delete {doc_id}?", "All revisions are removed and links are resolved by their delete rules.", "Delete", go)


# ---------------------------------------------------------------- vectors
class VectorView(StoreView):
    def __init__(self, toast):
        super().__init__(toast)
        self.graph = None
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vexpand=True)
        self.meta = Gtk.Label(css_classes=["mono", "dim-label"])

        # search page
        search = Gtk.Box(orientation=V)
        self.query = Gtk.Entry(placeholder_text="Entry id (vec-00042) or literal [0.12, -0.4, …]", hexpand=True)
        self.query.connect("activate", lambda *_: self.search())
        self.k = Gtk.SpinButton.new_with_range(1, 200, 1)
        self.k.set_value(10)
        self.mode = Gtk.DropDown.new_from_strings(["Exact", "Approximate (graph)"])
        btn = Gtk.Button(label="Search", css_classes=["suggested-action"])
        btn.connect("clicked", lambda *_: self.search())
        qbar = Gtk.Box(spacing=6, margin_top=12, margin_start=12, margin_end=12)
        for w in (self.query, Gtk.Label(label="k", css_classes=["dim-label"]), self.k, self.mode, btn):
            qbar.append(w)
        search.append(qbar)
        self.timing = Gtk.Label(xalign=0, css_classes=["mono", "dim-label", "caption"], margin_start=14, margin_top=8)
        search.append(self.timing)
        self.results = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE, css_classes=["boxed-list"], valign=Gtk.Align.START)
        self.results.set_placeholder(Gtk.Label(label="Run a nearest-neighbour search", css_classes=["dim-label"],
                                               margin_top=24, margin_bottom=24))
        search.append(Gtk.ScrolledWindow(vexpand=True, child=Adw.Clamp(
            maximum_size=920, child=self.results, margin_top=12, margin_bottom=24, margin_start=12, margin_end=12)))

        # graph page
        self.panel = GraphPanel(on_select=self._on_node, draggable=True)

        switcher = tabbed(self.stack, (search, "search", "Search"), (self.panel, "graph", "Graph"))
        self.stack.connect("notify::visible-child-name", lambda *_: self._maybe_load_graph())
        self.append(toolbar(switcher, spacer(), self.meta,
                            icon_button("list-add-symbolic", "Insert / update vector", lambda: self._upsert())))
        self.append(self.stack)

    def reload(self):
        m = self.store.meta
        self.meta.set_text(f"{m.get('dims', '?')} dims · {m.get('metric', '?')} · {m.get('index', '?')}")
        while (c := self.results.get_first_child()):
            self.results.remove(c)
        self.graph = None
        self._maybe_load_graph()

    # search
    def search(self, query=None):
        if query is not None:
            self.query.set_text(query)
            self.stack.set_visible_child_name("search")
        t = time.perf_counter()
        hits = self.call(self.backend.vector_search, self.store.name, self.query.get_text(),
                         int(self.k.get_value()), self.mode.get_selected() == 1)
        if hits is None:
            return
        while (c := self.results.get_first_child()):
            self.results.remove(c)
        worst = max((d for _, d in hits), default=1) or 1
        for rank, (vid, dist) in enumerate(hits, 1):
            row = Adw.ActionRow(title=vid, subtitle=f"distance {dist:.4f}", use_markup=False)
            row.add_prefix(Gtk.Label(label=f"{rank:>2}", css_classes=["mono", "dim-label"]))
            row.add_suffix(Gtk.LevelBar(min_value=0, max_value=1, value=max(0.02, 1 - dist / (worst * 1.15)),
                                        valign=Gtk.Align.CENTER, width_request=160))
            row.add_suffix(icon_button("find-location-symbolic", "Show in graph", lambda v=vid: self._locate(v)))
            row.add_suffix(icon_button("document-edit-symbolic", "Edit vector", lambda v=vid: self._upsert(v)))
            row.add_suffix(icon_button("user-trash-symbolic", "Delete", lambda v=vid: self._delete(v)))
            self.results.append(row)
        self.timing.set_text(f"{len(hits)} results · {(time.perf_counter() - t) * 1000:.1f} ms")

    # graph
    def _maybe_load_graph(self):
        if self.store and self.graph is None and self.stack.get_visible_child_name() == "graph":
            self._load_graph()

    def _load_graph(self):
        g = self.call(self.backend.vector_graph, self.store.name)
        if g is None:
            return
        self.graph = g
        pts = project_2d(g.vectors)
        deg = [0] * len(g.ids)
        for a, b in g.edges:
            deg[a] += 1
            deg[b] += 1
        nodes = [{"id": i, "label": i, "x": x, "y": y, "r": 3.5 + min(math.sqrt(d), 4), "color": MODEL_RGB["vector"]}
                 for i, (x, y), d in zip(g.ids, pts, deg)]
        sims = []
        for a, b in g.edges:
            va, vb = g.vectors[a], g.vectors[b]
            sims.append(sum(x * y for x, y in zip(va, vb)) / ((math.sqrt(sum(x * x for x in va)) * math.sqrt(sum(y * y for y in vb))) or 1))
        lo, hi = min(sims, default=0.0), max(sims, default=1.0)
        es = [0.15 + 0.85 * (s - lo) / ((hi - lo) or 1) for s in sims]
        self.panel.canvas.set_graph(nodes, g.edges, layout=ForceLayout(len(nodes), g.edges, home=pts, anchor=0.06), edge_strengths=es)
        self.panel.info.set_text(f"PCA projection · {len(nodes)} entries · {len(g.edges)} neighbour edges")
        self.panel.canvas.set_depths(similarity_depths(g.vectors), animate=False)
        self.panel.set_caption("Depth = similarity to the selected entry (centroid if none) · nearer = more similar · drag to orbit")
        self.panel.describe = self._describe

    def _locate(self, vid):
        self.stack.set_visible_child_name("graph")
        if not self.panel.canvas.select_id(vid):
            self.toast(f"{vid} isn't in the sampled graph")

    def _describe(self, kind, idx):
        c, g = self.panel.canvas, self.graph
        if kind == "node":
            n = c.nodes[idx]
            return n["id"], [("similarity", f"{n.get('z_to', 0.5):.0%} of range"), ("graph neighbours", str(len(c.adj[idx]))),
                             ("dimensions", str(len(g.vectors[idx])))]
        a, b = c.edges[idx]
        va, vb = g.vectors[a], g.vectors[b]
        dist = 1 - sum(x * y for x, y in zip(va, vb)) / ((math.sqrt(sum(x * x for x in va)) * math.sqrt(sum(y * y for y in vb))) or 1)
        return f"{g.ids[a]} ↔ {g.ids[b]}", [("edge", "neighbour graph"), ("cosine distance", f"{dist:.4f}")]

    def _on_node(self, i):
        if self.graph is not None:
            self.panel.canvas.set_depths(similarity_depths(self.graph.vectors, i))
        if i is None:
            return
        c = self.panel.canvas
        n = c.nodes[i]
        nb = [c.nodes[j]["id"] for j in c.adj[i]]
        self.panel.set_hud(n["id"], f"{len(nb)} graph neighbours: {', '.join(nb[:6])}{'…' if len(nb) > 6 else ''}", [
            ("Find similar", lambda: self.search(n["id"]), ["suggested-action"]),
            ("Edit", lambda: self._upsert(n["id"]), []),
            ("Delete", lambda: self._delete(n["id"]), ["destructive-action"]),
        ])

    # edits
    def _upsert(self, vid=None):
        current = self.call(self.backend.vector_get, self.store.name, vid) if vid else None
        literal = json.dumps([round(x, 4) for x in current]) if current else ""
        form(self.win, "Edit vector" if vid else "Insert vector",
             [("id", "Entry id", vid or ""), ("vec", f"Vector ({self.store.meta.get('dims', '?')} values, JSON array)", literal)],
             "Save", self._do_upsert)

    def _do_upsert(self, vals):
        try:
            vec = [float(x) for x in json.loads(vals["vec"])]
        except (ValueError, TypeError):
            self.toast("Vector must be a JSON array of numbers")
            return
        if not vals["id"].strip():
            self.toast("Entry id is required")
            return
        if self.mutate(self.backend.vector_put, self.store.name, vals["id"].strip(), vec):
            self.toast(f"Saved {vals['id'].strip()}")
            self.graph = None
            self._maybe_load_graph()

    def _delete(self, vid):
        def go():
            if self.mutate(self.backend.vector_delete, self.store.name, vid):
                self.toast(f"Deleted {vid}")
                self.graph = None
                self._maybe_load_graph()
                if self.query.get_text().strip() and self.query.get_text().strip() != vid:
                    self.search()

        confirm(self.win, f"Delete {vid}?", f"From {self.store.name}. The neighbour graph is repaired by the engine.", "Delete", go)


# ---------------------------------------------------------------- links
class LinksView(StoreView):
    def __init__(self, toast):
        super().__init__(toast)
        self.graph_loaded = False
        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, vexpand=True)
        self.meta = Gtk.Label(css_classes=["mono", "dim-label"])
        self.grid = ResultGrid()
        self.panel = GraphPanel(on_select=self._on_node, draggable=True)
        switcher = tabbed(self.stack, (self.grid, "edges", "Edges"), (self.panel, "graph", "Graph"))
        self.stack.connect("notify::visible-child-name", lambda *_: self._maybe_load_graph())
        self.append(toolbar(switcher, spacer(), self.meta,
                            icon_button("list-add-symbolic", "Add link", lambda: self._add()),
                            icon_button("user-trash-symbolic", "Delete selected links", self._delete)))
        self.append(self.stack)

    def reload(self):
        m = self.store.meta
        self.meta.set_text(f"{m.get('from')} → {m.get('to')} · {m.get('cardinality')} · on delete {m.get('on_delete')}")
        res = self.call(self.backend.link_edges, self.store.name)
        if res:
            self.grid.set_result(res)
        self.graph_loaded = False
        self._maybe_load_graph()

    def _maybe_load_graph(self):
        if self.store and not self.graph_loaded and self.stack.get_visible_child_name() == "graph":
            self._load_graph()

    def _load_graph(self):
        res = self.call(self.backend.link_edges, self.store.name, 400)
        if res is None:
            return
        self.graph_loaded = True
        index, out_d, in_d, edges = {}, [], [], []

        def node(nid):
            if nid not in index:
                index[nid] = len(index)
                out_d.append(0)
                in_d.append(0)
            return index[nid]

        ws = []
        for row in res.rows:
            a, b = node(row[0]), node(row[1])
            out_d[a] += 1
            in_d[b] += 1
            edges.append((a, b))
            ws.append(float(row[3]) if len(row) > 3 and row[3] is not None else 0.5)
        ids = list(index)
        src_rgb, dst_rgb, both_rgb = MODEL_RGB["table"], MODEL_RGB["link"], MODEL_RGB["vector"]
        nodes = []
        for nid in ids:
            i = index[nid]
            color = both_rgb if out_d[i] and in_d[i] else (src_rgb if out_d[i] else dst_rgb)
            nodes.append({"id": nid, "label": nid, "x": 0, "y": 0,
                          "r": 3 + min(math.sqrt(out_d[i] + in_d[i]) * 1.4, 9), "color": color})
        self._deg = (out_d, in_d)
        lo, hi = min(ws, default=0.0), max(ws, default=1.0)
        dn = [(w - lo) / ((hi - lo) or 1) for w in ws]
        raw, acc = [[] for _ in nodes], [[] for _ in nodes]
        for (a, b), w, d in zip(edges, ws, dn):
            acc[a].append(d)
            acc[b].append(d)
            raw[a].append(w)
            raw[b].append(w)
        for n, a in zip(nodes, acc):
            n["z"] = n["z_to"] = sum(a) / len(a) if a else 0.5
        self._wraw = [sum(r) / len(r) if r else 0.0 for r in raw]
        self._rows = res.rows
        self.panel.canvas.set_graph(nodes, edges, layout=ForceLayout(len(nodes), edges, weights=dn), directed=True, weights=dn,
                                      edge_strengths=[0.08 + 0.92 * d for d in dn])
        self.panel.info.set_text(f"{len(nodes)} entries · {len(edges)} links · drag nodes to rearrange")
        self.panel.set_caption("Depth = link weight · nearer = more reinforced · drag to orbit, Shift+drag to pan")
        self.panel.describe = self._describe

    def _describe(self, kind, idx):
        c = self.panel.canvas
        out_d, in_d = self._deg
        if kind == "node":
            return c.nodes[idx]["id"], [("out / in", f"{out_d[idx]} / {in_d[idx]}"), ("mean weight", f"{self._wraw[idx]:.3f}"),
                                        ("linked to", str(len(c.adj[idx])))]
        row = self._rows[idx]
        return f"{row[0]} → {row[1]}", [("weight", f"{row[3]:.3f}" if len(row) > 3 else "?"),
                                        ("created", str(row[2]) if len(row) > 2 else "?"),
                                        ("depth", f"{c.weights[idx]:.0%} of range")]

    def _on_node(self, i):
        if i is None:
            return
        c = self.panel.canvas
        nid = c.nodes[i]["id"]
        out_d, in_d = self._deg
        nb = [c.nodes[j]["id"] for j in c.adj[i]]
        self.panel.set_hud(nid, f"out {out_d[i]} · in {in_d[i]} · weight {self._wraw[i]:.2f} — {', '.join(nb[:5])}{'…' if len(nb) > 5 else ''}", [
            ("Link from here…", lambda: self._add(nid), ["suggested-action"]),
            ("Unlink all", lambda: self._unlink_node(nid), ["destructive-action"]),
        ])

    def _add(self, source=""):
        m = self.store.meta
        form(self.win, f"Add link to {self.store.name}",
             [("source", f"Source ({m.get('from', '?')})", source), ("target", f"Target ({m.get('to', '?')})", ""),
              ("weight", "Weight (higher = stronger / nearer in 3D)", "1.0")],
             "Link", lambda v: self._do_add(v["source"], v["target"], v["weight"]),
             body=f"Cardinality {m.get('cardinality', '?')} is enforced by the engine.")

    def _do_add(self, s, t, w="1.0"):
        try:
            wt = float(w)
        except ValueError:
            self.toast("Weight must be a number")
            return
        if self.mutate(self.backend.link_add, self.store.name, s, t, wt):
            self.toast(f"Linked {s} → {t}")
            self.reload()

    def _delete(self):
        pairs = [(r[0], r[1]) for r in self.grid.selected_rows()]
        if not pairs:
            self.toast("Select links in the Edges tab first")
            return
        self._confirm_unlink(pairs, f"Delete {len(pairs)} link{'s' if len(pairs) != 1 else ''}?")

    def _unlink_node(self, nid):
        res = self.call(self.backend.link_edges, self.store.name, 100000)
        pairs = [(r[0], r[1]) for r in (res.rows if res else []) if nid in (r[0], r[1])]
        self._confirm_unlink(pairs, f"Remove all {len(pairs)} links of {nid}?")

    def _confirm_unlink(self, pairs, heading):
        def go():
            n = self.call(self.backend.link_delete, self.store.name, pairs)
            if n is not None:
                self.toast(f"Removed {n} link{'s' if n != 1 else ''}")
                self.reload()

        confirm(self.win, heading, "Entries themselves are kept.", "Remove", go)


# ---------------------------------------------------------------- time series
class TimeSeriesView(StoreView):
    RANGES = [("Last hour", 3600), ("Last 24 hours", 86400), ("Last 7 days", 604800), ("Last 30 days", 2592000)]

    def __init__(self, toast):
        super().__init__(toast)
        self.points = []
        self.hover_x = None
        self.range = Gtk.DropDown.new_from_strings([r[0] for r in self.RANGES])
        self.range.set_selected(1)
        self.range.connect("notify::selected", lambda *_: self.reload())
        self.meta = Gtk.Label(xalign=0, hexpand=True, css_classes=["mono", "dim-label"])
        append = Gtk.Button(child=Adw.ButtonContent(icon_name="list-add-symbolic", label="Append Point"), css_classes=["flat"])
        append.connect("clicked", lambda *_: self._append())
        self.append(toolbar(self.meta, append, self.range))
        self.area = Gtk.DrawingArea(vexpand=True, hexpand=True, css_classes=["chart"])
        self.area.set_draw_func(self._draw)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", lambda _c, x, _y: self._hover(x))
        motion.connect("leave", lambda *_: self._hover(None))
        self.area.add_controller(motion)
        self.append(self.area)
        self.stats = Gtk.Box(spacing=12, homogeneous=True, css_classes=["ts-stats"])
        self.append(self.stats)

    def _hover(self, x):
        self.hover_x = x
        self.area.queue_draw()

    def reload(self):
        m = self.store.meta
        self.meta.set_text(f"{m.get('unit', '')} · late arrival: {m.get('late_arrival', '?')} · {self.store.count:,} points")
        self.points = self.call(self.backend.ts_points, self.store.name, self.RANGES[self.range.get_selected()][1]) or []
        while (c := self.stats.get_first_child()):
            self.stats.remove(c)
        if self.points:
            vs = [v for _, v in self.points]
            for label, val in (("min", min(vs)), ("max", max(vs)), ("avg", sum(vs) / len(vs)), ("points", len(vs))):
                card = Gtk.Box(orientation=V, css_classes=["stat-card", "small"])
                card.append(Gtk.Label(label=label.upper(), xalign=0, css_classes=["stat-label"]))
                card.append(Gtk.Label(label=f"{val:,.1f}" if label != "points" else str(val), xalign=0, css_classes=["stat-value", "mono"]))
                self.stats.append(card)
        self.area.queue_draw()

    def _append(self):
        form(self.win, f"Append to {self.store.name}",
             [("t", "Timestamp (YYYY-MM-DD HH:MM or epoch seconds)", time.strftime("%Y-%m-%d %H:%M")),
              ("v", f"Value ({self.store.meta.get('unit', '')})", "")],
             "Append", self._do_append, body=f"Late-arrival mode: {self.store.meta.get('late_arrival', '?')}.")

    def _do_append(self, vals):
        try:
            ts = vals["t"].strip()
            t = float(ts) if re.fullmatch(r"\d+(\.\d+)?", ts) else time.mktime(time.strptime(ts, "%Y-%m-%d %H:%M"))
            v = float(vals["v"])
        except ValueError:
            self.toast("Expected a timestamp and a number")
            return
        if self.mutate(self.backend.ts_append, self.store.name, t, v):
            self.toast("Point appended")
            self.reload()

    def _draw(self, area, cr, w, h):
        pts = self.points
        if len(pts) < 2:
            return
        pl, pr, pt, pb = 52, 16, 16, 26
        vs = [v for _, v in pts]
        lo, hi = min(vs), max(vs)
        pad = (hi - lo) * 0.1 or 1
        lo, hi = lo - pad, hi + pad
        t0, t1 = pts[0][0], pts[-1][0]
        X = lambda t: pl + (t - t0) / ((t1 - t0) or 1) * (w - pl - pr)
        Y = lambda v: pt + (1 - (v - lo) / (hi - lo)) * (h - pt - pb)
        fg = area.get_color()
        cr.select_font_face("monospace")
        cr.set_font_size(10)
        cr.set_line_width(1)
        for i in range(5):
            v = lo + (hi - lo) * i / 4
            y = round(Y(v)) + 0.5
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.07)
            cr.move_to(pl, y)
            cr.line_to(w - pr, y)
            cr.stroke()
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.5)
            cr.move_to(8, y + 3)
            cr.show_text(f"{v:.0f}")
        fmt_t = "%H:%M" if t1 - t0 <= 86400 else "%b %d"
        for t, align in ((t0, 0), (t1, 1)):
            s = time.strftime(fmt_t, time.localtime(t))
            cr.move_to(X(t) - align * cr.text_extents(s).width, h - 8)
            cr.show_text(s)

        def path():
            cr.move_to(X(pts[0][0]), Y(pts[0][1]))
            for t, v in pts[1:]:
                cr.line_to(X(t), Y(v))

        path()
        cr.line_to(X(t1), h - pb)
        cr.line_to(X(t0), h - pb)
        cr.close_path()
        g = cairo.LinearGradient(0, pt, 0, h - pb)
        g.add_color_stop_rgba(0, *ACCENT, 0.32)
        g.add_color_stop_rgba(1, *ACCENT, 0.0)
        cr.set_source(g)
        cr.fill()
        path()
        cr.set_source_rgb(*ACCENT)
        cr.set_line_width(2)
        cr.set_line_join(cairo.LineJoin.ROUND)
        cr.stroke()

        if self.hover_x is not None and pl <= self.hover_x <= w - pr:
            t_at = t0 + (self.hover_x - pl) / (w - pl - pr) * (t1 - t0)
            t, v = min(pts, key=lambda p: abs(p[0] - t_at))
            x, y = X(t), Y(v)
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.25)
            cr.set_line_width(1)
            cr.move_to(round(x) + 0.5, pt)
            cr.line_to(round(x) + 0.5, h - pb)
            cr.stroke()
            cr.set_source_rgb(*ACCENT)
            cr.arc(x, y, 4, 0, 2 * math.pi)
            cr.fill()
            label = f"{time.strftime('%b %d %H:%M', time.localtime(t))}  {v:,.2f}"
            ext = cr.text_extents(label)
            bx = min(x + 10, w - pr - ext.x_advance - 8)
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.9)
            cr.move_to(bx, pt + 12)
            cr.show_text(label)


# ---------------------------------------------------------------- overview
class OverviewView(Gtk.ScrolledWindow):
    def __init__(self, on_open_store):
        super().__init__(vexpand=True)
        self.on_open_store = on_open_store
        box = Gtk.Box(orientation=V, spacing=24)
        self.title = Gtk.Label(xalign=0, css_classes=["title-1"])
        self.subtitle = Gtk.Label(xalign=0, css_classes=["mono", "dim-label"], ellipsize=Pango.EllipsizeMode.MIDDLE)
        head = Gtk.Box(orientation=V, spacing=4)
        head.append(self.title)
        head.append(self.subtitle)
        box.append(head)
        self.cards = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, column_spacing=12, row_spacing=12,
                                 min_children_per_line=2, max_children_per_line=5, homogeneous=True)
        box.append(self.cards)
        self.stores_group = Adw.PreferencesGroup(title="Stores")
        self.file_group = Adw.PreferencesGroup(title="File")
        box.append(self.stores_group)
        box.append(self.file_group)
        self._rows = []
        self.set_child(Adw.Clamp(maximum_size=1000, child=box, margin_top=32, margin_bottom=32, margin_start=24, margin_end=24))

    def load(self, backend):
        info = backend.file_info()
        stores = backend.list_stores()
        path = info.get("Path") or ""
        self.title.set_text(path.rsplit("/", 1)[-1] or "Database")
        self.subtitle.set_text(f"{path} · {info.get('Size', '')}")
        while (c := self.cards.get_first_child()):
            self.cards.remove(c)
        for row, group in self._rows:
            group.remove(row)
        self._rows = []
        for m in Model:
            ms = [s for s in stores if s.model == m]
            heading, _single, icon = MODEL_INFO[m]
            card = Gtk.Box(orientation=V, spacing=4, css_classes=["stat-card", f"model-{m.value}"])
            card.append(Gtk.Image(icon_name=icon, halign=Gtk.Align.START, css_classes=["model-icon", f"model-{m.value}"]))
            card.append(Gtk.Label(label=short(sum(s.count for s in ms)), xalign=0, css_classes=["stat-value", "mono"]))
            card.append(Gtk.Label(label=heading.title(), xalign=0))
            card.append(Gtk.Label(label=f"{len(ms)} store{'s' if len(ms) != 1 else ''}", xalign=0, css_classes=["dim-label", "caption"]))
            self.cards.append(card)
        for s in stores:
            row = Adw.ActionRow(title=s.name, subtitle=MODEL_INFO[s.model][1], activatable=True, use_markup=False)
            row.add_prefix(Gtk.Image(icon_name=MODEL_INFO[s.model][2], css_classes=["model-icon", f"model-{s.model.value}"]))
            row.add_suffix(Gtk.Label(label=f"{s.count:,}", css_classes=["mono", "dim-label"]))
            row.add_suffix(Gtk.Image(icon_name="go-next-symbolic"))
            row.connect("activated", lambda _r, st=s: self.on_open_store(st))
            self.stores_group.add(row)
            self._rows.append((row, self.stores_group))
        for k, v in info.items():
            row = Adw.ActionRow(title=k, use_markup=False)
            row.add_suffix(Gtk.Label(label=str(v), css_classes=["mono", "dim-label"], selectable=True))
            self.file_group.add(row)
            self._rows.append((row, self.file_group))


from .console import ConsoleView  # noqa: E402,F401  (defined in console.py)
