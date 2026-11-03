from __future__ import annotations

import os
import sys
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gdk, Gio, Gtk

from .backend import MockBackend, NativeBackend
from .window import MainWindow

APP_ID = "io.github.gorovehq.ComboliteStudio"
VERSION = "0.1.0"


class ComboliteApp(Adw.Application):
    def __init__(self, backend):
        super().__init__(application_id=APP_ID, flags=Gio.ApplicationFlags.HANDLES_OPEN)
        self.backend = backend
        self.win = None

    def do_startup(self):
        Adw.Application.do_startup(self)
        css = Gtk.CssProvider()
        css.load_from_path(str(Path(__file__).with_name("style.css")))
        Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        Adw.StyleManager.get_default().set_color_scheme(Adw.ColorScheme.PREFER_DARK)
        for name, cb, accels in (("quit", lambda *_: self.quit(), ["<primary>q"]),
                                 ("about", self._about, []),
                                 ("toggle-theme", self._toggle_theme, ["<primary><shift>t"])):
            a = Gio.SimpleAction.new(name, None)
            a.connect("activate", cb)
            self.add_action(a)
            if accels:
                self.set_accels_for_action(f"app.{name}", accels)

    def _window(self):
        if self.win is None:
            self.win = MainWindow(self, self.backend)
        return self.win

    def do_activate(self):
        self._window().present()

    def do_open(self, files, n_files, hint):
        win = self._window()
        win.present()
        if files:
            win.open_database(files[0].get_path())

    def _toggle_theme(self, *_):
        sm = Adw.StyleManager.get_default()
        sm.set_color_scheme(Adw.ColorScheme.FORCE_LIGHT if sm.get_dark() else Adw.ColorScheme.FORCE_DARK)

    def _about(self, *_):
        kw = dict(application_name="Combolite Studio", application_icon="drive-harddisk-symbolic",
                  version=VERSION, developer_name="gorove", license_type=Gtk.License.MIT_X11,
                  website="https://github.com/gorovehq/db-combolite",
                  comments="Desktop client for the db-combolite embeddable multi-model database.")
        if hasattr(Adw, "AboutDialog"):
            Adw.AboutDialog(**kw).present(self.win)
        else:
            Adw.AboutWindow(transient_for=self.win, **kw).present()


def main(argv=None):
    argv = argv or sys.argv
    lib = os.environ.get("COMBOLITE_LIB")
    backend = None
    try:
        backend = NativeBackend(lib)
    except Exception:
        backend = MockBackend()
    return ComboliteApp(backend).run(argv)
