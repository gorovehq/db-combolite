"""CMBQL query console: syntax highlighting, live diagnostics (squiggles, tooltips,
quick-fixes) and a results grid. Highlighting uses GtkTextTags so it works with or
without GtkSourceView (which, when present, only adds line numbers)."""
from __future__ import annotations

import re

from gi.repository import Adw, Gdk, GLib, Gtk, Pango

try:
    import gi
    gi.require_version("GtkSource", "5")
    from gi.repository import GtkSource
except (ValueError, ImportError):
    GtkSource = None

from . import cmbql
from .backend import BackendError, Procedure
from .dialogs import confirm, form
from .snippets import CATS
from .views import ResultGrid, icon_button, spacer, toolbar

V = Gtk.Orientation.VERTICAL

PALETTE = {
    True: dict(kw="#b291ea", str="#6fc08a", num="#d9a85a", comment="#7c7f87", name="#7aa7f0", op="#3cc4c9",
               param="#e88aa3", fn="#4fb8e0", error="#f06a7a", warning="#e8b04a", info="#6c7078"),
    False: dict(kw="#7a4fd0", str="#2b7d4a", num="#9a6a10", comment="#8a8d94", name="#2f62c4", op="#0d7f84",
                param="#c0405f", fn="#1a78a8", error="#c01c28", warning="#a06a00", info="#9a9da3"),
}
TOKEN_TAG = {"kw": "kw", "str": "str", "num": "num", "comment": "comment", "op": "op", "param": "param", "fn": "fn", "error": "error"}
ICON = {"error": "dialog-error-symbolic", "warning": "dialog-warning-symbolic", "info": "dialog-information-symbolic"}

SAMPLE = """-- CMBQL: edit the grammar tables in cmbql.py when the query core lands.
from books
where year < 1850 and lang = 'en'
order by downloads desc
limit 20
"""


def rgba(h):
    c = Gdk.RGBA()
    c.parse(h)
    return c


