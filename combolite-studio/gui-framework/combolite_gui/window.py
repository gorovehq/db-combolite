from __future__ import annotations

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk, Pango

from .backend import MODEL_INFO, Model
from .console import ConsoleView
from .views import (DocumentView, LinksView, OverviewView, TableView,
                    TimeSeriesView, VectorView, short)


class SidebarRow(Gtk.ListBoxRow):
    def __init__(self, section, kind, label, icon, store=None):
        super().__init__()
        self.section, self.kind, self.store, self.label = section, kind, store, label
        box = Gtk.Box(spacing=10, margin_top=2, margin_bottom=2)
        img = Gtk.Image(icon_name=icon)
        if store:
            img.add_css_class("model-icon")
            img.add_css_class(f"model-{store.model.value}")
        box.append(img)
        box.append(Gtk.Label(label=label, xalign=0, hexpand=True, ellipsize=Pango.EllipsizeMode.END))
        if store:
            box.append(Gtk.Label(label=short(store.count), css_classes=["mono", "dim-label", "count"]))
        self.set_child(box)


class MainWindow(Adw.ApplicationWindow):
    def __init__(self, app, backend):
        super().__init__(application=app, title="Combolite Studio", default_width=1280, default_height=820)
        self.set_size_request(360, 480)
        self.backend = backend
        self.current = None

        self.split = Adw.NavigationSplitView(min_sidebar_width=240, max_sidebar_width=320, sidebar_width_fraction=0.22)
        self.toasts = Adw.ToastOverlay(child=self.split)
        self.set_content(self.toasts)
        self.split.set_sidebar(Adw.NavigationPage(title="Combolite Studio", child=self._build_sidebar()))
        self.content_page = Adw.NavigationPage(title="Welcome", child=self._build_content())
        self.split.set_content(self.content_page)

        bp = Adw.Breakpoint.new(Adw.BreakpointCondition.parse("max-width: 720sp"))
        bp.add_setter(self.split, "collapsed", True)
        self.add_breakpoint(bp)

        self._install_actions(app)
        self._sync_state()

    # ---------- UI ----------
    def _build_sidebar(self):
        tv = Adw.ToolbarView()
        hb = Adw.HeaderBar()
        self.sidebar_title = Adw.WindowTitle(title="Combolite", subtitle="no database")
        hb.set_title_widget(self.sidebar_title)
        hb.pack_start(Gtk.Button(icon_name="document-open-symbolic", tooltip_text="Open database (Ctrl+O)", action_name="win.open"))
        menu = Gio.Menu()
        db = Gio.Menu()
        db.append("New Database…", "win.new")
        db.append("Open Database…", "win.open")
        db.append("Close Database", "win.close-db")
        menu.append_section(None, db)
        app_s = Gio.Menu()
        app_s.append("Toggle Dark Mode", "app.toggle-theme")
        app_s.append("About Combolite Studio", "app.about")
        menu.append_section(None, app_s)
        hb.pack_end(Gtk.MenuButton(icon_name="open-menu-symbolic", menu_model=menu, tooltip_text="Main menu"))
        tv.add_top_bar(hb)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.sidebar_search = Gtk.SearchEntry(placeholder_text="Filter stores  (Ctrl+K)", margin_start=10, margin_end=10, margin_bottom=6)
        self.sidebar_search.connect("search-changed", lambda *_: self.sidebar_list.invalidate_filter())
        box.append(self.sidebar_search)
        self.sidebar_list = Gtk.ListBox(css_classes=["navigation-sidebar"])
        self.sidebar_list.set_header_func(self._row_header)
        self.sidebar_list.set_filter_func(self._row_filter)
        self.sidebar_list.connect("row-selected", self._on_row)
        self.sidebar_list.set_placeholder(Gtk.Label(label="No database open", css_classes=["dim-label"], margin_top=24))
        box.append(Gtk.ScrolledWindow(vexpand=True, child=self.sidebar_list, hscrollbar_policy=Gtk.PolicyType.NEVER))
        tv.set_content(box)
        return tv

    def _build_content(self):
        tv = Adw.ToolbarView()
        hb = Adw.HeaderBar()
        self.content_title = Adw.WindowTitle(title="Combolite Studio")
        hb.set_title_widget(self.content_title)
        hb.pack_end(Gtk.Button(icon_name="view-refresh-symbolic", tooltip_text="Refresh (F5)", action_name="win.refresh"))
        tv.add_top_bar(hb)

        self.stack = Gtk.Stack(transition_type=Gtk.StackTransitionType.CROSSFADE, transition_duration=120)
        self.stack.add_named(self._build_welcome(), "welcome")
        self.overview = OverviewView(self._select_store)
        self.stack.add_named(self.overview, "overview")
        self.console = ConsoleView(self.toast)
        self.console.backend = self.backend
        self.stack.add_named(self.console, "console")
        self.views = {
            Model.TABLE: TableView(self.toast),
            Model.DOCUMENT: DocumentView(self.toast),
            Model.VECTOR: VectorView(self.toast),
            Model.LINK: LinksView(self.toast),
            Model.TIMESERIES: TimeSeriesView(self.toast),
        }
        for m, v in self.views.items():
            self.stack.add_named(v, m.value)
        tv.set_content(self.stack)
        tv.add_bottom_bar(self._build_statusbar())
        return tv

    def _build_welcome(self):
        page = Adw.StatusPage(icon_name="drive-harddisk-symbolic", title="Combolite Studio",
                              description="Tables, documents, full-text, vectors, links and time series — in one file.")
        btns = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, halign=Gtk.Align.CENTER)
        for label, action, cls in (("Open Database…", "win.open", ["suggested-action", "pill"]),
                                   ("New Database…", "win.new", ["pill"]),
                                   ("Explore Demo Data", "win.demo", ["pill", "flat"])):
            btns.append(Gtk.Button(label=label, action_name=action, css_classes=cls))
        page.set_child(btns)
        return page

    def _build_statusbar(self):
        self.status = Gtk.Box(spacing=10, css_classes=["statusbar"])
        self.st_dot = Gtk.Box(css_classes=["status-dot"], valign=Gtk.Align.CENTER)
        self.st_path = Gtk.Label(xalign=0, hexpand=True, ellipsize=Pango.EllipsizeMode.MIDDLE, css_classes=["mono"])
        self.st_meta = Gtk.Label(css_classes=["mono", "dim-label"])
        self.st_backend = Gtk.Label(label=self.backend.name.upper(), css_classes=["pill-badge"])
        for w in (self.st_dot, self.st_path, self.st_meta, self.st_backend):
            self.status.append(w)
        return self.status

    # ---------- sidebar ----------
    def _rebuild_sidebar(self):
        while (c := self.sidebar_list.get_first_child()):
            self.sidebar_list.remove(c)
        if not self.backend.is_open:
            return
        self.sidebar_list.append(SidebarRow("general", "overview", "Overview", "user-home-symbolic"))
        self.sidebar_list.append(SidebarRow("general", "console", "Query Console", "utilities-terminal-symbolic"))
        stores = self._call(self.backend.list_stores) or []
        for m in Model:
            for s in sorted((s for s in stores if s.model == m), key=lambda s: s.name):
                self.sidebar_list.append(SidebarRow(m.value, "store", s.name, MODEL_INFO[m][2], store=s))

    def _row_header(self, row, before):
        if row.section != "general" and (before is None or before.section != row.section):
            row.set_header(Gtk.Label(label=MODEL_INFO[Model(row.section)][0], xalign=0, css_classes=["sidebar-heading"]))
        else:
            row.set_header(None)

    def _row_filter(self, row):
        q = self.sidebar_search.get_text().strip().lower()
        return row.kind != "store" or not q or q in row.label.lower()

    def _on_row(self, _lb, row):
        if row is None:
            return
        self.current = row
        if row.kind == "overview":
            self.overview.load(self.backend)
            self._show("overview", "Overview", self.backend.path)
        elif row.kind == "console":
            self._show("console", "Query Console", "CMBQL")
            self.console.grab_focus()
        else:
            s = row.store
            self.views[s.model].load(self.backend, s)
            self._show(s.model.value, s.name, f"{MODEL_INFO[s.model][1]} · {s.count:,} entries")
        self.split.set_show_content(True)

    def _select_store(self, store):
        i = 0
        while (row := self.sidebar_list.get_row_at_index(i)):
            if row.store is store or (row.store and row.store.name == store.name and row.store.model == store.model):
                self.sidebar_list.select_row(row)
                return
            i += 1

    def _show(self, page, title, subtitle=""):
        self.stack.set_visible_child_name(page)
        self.content_title.set_title(title)
        self.content_title.set_subtitle(subtitle or "")
        self.content_page.set_title(title)

    # ---------- state ----------
    def toast(self, msg):
        self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(msg), timeout=4))

    def _call(self, fn, *args):
        try:
            return fn(*args)
        except Exception as e:  # surface engine errors instead of crashing the UI
            self.toast(str(e))
            return None

    def _sync_state(self):
        is_open = self.backend.is_open
        for name in ("close-db", "refresh", "run-query", "console"):
            self.lookup_action(name).set_enabled(is_open)
        self._rebuild_sidebar()
        self.status.set_visible(is_open)
        self.sidebar_title.set_subtitle((self.backend.path or "no database").rsplit("/", 1)[-1])
        if not is_open:
            self._show("welcome", "Combolite Studio")
            return
        info = self._call(self.backend.file_info) or {}
        self.st_path.set_text(self.backend.path)
        self.st_meta.set_text(" · ".join(str(info[k]) for k in ("Size", "Page size", "Format") if k in info))
        self.sidebar_list.select_row(self.sidebar_list.get_row_at_index(0))

    def open_database(self, path, create=False):
        if self.backend.is_open:
            self._call(self.backend.close)
        try:
            (self.backend.create if create else self.backend.open)(path)
        except Exception as e:
            self.backend.path = None
            self.toast(f"Could not open {path}: {e}")
        self._sync_state()

    # ---------- actions ----------
    def _install_actions(self, app):
        for name, cb, accels in (
            ("open", self._on_open, ["<primary>o"]),
            ("new", self._on_new, ["<primary>n"]),
            ("demo", lambda *_: self.open_database(":demo:/gutenfree.cmb"), []),
            ("close-db", self._on_close, ["<primary>w"]),
            ("refresh", self._on_refresh, ["F5", "<primary>r"]),
            ("run-query", lambda *_: self.console.run(), ["<primary>Return"]),
            ("console", lambda *_: self._select_kind("console"), ["<primary>e"]),
            ("focus-search", lambda *_: self.sidebar_search.grab_focus(), ["<primary>k"]),
        ):
            a = Gio.SimpleAction.new(name, None)
            a.connect("activate", cb)
            self.add_action(a)
            if accels:
                app.set_accels_for_action(f"win.{name}", accels)

    def _select_kind(self, kind):
        i = 0
        while (row := self.sidebar_list.get_row_at_index(i)):
            if row.kind == kind:
                self.sidebar_list.select_row(row)
                return
            i += 1

    def _filters(self):
        f = Gtk.FileFilter(name="db-combolite databases")
        for p in ("*.cdbarch", "*.cmb", "*.combolite", "*.db"):
            f.add_pattern(p)
        a = Gtk.FileFilter(name="All files")
        a.add_pattern("*")
        store = Gio.ListStore.new(Gtk.FileFilter)
        store.append(f)
        store.append(a)
        return store

    def _on_open(self, *_):
        dlg = Gtk.FileDialog(title="Open Database", filters=self._filters())
        dlg.open(self, None, lambda d, res: self._finish(d.open_finish, res, False))

    def _on_new(self, *_):
        dlg = Gtk.FileDialog(title="New Database", initial_name="untitled.cmb", filters=self._filters())
        dlg.save(self, None, lambda d, res: self._finish(d.save_finish, res, True))

    def _finish(self, finish, res, create):
        try:
            f = finish(res)
        except GLib.Error:
            return  # cancelled
        if f:
            self.open_database(f.get_path(), create)

    def _on_close(self, *_):
        self._call(self.backend.close)
        self._sync_state()

    def _on_refresh(self, *_):
        row = self.current
        if row is None:
            return
        if row.kind == "overview":
            self.overview.load(self.backend)
        elif row.kind == "store":
            self.views[row.store.model].reload()
        self.toast("Refreshed")
