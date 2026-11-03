"""Small dialog helpers that work on libadwaita 1.4 (MessageDialog) and 1.5+ (AlertDialog)."""
from gi.repository import Adw, Gtk


def _dialog(parent, heading, body):
    if hasattr(Adw, "AlertDialog"):
        d = Adw.AlertDialog(heading=heading, body=body or "")
        return d, lambda: d.present(parent)
    d = Adw.MessageDialog(transient_for=parent, heading=heading, body=body or "")
    return d, d.present


def confirm(parent, heading, body, ok_label, on_ok, destructive=True):
    d, show = _dialog(parent, heading, body)
    d.add_response("cancel", "Cancel")
    d.add_response("ok", ok_label)
    d.set_response_appearance("ok", Adw.ResponseAppearance.DESTRUCTIVE if destructive else Adw.ResponseAppearance.SUGGESTED)
    d.set_default_response("cancel")
    d.set_close_response("cancel")
    d.connect("response", lambda _d, r: on_ok() if r == "ok" else None)
    show()


def form(parent, heading, fields, ok_label, on_ok, body=None):
    """fields: [(key, label, default)]. on_ok receives {key: text}."""
    d, show = _dialog(parent, heading, body)
    box = Gtk.ListBox(css_classes=["boxed-list"], selection_mode=Gtk.SelectionMode.NONE)
    rows = {}
    for key, label, default in fields:
        row = Adw.EntryRow(title=label)
        row.set_text("" if default is None else str(default))
        box.append(row)
        rows[key] = row
    d.set_extra_child(box)
    d.add_response("cancel", "Cancel")
    d.add_response("ok", ok_label)
    d.set_response_appearance("ok", Adw.ResponseAppearance.SUGGESTED)
    d.set_default_response("ok")
    d.set_close_response("cancel")
    d.connect("response", lambda _d, r: on_ok({k: w.get_text() for k, w in rows.items()}) if r == "ok" else None)
    show()