class ConsoleView(Gtk.Box):
    def __init__(self, toast):
        super().__init__(orientation=V)
        self.toast = toast
        self.backend = None
        self.diags: list = []
        self._timer = None
        self._names, self._names_key = (), None

        if GtkSource:
            self.buf = GtkSource.Buffer(highlight_syntax=False)
            self.view = GtkSource.View(buffer=self.buf, show_line_numbers=True, highlight_current_line=True,
                                       monospace=True, tab_width=2, auto_indent=True, insert_spaces_instead_of_tabs=True)
        else:
            self.buf = Gtk.TextBuffer()
            self.view = Gtk.TextView(buffer=self.buf, monospace=True)
        self.view.add_css_class("code-view")
        self.view.set_top_margin(12)
        self.view.set_bottom_margin(12)
        self.view.set_left_margin(12)
        self.view.set_right_margin(12)
        self.view.set_has_tooltip(True)
        self.view.connect("query-tooltip", self._tooltip)

        self.tags = {n: self.buf.create_tag(f"t-{n}") for n in ("kw", "str", "num", "comment", "op", "param", "name", "fn")}
        self.tags["comment"].set_property("style", Pango.Style.ITALIC)
        self.tags["kw"].set_property("weight", 700)
        self.dtags = {s: self.buf.create_tag(f"d-{s}", underline=Pango.Underline.ERROR if s != "info" else Pango.Underline.SINGLE)
                      for s in ("error", "warning", "info")}
        sm = Adw.StyleManager.get_default()
        sm.connect("notify::dark", lambda *_: self._recolor())
        self._recolor()

        self.buf.connect("changed", lambda *_: self._schedule())
        sc = Gtk.ShortcutController(propagation_phase=Gtk.PropagationPhase.CAPTURE)
        for trig, fn in (("<Control>Return", self.run), ("<Control>period", self.fix_all)):
            sc.add_shortcut(Gtk.Shortcut(trigger=Gtk.ShortcutTrigger.parse_string(trig),
                                         action=Gtk.CallbackAction.new(lambda *_a, f=fn: f() or True)))
        self.view.add_controller(sc)

        # toolbar
        self.chip = Gtk.Label(css_classes=["diag-chip", "ok"])
        self.fix_btn = Gtk.Button(label="Fix All", tooltip_text="Apply every quick-fix (Ctrl+.)", css_classes=["flat"])
        self.fix_btn.connect("clicked", lambda *_: self.fix_all())
        run = Gtk.Button(child=Adw.ButtonContent(icon_name="media-playback-start-symbolic", label="Run"),
                         tooltip_text="Run (Ctrl+Enter)", css_classes=["suggested-action"])
        run.connect("clicked", lambda *_: self.run())
        save_proc = Gtk.Button(child=Adw.ButtonContent(icon_name="document-save-symbolic", label="Save as Procedure"),
                               tooltip_text="Store this query as a reusable procedure", css_classes=["flat"])
        save_proc.connect("clicked", lambda *_: self._save_proc_dialog())
        explain = Gtk.Button(label="Explain", tooltip_text="Explain query execution plan", css_classes=["flat"])
        explain.connect("clicked", lambda *_: self.explain())
        self.append(toolbar(Gtk.Label(label="CMBQL", css_classes=["heading"]),
                            Gtk.Label(label="PREVIEW", css_classes=["pill-badge"]), self.chip, spacer(),
                            icon_button("edit-clear-all-symbolic", "Clear editor", self.clear_editor),
                            self.fix_btn, save_proc, explain, run))

        # editor / diagnostics / results
        self.paned = Gtk.Paned(orientation=V, position=340, vexpand=True, wide_handle=True,
                               shrink_start_child=False, shrink_end_child=False,
                               resize_start_child=True, resize_end_child=True)
        self.paned.add_css_class("console-paned")
        editor = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, vexpand=True)
        rail_sw = Gtk.ScrolledWindow(child=self._build_rail(), hscrollbar_policy=Gtk.PolicyType.NEVER,
                                     vscrollbar_policy=Gtk.PolicyType.AUTOMATIC)
        editor.append(rail_sw)
        editor_sw = Gtk.ScrolledWindow(child=self.view, hexpand=True, vexpand=True, min_content_height=140)
        editor.append(editor_sw)
        self.paned.set_start_child(editor)

        bottom = Gtk.Box(orientation=V, vexpand=True)
        self.diag_list = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE, css_classes=["diag-list"])
        self.diag_list.connect("row-activated", lambda _l, row: self._jump(row.diag))
        self.revealer = Gtk.Revealer(child=Gtk.ScrolledWindow(child=self.diag_list, max_content_height=130,
                                                              propagate_natural_height=True,
                                                              hscrollbar_policy=Gtk.PolicyType.NEVER))
        bottom.append(self.revealer)
        self.msg = Gtk.Label(xalign=0, css_classes=["mono", "dim-label", "console-msg"], wrap=True, label="Ready.")
        bottom.append(self.msg)
        self.grid = ResultGrid()
        bottom.append(self.grid)
        self.paned.set_end_child(bottom)
        self.append(self.paned)

        self.buf.set_text(SAMPLE)

    def clear_editor(self):
        self.buf.set_text("")
        self.view.grab_focus()

    # ---- tool rail (Photoshop-style palette)
    def _build_rail(self):
        rail = Gtk.Box(orientation=V, spacing=2, css_classes=["tool-rail"])
        for cat in CATS:
            pop = Gtk.Popover(css_classes=["tool-flyout"], position=Gtk.PositionType.RIGHT)
            if cat.key == "procs":
                pop.connect("show", lambda p: p.set_child(self._procs_panel(p)))
            else:
                pop.set_child(self._snippet_panel(cat, pop))
            inner = Gtk.Box(orientation=V, halign=Gtk.Align.CENTER)
            inner.append(Gtk.Label(label=cat.glyph, css_classes=["rail-glyph"]))
            inner.append(Gtk.Label(label=cat.label, css_classes=["rail-label"]))
            rail.append(Gtk.MenuButton(child=inner, popover=pop, direction=Gtk.ArrowType.RIGHT,
                                       tooltip_text=cat.tip, css_classes=["flat", "rail-btn"]))
        return rail

    def _snippet_panel(self, cat, pop):
        box = Gtk.Box(orientation=V, spacing=8, margin_top=10, margin_bottom=10, margin_start=10, margin_end=10)
        box.append(Gtk.Label(label=cat.tip.upper(), xalign=0, css_classes=["sidebar-heading"], margin_top=0, margin_start=2))
        flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, min_children_per_line=3, max_children_per_line=3,
                           column_spacing=4, row_spacing=4, homogeneous=True)
        for label, text, mode, tip in cat.snippets:
            b = Gtk.Button(label=label, tooltip_text=tip or text.replace("|", ""), css_classes=["snippet-btn", "mono"])
            b.connect("clicked", lambda _b, t=text, m=mode: (pop.popdown(), self._insert(t, m)))
            flow.append(b)
        box.append(flow)
        return box

    def _insert(self, text, mode):
        buf = self.buf
        mark = text.find("|")
        text = text.replace("|", "")
        if mode == "stmt":
            n = buf.get_char_count()
            lead = "\n\n" if self._text().strip() else ""
            base = n + len(lead)
            buf.insert(buf.get_end_iter(), lead + text + "\n")
        elif mode == "prefix":
            base = 0
            buf.insert(buf.get_start_iter(), text)
        else:
            it = buf.get_iter_at_mark(buf.get_insert())
            lead = ""
            if not it.is_start():
                p = it.copy()
                p.backward_char()
                c = p.get_char()
                lead = "" if c.isspace() or c == "(" or text[:1] in ")" else " "
            base = it.get_offset() + len(lead)
            buf.insert(it, lead + text)
        buf.place_cursor(buf.get_iter_at_offset(base + (mark if mark >= 0 else len(text))))
        self.view.grab_focus()

    def _procs_panel(self, pop):
        box = Gtk.Box(orientation=V, spacing=8, margin_top=10, margin_bottom=10, margin_start=10, margin_end=10, width_request=360)
        box.append(Gtk.Label(label="STORED PROCEDURES", xalign=0, css_classes=["sidebar-heading"], margin_top=0, margin_start=2))
        try:
            procs = self.backend.proc_list()
        except (BackendError, NotImplementedError) as e:
            procs = []
            box.append(Gtk.Label(label=str(e), xalign=0, wrap=True, css_classes=["dim-label"]))
        lb = Gtk.ListBox(selection_mode=Gtk.SelectionMode.NONE, css_classes=["boxed-list"])
        lb.set_placeholder(Gtk.Label(label="No procedures yet", css_classes=["dim-label"], margin_top=12, margin_bottom=12))
        for p in procs:
            sig = f"{p.name}({', '.join(p.params)})"
            row = Adw.ActionRow(title=sig, subtitle=p.doc or "", use_markup=False, title_lines=1)
            row.add_css_class("mono")
            call = Gtk.Button(label="Call", valign=Gtk.Align.CENTER, css_classes=["suggested-action", "small-btn"], tooltip_text="Insert a call")
            call.connect("clicked", lambda _b, s=sig: (pop.popdown(), self._insert(f"call {s}", "stmt")))
            body = Gtk.Button(label="Body", valign=Gtk.Align.CENTER, css_classes=["small-btn"], tooltip_text="Insert the procedure's query as a template")
            body.connect("clicked", lambda _b, p=p: (pop.popdown(), self._insert(p.body, "stmt")))
            trash = icon_button("user-trash-symbolic", "Delete procedure", lambda p=p: (pop.popdown(), self._delete_proc(p.name)))
            for w in (call, body, trash):
                row.add_suffix(w)
            lb.append(row)
        box.append(Gtk.ScrolledWindow(child=lb, max_content_height=300, propagate_natural_height=True,
                                      hscrollbar_policy=Gtk.PolicyType.NEVER))
        save = Gtk.Button(label="Save Current Query as Procedure…", css_classes=["pill"])
        save.connect("clicked", lambda *_: (pop.popdown(), self._save_proc_dialog()))
        box.append(save)
        return box

    def _save_proc_dialog(self):
        body = self._text().strip()
        if not body:
            self.toast("Write a query first")
            return
        params = list(dict.fromkeys(re.findall(r"\$[A-Za-z_]\w*", body)))
        form(self.get_root(), "Store query as procedure",
             [("name", "Name", ""), ("params", "Parameters (detected from $names)", ", ".join(params)), ("doc", "Description", "")],
             "Save", self._do_save_proc, body="Call it later with  call name($arg, …)  or insert its body from the Procs palette.")

    def _do_save_proc(self, v):
        name = v["name"].strip()
        if not re.fullmatch(r"[A-Za-z_]\w*", name) or name.lower() in cmbql.KEYWORDS:
            self.toast("Name must be an identifier (letters, digits, _) and not a keyword")
            return
        self._refresh()
        if any(d.sev == "error" for d in self.diags):
            self.toast("Fix the errors in the query before storing it")
            return
        params = [p.strip() for p in v["params"].split(",") if p.strip()]
        try:
            self.backend.proc_save(Procedure(name, params, self._text().strip(), v["doc"].strip()))
        except (BackendError, NotImplementedError) as e:
            self.toast(str(e))
            return
        self.toast(f"Stored procedure {name}")
        self._refresh()

    def _delete_proc(self, name):
        def go():
            try:
                self.backend.proc_delete(name)
                self.toast(f"Deleted {name}")
            except (BackendError, NotImplementedError) as e:
                self.toast(str(e))
        confirm(self.get_root(), f"Delete procedure {name}?", "Queries that call it will stop working.", "Delete", go)

    def grab_focus(self):
        return self.view.grab_focus()

    # ---- theme
    def _recolor(self):
        p = PALETTE[Adw.StyleManager.get_default().get_dark()]
        self.palette = p
        for n, t in self.tags.items():
            t.set_property("foreground", p[n if n in p else "name"])
        for s, t in self.dtags.items():
            t.set_property("underline-rgba", rgba(p[s]))

    # ---- analysis
    def _store_names(self):
        key = getattr(self.backend, "path", None)
        if self.backend is not None and key != self._names_key:
            try:
                self._names = [s.name for s in self.backend.list_stores()] if key else []
            except Exception:
                self._names = []
            self._names_key = key
        try:
            procs = [p.name for p in self.backend.proc_list()] if key else []
        except Exception:
            procs = []
        return list(self._names) + procs

    def _schedule(self):
        if self._timer:
            GLib.source_remove(self._timer)
        self._timer = GLib.timeout_add(120, self._refresh)

    def _text(self):
        return self.buf.get_text(self.buf.get_start_iter(), self.buf.get_end_iter(), False)

    def _refresh(self):
        self._timer = None
        text = self._text()
        names = self._store_names()
        known = {n.lower() for n in names}
        b = self.buf
        b.remove_all_tags(b.get_start_iter(), b.get_end_iter())
        at = b.get_iter_at_offset
        for t in cmbql.tokenize(text):
            tag = TOKEN_TAG.get(t.kind)
            if t.kind == "ident" and t.text.lower() in known:
                tag = "name"
            if tag in self.tags:
                b.apply_tag(self.tags[tag], at(t.start), at(t.end))
        self.diags = cmbql.analyze(text, names)
        for d in self.diags:
            end = d.end if d.end > d.start else min(d.start + 1, len(text))
            b.apply_tag(self.dtags[d.sev], at(d.start), at(end))
        self._render_diags(text)
        return False

    def _pos(self, text, off):
        line = text.count("\n", 0, off) + 1
        return line, off - (text.rfind("\n", 0, off) + 1) + 1

    def _render_diags(self, text):
        while (c := self.diag_list.get_first_child()):
            self.diag_list.remove(c)
        counts = {s: sum(d.sev == s for d in self.diags) for s in ICON}
        for d in self.diags:
            row = Gtk.ListBoxRow(css_classes=["diag-row", d.sev])
            row.diag = d
            box = Gtk.Box(spacing=10, margin_start=12, margin_end=8, margin_top=3, margin_bottom=3)
            box.append(Gtk.Image(icon_name=ICON[d.sev], css_classes=[f"diag-{d.sev}"]))
            box.append(Gtk.Label(label="%d:%d" % self._pos(text, d.start), css_classes=["mono", "dim-label", "caption"]))
            box.append(Gtk.Label(label=d.msg, xalign=0, hexpand=True, wrap=True))
            if d.fix:
                b = Gtk.Button(label="Fix", css_classes=["flat", "small-btn"])
                b.connect("clicked", lambda _b, dd=d: self._apply(dd))
                box.append(b)
            row.set_child(box)
            self.diag_list.append(row)
        self.revealer.set_reveal_child(bool(self.diags))
        parts = [f"{counts[s]} {s}{'s' if counts[s] != 1 else ''}" for s in ("error", "warning") if counts[s]]
        self.chip.set_text(" · ".join(parts) if parts else "No problems")
        for c in ("ok", "bad", "warn"):
            self.chip.remove_css_class(c)
        self.chip.add_css_class("bad" if counts["error"] else "warn" if counts["warning"] else "ok")
        self.fix_btn.set_sensitive(any(d.fix and d.sev != "info" for d in self.diags))

    # ---- interactions
    def _tooltip(self, view, x, y, _kb, tip):
        bx, by = view.window_to_buffer_coords(Gtk.TextWindowType.WIDGET, int(x), int(y))
        ok, it = view.get_iter_at_location(bx, by)
        if not ok:
            return False
        off = it.get_offset()
        hits = [d for d in self.diags if d.start <= off < max(d.end, d.start + 1)]
        if not hits:
            return False
        tip.set_text("\n".join(f"{d.sev}: {d.msg}" + ("   (Ctrl+. to fix)" if d.fix else "") for d in hits))
        return True

    def _jump(self, d):
        self.buf.place_cursor(self.buf.get_iter_at_offset(d.start))
        self.view.grab_focus()

    def _replace(self, text):
        self.buf.begin_user_action()
        self.buf.set_text(text)
        self.buf.end_user_action()

    def _apply(self, d):
        self._replace(cmbql.apply_fix(self._text(), d))

    def fix_all(self):
        new = cmbql.apply_all(self._text(), self.diags)
        if new != self._text():
            self._replace(new)

    def run(self):
        if self._timer:
            GLib.source_remove(self._timer)
            self._refresh()
        text = self._text()
        self.msg.remove_css_class("error")
        errs = [d for d in self.diags if d.sev == "error"]
        if errs:
            self.msg.add_css_class("error")
            line, col = self._pos(text, errs[0].start)
            self.msg.set_text(f"{len(errs)} error{'s' if len(errs) != 1 else ''} — not run. First: {line}:{col} {errs[0].msg}")
            self._jump(errs[0])
            return
        try:
            res = self.backend.execute(text)
        except (BackendError, NotImplementedError) as e:
            self.msg.add_css_class("error")
            self.msg.set_text(str(e))
            return
        self.grid.set_result(res)
        self.msg.set_text(f"{len(res.rows)} rows · {res.elapsed_ms:.1f} ms" + (f" · {res.message}" if res.message else ""))

    def explain(self):
        if self._timer:
            GLib.source_remove(self._timer)
            self._refresh()
        text = self._text()
        self.msg.remove_css_class("error")
        errs = [d for d in self.diags if d.sev == "error"]
        if errs:
            self.msg.add_css_class("error")
            line, col = self._pos(text, errs[0].start)
            self.msg.set_text(f"{len(errs)} error{'s' if len(errs) != 1 else ''} — cannot explain. First: {line}:{col} {errs[0].msg}")
            self._jump(errs[0])
            return
        try:
            res = self.backend.explain(text)
        except (BackendError, NotImplementedError) as e:
            self.msg.add_css_class("error")
            self.msg.set_text(str(e))
            return
        self.grid.set_result(res)
        self.msg.set_text(f"Plan: {len(res.rows)} steps · {res.elapsed_ms:.1f} ms" + (f" · {res.message}" if res.message else ""))
