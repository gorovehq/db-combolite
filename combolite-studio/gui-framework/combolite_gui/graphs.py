"""Interactive node-link canvas used by the vector-graph and link visualizers.

GraphCanvas: pan (drag background), zoom (scroll, around pointer), select (click),
drag nodes (optional, re-heats the force layout), hover labels, neighbour highlight.
"""
from __future__ import annotations

import heapq
import math
import random

import cairo
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk, Pango

try:
    import numpy as np
except ImportError:  # pure-python fallback below
    np = None

ACCENT = (0.24, 0.77, 0.79)


def hex_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))


MODEL_RGB = {k: hex_rgb(v) for k, v in {
    "table": "#7aa7f0", "document": "#d9a85a", "vector": "#b291ea", "link": "#6fc08a", "timeseries": "#e88aa3"}.items()}


# ---------------------------------------------------------------- layout helpers
def project_2d(vectors, size=320.0):
    """PCA to 2D, scaled so the widest axis spans ±size."""
    n = len(vectors)
    if n == 0:
        return []
    if n == 1:
        return [(0.0, 0.0)]
    pts = None
    if np is not None:
        try:
            X = np.asarray(vectors, dtype=float)
            if X.ndim != 2 or X.shape[0] == 0 or X.shape[1] == 0:
                return [(0.0, 0.0)] * n
            X = np.nan_to_num(X, nan=0.0, posinf=0.0, neginf=0.0)
            X = X - X.mean(axis=0)
            if np.all(X == 0):
                pts = [(math.cos(2 * math.pi * i / n) * size * 0.5,
                        math.sin(2 * math.pi * i / n) * size * 0.5) for i in range(n)]
            else:
                _, _, vt = np.linalg.svd(X, full_matrices=False)
                P = X @ vt[:2].T
                if P.shape[1] < 2:
                    P = np.column_stack([P, np.zeros(n)])
                pts = [tuple(p) for p in P.tolist()]
        except Exception:
            pts = None
    if pts is None:
        d = len(vectors[0]) if len(vectors) > 0 else 0
        if d == 0:
            return [(0.0, 0.0)] * n
        mean = [sum(v[k] for v in vectors) / n for k in range(d)]
        X = [[v[k] - mean[k] for k in range(d)] for v in vectors]
        comps = []
        for c in range(2):
            r = random.Random(c)
            w = [r.random() - 0.5 for _ in range(d)]
            for _ in range(30):
                proj = [sum(x[k] * w[k] for k in range(d)) for x in X]
                w = [sum(proj[i] * X[i][k] for i in range(n)) for k in range(d)]
                for p in comps:
                    dot = sum(w[k] * p[k] for k in range(d))
                    w = [w[k] - dot * p[k] for k in range(d)]
                norm = math.sqrt(sum(x * x for x in w)) or 1
                w = [x / norm for x in w]
            comps.append(w)
        pts = [(sum(x[k] * comps[0][k] for k in range(d)), sum(x[k] * comps[1][k] for k in range(d))) for x in X]
    m = max(max(abs(x), abs(y)) for x, y in pts) or 1
    return [(x / m * size, y / m * size) for x, y in pts]


# heat ramp built from the app's own palette: weak (blue) -> teal -> green -> amber -> strong (pink)
HEAT_STOPS = [(0.0, MODEL_RGB["table"]), (0.3, ACCENT), (0.55, MODEL_RGB["link"]), (0.78, MODEL_RGB["document"]), (1.0, MODEL_RGB["timeseries"])]


def heat_rgb(t):
    t = max(0.0, min(1.0, t))
    for (t0, c0), (t1, c1) in zip(HEAT_STOPS, HEAT_STOPS[1:]):
        if t <= t1:
            u = (t - t0) / ((t1 - t0) or 1)
            return tuple(a + (b - a) * u for a, b in zip(c0, c1))
    return HEAT_STOPS[-1][1]


def propagate(adj_e, strengths, src, max_hops=6):
    """Strongest-path search from src (cost = -log strength, so strength multiplies along a path).
    -> {node: (cost, hops, parent, edge)} for everything within max_hops."""
    best = {src: (0.0, 0, None, None)}
    heap = [(0.0, 0, src)]
    while heap:
        cost, hops, u = heapq.heappop(heap)
        if cost > best[u][0] + 1e-12 or hops >= max_hops:
            continue
        for v, e in adj_e[u]:
            c = cost - math.log(max(strengths[e], 1e-3))
            if v not in best or c < best[v][0] - 1e-12:
                best[v] = (c, hops + 1, u, e)
                heapq.heappush(heap, (c, hops + 1, v))
    return best


def similarity_depths(vectors, ref=None):
    """Cosine similarity to vectors[ref] (or to the centroid), min-max scaled to 0..1 (1 = most similar)."""
    n = len(vectors)
    if n == 0:
        return []
    d = len(vectors[0]) if len(vectors) > 0 else 0
    if d == 0:
        return [0.5] * n
    r = vectors[ref] if ref is not None else [sum(v[k] for v in vectors) / n for k in range(d)]
    rn = math.sqrt(sum(x * x for x in r)) or 1
    sims = []
    for v in vectors:
        vn = math.sqrt(sum(x * x for x in v)) or 1
        dot = sum(a * b for a, b in zip(v, r))
        sim = dot / (vn * rn)
        if math.isnan(sim):
            sim = 0.0
        sims.append(sim)
    lo, hi = min(sims), max(sims)
    span = (hi - lo) or 1
    return [(s - lo) / span for s in sims]


class ForceLayout:
    """Damped spring physics: edges are rubber bands (shorter rest length for heavier weights),
    nodes repel, and the integrator is deliberately under-damped so released nodes jiggle.
    With home= positions the layout starts at rest and anchors pull nodes back after a drag."""
    REP, KS = 0.35, 0.2

    def __init__(self, n, edges, k=34.0, seed=7, home=None, anchor=0.0, weights=None):
        r = random.Random(seed)
        self.n, self.edges, self.k, self.anchor = n, edges, k, anchor
        self.pinned, self.awake = set(), False
        self.settle = 0 if home else 140
        if home:
            pos = [[float(x), float(y)] for x, y in home]
            rest = [math.hypot(pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]) or 1.0 for a, b in edges]
        else:
            pos = [[r.uniform(-250, 250), r.uniform(-250, 250)] for _ in range(n)]
            rest = [k * (1.5 - 0.8 * (weights[e] if weights else 0.5)) for e in range(len(edges))]
        self.home = [list(p) for p in pos] if home else None
        if np is not None:
            self.P, self.V = np.array(pos, dtype=float), np.zeros((n, 2))
            self.H = np.array(self.home, dtype=float) if home else None
            self.ea = np.array([e[0] for e in edges], dtype=int)
            self.eb = np.array([e[1] for e in edges], dtype=int)
            self.rest = np.array(rest, dtype=float)
        else:
            self.P, self.V, self.H, self.rest = pos, [[0.0, 0.0] for _ in range(n)], self.home, rest

    @property
    def pos(self):
        return self.P

    def wake(self):
        self.awake = True

    def step(self):
        if self.n == 0:
            return False
        damp = 0.78 if self.settle > 0 else 0.9
        rep = 0.0 if self.home else self.REP
        if np is not None:
            P, V = self.P, self.V
            D = P[:, None, :] - P[None, :, :]
            d2 = (D * D).sum(2) + 0.01
            w = np.where(d2 < 90000, rep * self.k * self.k / d2, 0.0)
            np.fill_diagonal(w, 0.0)
            F = (D * w[:, :, None]).sum(1)
            if len(self.ea):
                dl = P[self.ea] - P[self.eb]
                dist = np.sqrt((dl * dl).sum(1)) + 1e-6
                fs = (self.KS * (dist - self.rest) / dist)[:, None] * dl
                np.add.at(F, self.ea, -fs)
                np.add.at(F, self.eb, fs)
            F += (self.H - P) * self.anchor if self.H is not None else -P * 0.004
            V = (V + F * 0.35) * damp
            sp = np.sqrt((V * V).sum(1))
            big = sp > 40
            V[big] *= (40 / sp[big])[:, None]
            for i in self.pinned:
                V[i] = 0
            self.P, self.V = P + V, V
            energy = float(sp.sum())
        else:
            P, V, n = self.P, self.V, self.n
            F = [[0.0, 0.0] for _ in range(n)]
            if rep:
                k2 = self.k * self.k
                for i in range(n):
                    for j in range(i + 1, n):
                        dx, dy = P[i][0] - P[j][0], P[i][1] - P[j][1]
                        d2 = dx * dx + dy * dy + 0.01
                        if d2 > 90000:
                            continue
                        f = rep * k2 / d2
                        F[i][0] += dx * f
                        F[i][1] += dy * f
                        F[j][0] -= dx * f
                        F[j][1] -= dy * f
            for e, (a, c) in enumerate(self.edges):
                dx, dy = P[a][0] - P[c][0], P[a][1] - P[c][1]
                d = math.hypot(dx, dy) + 1e-6
                f = self.KS * (d - self.rest[e]) / d
                F[a][0] -= dx * f
                F[a][1] -= dy * f
                F[c][0] += dx * f
                F[c][1] += dy * f
            energy = 0.0
            for i in range(n):
                if self.H is not None:
                    F[i][0] += (self.H[i][0] - P[i][0]) * self.anchor
                    F[i][1] += (self.H[i][1] - P[i][1]) * self.anchor
                else:
                    F[i][0] -= P[i][0] * 0.004
                    F[i][1] -= P[i][1] * 0.004
                vx, vy = (V[i][0] + F[i][0] * 0.35) * damp, (V[i][1] + F[i][1] * 0.35) * damp
                s = math.hypot(vx, vy)
                if s > 40:
                    vx, vy = vx * 40 / s, vy * 40 / s
                if i in self.pinned:
                    vx = vy = 0.0
                V[i][0], V[i][1] = vx, vy
                P[i][0] += vx
                P[i][1] += vy
                energy += s
        if self.settle:
            self.settle -= 1
        moving = energy > 0.03 * self.n or bool(self.pinned) or self.awake or self.settle > 0
        self.awake = False
        return moving


# ---------------------------------------------------------------- canvas
class GraphCanvas(Gtk.DrawingArea):
    DEPTH = 420.0     # world units between the farthest and nearest node at full 3D
    FOCAL = 1100.0

    def __init__(self, on_select=None, draggable_nodes=False):
        super().__init__(hexpand=True, vexpand=True, focusable=True)
        self.nodes = []        # dicts: id, label, x, y, r, color
        self.edges = []        # (i, j)
        self.adj = []
        self.directed = False
        self.sel = None
        self.hover = None
        self.scale, self.cx, self.cy = 1.0, 0.0, 0.0
        self.on_select = on_select
        self.draggable = draggable_nodes
        self.layout = None
        self._timer = None
        self._need_fit = True
        self._user_view = False
        self._drag = None
        self._ptr = (0.0, 0.0)
        self.show_labels = False
        self.heat, self.heat_src, self.heat_edges = None, None, set()
        self.edge_s = None      # per-edge strength 0..1 for path-strength heat
        self.adj_e = []
        self.on_heat = None
        self._drag_src = None
        self._ctr = None        # node index the view is gliding to
        self._pinch0 = 1.0
        self.hover_edge = None
        self.on_hover = None    # (kind "node"|"edge"|None, index, x, y)
        self.weights = None     # per-edge strength 0..1 (drives width / depth)
        self.blend = self.blend_to = 0.0   # 0 = flat 2D, 1 = full 3D
        self.yaw, self.pitch = -0.6, 0.32
        self._anim = None
        self.set_draw_func(self._draw)

        drag = Gtk.GestureDrag()
        drag.connect("drag-begin", self._drag_begin)
        drag.connect("drag-update", self._drag_update)
        drag.connect("drag-end", self._drag_end)
        self.add_controller(drag)
        scroll = Gtk.EventControllerScroll(flags=Gtk.EventControllerScrollFlags.VERTICAL)
        scroll.connect("scroll", self._scroll)
        self.add_controller(scroll)
        pinch = Gtk.GestureZoom()
        pinch.connect("begin", lambda *_: setattr(self, "_pinch0", self.scale))
        pinch.connect("scale-changed", lambda _g, s: self._set_scale(self._pinch0 * s))
        self.add_controller(pinch)
        dbl = Gtk.GestureClick()
        dbl.connect("pressed", lambda _g, n, x, y: self.reset_view() if (n == 2 and self.hit(x, y) is None) else None)
        self.add_controller(dbl)
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._motion)
        motion.connect("leave", lambda *_: self._set_hover(None))
        self.add_controller(motion)

    # data
    def set_graph(self, nodes, edges, layout=None, directed=False, weights=None, edge_strengths=None):
        if self._timer:
            GLib.source_remove(self._timer)
            self._timer = None
        self.nodes, self.edges, self.directed, self.weights = nodes, edges, directed, weights
        self.edge_s = edge_strengths or weights
        self.adj_e = [[] for _ in nodes]
        for e, (a, b) in enumerate(edges):
            self.adj_e[a].append((b, e))
            self.adj_e[b].append((a, e))
        for n in nodes:
            n.setdefault("z", 0.5)
            n.setdefault("z_to", n["z"])
        self.adj = [[] for _ in nodes]
        for a, b in edges:
            self.adj[a].append(b)
            self.adj[b].append(a)
        self.sel = self.hover = None
        self._drag_src = None
        self._need_fit, self._user_view = True, False
        self.layout = layout
        self._update_heat()
        if layout:
            self._sync_layout()
            self._timer = GLib.timeout_add(16, self._tick)
        self.queue_draw()

    def _sync_layout(self):
        for n, (x, y) in zip(self.nodes, self.layout.pos):
            n["x"], n["y"] = x, y

    def _tick(self):
        moving = self.layout.step()
        self._sync_layout()
        if not self._user_view:
            self._need_fit = True
        self.queue_draw()
        if not moving:
            self._timer = None
        return moving

    def _reheat(self):
        if self.layout:
            self.layout.wake()
            if not self._timer:
                self._timer = GLib.timeout_add(16, self._tick)

    # ---- 3D mode / depth animation
    def set_mode(self, mode):
        self.blend_to = 1.0 if mode == "3d" else 0.0
        self._start_anim()

    def set_depths(self, zs, animate=True):
        """zs[i] in 0..1, 1 = nearest to the viewer."""
        for n, z in zip(self.nodes, zs):
            n["z_to"] = z
            if not animate:
                n["z"] = z
        if animate:
            self._start_anim()
        else:
            self.queue_draw()

    def _set_scale(self, s):
        self.scale = max(0.05, min(40.0, s))
        self._user_view = True
        self.queue_draw()

    def zoom_by(self, f):
        self._set_scale(self.scale * f)

    def _start_anim(self):
        if not self._anim:
            self._anim = GLib.timeout_add(16, self._anim_tick)

    def _anim_tick(self):
        busy = False
        if self._ctr is not None and self._ctr < len(self.nodes):
            n = self.nodes[self._ctr]
            dx, dy = n["x"] - self.cx, n["y"] - self.cy
            if abs(dx) + abs(dy) > 0.5 / max(self.scale, 0.01):
                self.cx += dx * 0.18
                self.cy += dy * 0.18
                busy = True
            else:
                self._ctr = None
        d = self.blend_to - self.blend
        if abs(d) > 0.004:
            self.blend += d * 0.16
            busy = True
        else:
            self.blend = self.blend_to
        for n in self.nodes:
            dz = n.get("z_to", 0.5) - n.get("z", 0.5)
            if abs(dz) > 0.002:
                n["z"] += dz * 0.2
                busy = True
            else:
                n["z"] = n.get("z_to", 0.5)
        self.queue_draw()
        if not busy:
            self._anim = None
        return busy

    # ---- view transform
    def to_screen(self, x, y):
        return ((x - self.cx) * self.scale + self.get_width() / 2, (y - self.cy) * self.scale + self.get_height() / 2)

    def to_world(self, sx, sy):
        return ((sx - self.get_width() / 2) / self.scale + self.cx, (sy - self.get_height() / 2) / self.scale + self.cy)

    def _proj(self, n):
        """-> (screen_x, screen_y, perspective_scale, camera_z). Identical to to_screen when blend == 0."""
        w, h, b = self.get_width(), self.get_height(), self.blend
        X, Y = n["x"] - self.cx, n["y"] - self.cy
        if b < 0.001:
            return (w / 2 + X * self.scale, h / 2 + Y * self.scale, 1.0, 0.0)
        Z = (n.get("z", 0.5) - 0.5) * self.DEPTH * b
        ya, pa = self.yaw * b, self.pitch * b
        x1, z1 = X * math.cos(ya) + Z * math.sin(ya), -X * math.sin(ya) + Z * math.cos(ya)
        y2, z2 = Y * math.cos(pa) - z1 * math.sin(pa), Y * math.sin(pa) + z1 * math.cos(pa)
        k = self.FOCAL / max(self.FOCAL - z2, 120.0)
        return (w / 2 + x1 * self.scale * k, h / 2 + y2 * self.scale * k, k, z2)

    def fit_view(self, w=None, h=None):
        w, h = w or self.get_width(), h or self.get_height()
        if not self.nodes or w < 10:
            return
        xs, ys = [n["x"] for n in self.nodes], [n["y"] for n in self.nodes]
        self.cx, self.cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
        self.scale = min((w - 100) / ((max(xs) - min(xs)) or 1), (h - 100) / ((max(ys) - min(ys)) or 1), 4)
        if self.blend_to > 0.5:
            self.scale *= 0.8
        self._need_fit = False
        self.queue_draw()

    def reset_view(self):
        self._user_view = False
        self.yaw, self.pitch = -0.6, 0.32
        self.fit_view()

    def hit(self, sx, sy):
        best, key = None, None
        for i, n in enumerate(self.nodes):
            px, py, k, z = self._proj(n)
            rr = max(n["r"] * k, 5) + 4
            d2 = (px - sx) ** 2 + (py - sy) ** 2
            if d2 <= rr * rr and (key is None or (-z, d2) < key):
                best, key = i, (-z, d2)
        return best

    def _update_heat(self):
        src = self.sel if self.sel is not None else self._drag_src
        if src is None or src >= len(self.nodes):
            self.heat, self.heat_src, self.heat_edges = None, None, set()
        else:
            es = self.edge_s or [0.6] * len(self.edges)
            res = propagate(self.adj_e, es, src)
            self.heat = {i: (max(0.0, min(1.0, 1 + math.log10(max(math.exp(-c), 1e-6)) / 2)), h, math.exp(-c))
                         for i, (c, h, _p, _e) in res.items()}
            self.heat_edges = {e for (_c, _h, _p, e) in res.values() if e is not None}
            self.heat_src = src
        self.queue_draw()
        if self.on_heat:
            self.on_heat(self.heat_src, len(self.heat) - 1 if self.heat else 0)

    def heat_info(self, i):
        """-> (path strength, hops) from the current heat source, or None."""
        h = self.heat.get(i) if self.heat else None
        return (h[2], h[1]) if h and i != self.heat_src else None

    def _node_rgb(self, i, base):
        if self.heat is None:
            return base, 1.0
        h = self.heat.get(i)
        return (base, 0.14) if h is None else (heat_rgb(h[0]), 1.0)

    def select(self, i):
        self.sel = i
        self._update_heat()
        if i is not None:
            self._ctr, self._user_view = i, True
            self._start_anim()
        self.queue_draw()
        if self.on_select:
            self.on_select(i)

    def select_id(self, node_id):
        for i, n in enumerate(self.nodes):
            if n["id"] == node_id:
                self.select(i)
                return True
        return False

    # ---- input
    def _drag_begin(self, g, x, y):
        self._moved = False
        self._set_hover(None)
        i = self.hit(x, y)
        shift = bool(g.get_current_event_state() & Gdk.ModifierType.SHIFT_MASK)
        self._ctr = None
        if i is not None and self.draggable and not shift:
            n = self.nodes[i]
            self._drag_src = i
            self._update_heat()
            self._drag = ("node", i, n["x"], n["y"], x, y, self._proj(n)[2])
        elif self.blend_to > 0.5 and not shift:
            self._drag = ("rotate", self.yaw, self.pitch, x, y, i)
        else:
            self._drag = ("pan", self.cx, self.cy, x, y, i)

    def _drag_update(self, _g, dx, dy):
        if abs(dx) + abs(dy) > 3:
            self._moved = True
        if not self._moved or not self._drag:
            return
        kind = self._drag[0]
        if kind == "node":
            _, i, wx0, wy0, _sx, _sy, k0 = self._drag
            f = 1.0 / (self.scale * k0)
            ddx, ddy = dx * f, dy * f
            ya, pa = self.yaw * self.blend, self.pitch * self.blend
            z1 = -ddy * math.sin(pa)
            wx = wx0 + ddx * math.cos(ya) - z1 * math.sin(ya)
            wy = wy0 + ddy * math.cos(pa)
            self.nodes[i]["x"], self.nodes[i]["y"] = wx, wy
            if self.layout:
                self.layout.pos[i] = [wx, wy]
                self.layout.pinned = {i}
                self._reheat()
        elif kind == "rotate":
            self.yaw = self._drag[1] + dx * 0.01
            self.pitch = max(-1.4, min(1.4, self._drag[2] + dy * 0.01))
        else:
            self.cx = self._drag[1] - dx / self.scale
            self.cy = self._drag[2] - dy / self.scale
        self._user_view = True
        self.queue_draw()

    def _drag_end(self, _g, dx, dy):
        if self._drag and not self._moved:
            self.select(self._drag[1] if self._drag[0] == "node" else self._drag[5])
        if self.layout:
            self.layout.pinned = set()
        self._drag = None
        if self._drag_src is not None:
            self._drag_src = None
            self._update_heat()

    def _scroll(self, _c, _dx, dy):
        f = 0.88 if dy > 0 else 1.14
        if self.blend_to > 0.5:
            self.scale = max(0.05, min(40.0, self.scale * f))
        else:
            px, py = self._ptr
            wx, wy = self.to_world(px, py)
            self.scale = max(0.05, min(40.0, self.scale * f))
            self.cx = wx - (px - self.get_width() / 2) / self.scale
            self.cy = wy - (py - self.get_height() / 2) / self.scale
        self._user_view = True
        self.queue_draw()
        return True

    def _motion(self, _c, x, y):
        self._ptr = (x, y)
        if self._drag:
            return
        i = self.hit(x, y)
        self._set_hover(i, None if i is not None else self.hit_edge(x, y))

    def _set_hover(self, i, e=None):
        if (i, e) == (self.hover, self.hover_edge):
            return
        self.hover, self.hover_edge = i, e
        self.set_cursor_from_name("pointer" if (i is not None or e is not None) else None)
        self.queue_draw()
        if self.on_hover:
            x, y = self._ptr
            self.on_hover("node" if i is not None else "edge" if e is not None else None, i if i is not None else e, x, y)

    def _stretch(self, e):
        rest = getattr(self.layout, "rest", None) if self.layout else None
        if rest is None:
            return 0.0
        a, c = self.edges[e]
        na, nc = self.nodes[a], self.nodes[c]
        return math.hypot(na["x"] - nc["x"], na["y"] - nc["y"]) / (float(rest[e]) or 1.0) - 1.0

    def hit_edge(self, sx, sy):
        S = [self._proj(n) for n in self.nodes]
        best, bd = None, 6.0
        for e, (a, c) in enumerate(self.edges):
            ax, ay, bx, by = S[a][0], S[a][1], S[c][0], S[c][1]
            dx, dy = bx - ax, by - ay
            L = dx * dx + dy * dy
            t = 0.0 if L == 0 else max(0.0, min(1.0, ((sx - ax) * dx + (sy - ay) * dy) / L))
            d = math.hypot(sx - (ax + t * dx), sy - (ay + t * dy))
            if d < bd:
                best, bd = e, d
        return best

    # ---- drawing
    def _draw(self, _area, cr, w, h):
        if self._need_fit and self.nodes:
            self.fit_view(w, h)
        fg = self.get_color()
        dark = (fg.red + fg.green + fg.blue) / 3 > 0.5
        b = self.blend
        cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.06 * (1 - 0.7 * b))
        for gx in range(14, w, 28):
            for gy in range(14, h, 28):
                cr.rectangle(gx, gy, 1.3, 1.3)
        cr.fill()
        if not self.nodes:
            return

        focus = self.sel if self.sel is not None else self.hover
        near = set(self.adj[focus]) | {focus} if focus is not None else None
        P = [self._proj(n) for n in self.nodes]
        zs = [p[3] for p in P]
        zmin, span = min(zs), (max(zs) - min(zs)) or 1.0
        depth = (lambda i: (P[i][3] - zmin) / span) if b > 0.02 else (lambda i: 1.0)
        fade = lambda d: 1 - b * (1 - d) * 0.65
        S = [(p[0], p[1]) for p in P]

        # edges, back to front
        per_edge = b > 0.02 or self.weights is not None or self.hover_edge is not None or self.layout is not None or self.heat is not None
        if not per_edge:
            cr.set_line_width(1)
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.05 if near else 0.13)
            hot = []
            for a, c in self.edges:
                if near and focus in (a, c):
                    hot.append((a, c))
                    continue
                cr.move_to(*S[a])
                cr.line_to(*S[c])
            cr.stroke()
            cr.set_source_rgba(*ACCENT, 0.9)
            cr.set_line_width(1.8)
            for a, c in hot:
                cr.move_to(*S[a])
                cr.line_to(*S[c])
                if self.directed:
                    self._arrow(cr, S[a], S[c], self.nodes[c]["r"])
            cr.stroke()
        else:
            order = sorted(range(len(self.edges)), key=lambda e: P[self.edges[e][0]][3] + P[self.edges[e][1]][3])
            cr.set_line_cap(1)
            for e in order:
                a, c = self.edges[e]
                wt = self.weights[e] if self.weights else 0.5
                kk = (P[a][2] + P[c][2]) / 2
                f = fade((depth(a) + depth(c)) / 2)
                st = self._stretch(e)
                if e == self.hover_edge:
                    cr.set_source_rgba(*ACCENT, 1.0)
                    cr.set_line_width((2.0 + 2.6 * wt) * kk)
                elif self.heat is not None:
                    ha, hc = self.heat.get(a), self.heat.get(c)
                    se = self.edge_s[e] if self.edge_s else 0.5
                    if ha and hc and (a == self.heat_src or c == self.heat_src):
                        other = c if a == self.heat_src else a
                        cr.set_source_rgba(*heat_rgb(self.heat[other][0]), 0.95 * f)
                        cr.set_line_width((1.4 + 3.6 * se) * kk)      # immediate relations: thickness = strength
                    elif ha and hc and e in self.heat_edges:
                        child = a if ha[1] > hc[1] else c
                        cr.set_source_rgba(*heat_rgb(self.heat[child][0]), 0.75 * f)
                        cr.set_line_width((0.8 + 2.0 * se) * kk)
                    else:
                        cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.04 * f)
                        cr.set_line_width(0.8 * kk)
                elif near and focus in (a, c):
                    cr.set_source_rgba(*ACCENT, 0.9 * f)
                    cr.set_line_width((1.4 + 2.4 * wt) * kk)
                elif st > 0.2:   # stretched band: thins out and warms up
                    cr.set_source_rgba(1.0, 0.66, 0.3, min(0.85, 0.2 + st * 0.5) * f)
                    cr.set_line_width(max(0.5, (0.6 + 2.0 * wt) / (1 + st * 0.8)) * kk)
                else:
                    cr.set_source_rgba(fg.red, fg.green, fg.blue, (0.05 if near else 0.08 + 0.3 * wt) * f)
                    cr.set_line_width((0.6 + 2.0 * wt) * kk)
                cr.move_to(*S[a])
                if st < -0.2:    # slack band: sags sideways
                    mx_, my_ = (S[a][0] + S[c][0]) / 2, (S[a][1] + S[c][1]) / 2
                    nx_, ny_ = -(S[c][1] - S[a][1]), S[c][0] - S[a][0]
                    ln = math.hypot(nx_, ny_) or 1.0
                    sag = min(-st, 0.8) * 0.35 * ln * (1 if e % 2 else -1)
                    cx_, cy_ = mx_ + nx_ / ln * sag, my_ + ny_ / ln * sag
                    cr.curve_to(cx_, cy_, cx_, cy_, *S[c])
                else:
                    cr.line_to(*S[c])
                if self.directed and near and focus in (a, c):
                    self._arrow(cr, S[a], S[c], self.nodes[c]["r"] * P[c][2])
                cr.stroke()

        # nodes, back to front
        for i in sorted(range(len(self.nodes)), key=lambda i: P[i][3]):
            n = self.nodes[i]
            sx, sy, k, _z = P[i]
            if sx < -30 or sy < -30 or sx > w + 30 or sy > h + 30:
                continue
            d = depth(i)
            a = (1.0 if (near is None or i in near or self.heat is not None) else 0.22) * fade(d)
            r = n["r"] * k
            if b > 0.02 and d > 0.7:
                cr.set_source_rgba(*n["color"], 0.16 * b * (d - 0.7) / 0.3)
                cr.arc(sx, sy, r * 2.2, 0, 2 * math.pi)
                cr.fill()
            col, hm = self._node_rgb(i, n["color"])
            cr.set_source_rgba(*col, a * hm)
            cr.arc(sx, sy, r, 0, 2 * math.pi)
            cr.fill()
            if i == self.sel:
                cr.set_source_rgba(*ACCENT, 1)
                cr.set_line_width(2)
                cr.arc(sx, sy, r + 4, 0, 2 * math.pi)
                cr.stroke()

        cr.select_font_face("sans-serif")
        cr.set_font_size(11)
        labels = list(range(len(self.nodes))) if self.show_labels else []
        if self.sel is not None:
            labels += self.adj[self.sel][:12]
        if self.hover is not None and self.hover not in labels:
            labels.append(self.hover)
        labels = list(dict.fromkeys(labels))
        if focus is not None:
            if focus in labels:
                labels.remove(focus)
            labels.append(focus)
        for i in labels:
            self._label(cr, self.nodes[i]["label"], *S[i], fg, dark, i == focus)

    @staticmethod
    def _arrow(cr, a, b, r):
        ang = math.atan2(b[1] - a[1], b[0] - a[0])
        tx, ty = b[0] - math.cos(ang) * (r + 3), b[1] - math.sin(ang) * (r + 3)
        cr.move_to(tx, ty)
        cr.line_to(tx - 8 * math.cos(ang - 0.4), ty - 8 * math.sin(ang - 0.4))
        cr.move_to(tx, ty)
        cr.line_to(tx - 8 * math.cos(ang + 0.4), ty - 8 * math.sin(ang + 0.4))

    @staticmethod
    def _label(cr, text, x, y, fg, dark, strong):
        ext = cr.text_extents(text)
        bx, by, pad = x + 10, y - 8, 4
        cr.set_source_rgba(*((0.09, 0.09, 0.1) if dark else (1, 1, 1)), 0.9)
        cr.rectangle(bx - pad, by - ext.height - pad, ext.x_advance + 2 * pad, ext.height + 2 * pad + 1)
        cr.fill()
        cr.set_source_rgba(fg.red, fg.green, fg.blue, 1 if strong else 0.75)
        cr.move_to(bx, by)
        cr.show_text(text)


class GraphPanel(Gtk.Overlay):
    """GraphCanvas + floating HUD (selection details/actions) + info chip + fit button."""

    def __init__(self, on_select=None, draggable=False):
        super().__init__()
        self.canvas = GraphCanvas(on_select=self._selected, draggable_nodes=draggable)
        self._on_select = on_select
        self.set_child(self.canvas)

        self.hud = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, css_classes=["graph-hud"],
                           halign=Gtk.Align.START, valign=Gtk.Align.END, margin_start=16, margin_bottom=64, visible=False)
        self.hud_title = Gtk.Label(xalign=0, css_classes=["heading", "mono"], selectable=True)
        self.hud_sub = Gtk.Label(xalign=0, css_classes=["dim-label", "caption"], wrap=True, max_width_chars=44)
        self.hud_actions = Gtk.Box(spacing=6, margin_top=4)
        for wdg in (self.hud_title, self.hud_sub, self.hud_actions):
            self.hud.append(wdg)
        self.add_overlay(self.hud)

        self.info = Gtk.Label(css_classes=["graph-info", "mono", "caption"], halign=Gtk.Align.END,
                              valign=Gtk.Align.START, margin_top=12, margin_end=12)
        self.add_overlay(self.info)

        zoom = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, halign=Gtk.Align.END, valign=Gtk.Align.END,
                       margin_end=16, margin_bottom=64)
        for icon, tip, cb in (("zoom-in-symbolic", "Zoom in", lambda: self.canvas.zoom_by(1.3)),
                              ("zoom-out-symbolic", "Zoom out", lambda: self.canvas.zoom_by(1 / 1.3)),
                              ("zoom-fit-best-symbolic", "Fit to view (double-click background)", lambda: self.canvas.reset_view())):
            b = Gtk.Button(icon_name=icon, tooltip_text=tip, css_classes=["osd", "circular"])
            b.connect("clicked", lambda _b, f=cb: f())
            zoom.append(b)
        self.add_overlay(zoom)

        self.btn2 = Gtk.ToggleButton(label="2D", active=True)
        self.btn3 = Gtk.ToggleButton(label="3D", group=self.btn2, tooltip_text="Depth view: drag to orbit, Shift+drag to pan")
        self.btn3.connect("toggled", lambda b: self._mode(b.get_active()))
        seg = Gtk.Box(css_classes=["linked", "mode-toggle"])
        seg.append(self.btn2)
        seg.append(self.btn3)
        self.lbl_btn = Gtk.ToggleButton(label="Aa", tooltip_text="Show all labels", css_classes=["mono"])
        self.lbl_btn.connect("toggled", lambda b: self._labels(b.get_active()))
        modes = Gtk.Box(spacing=8, halign=Gtk.Align.START, valign=Gtk.Align.START, margin_start=12, margin_top=12)
        modes.append(seg)
        modes.append(self.lbl_btn)
        self.add_overlay(modes)

        # hover context box; owners set panel.describe(kind, index) -> (title, [(key, value), ...])
        self.describe = None
        self.canvas.on_hover = self._hover
        self.tip = Gtk.Popover(autohide=False, has_arrow=False, can_target=False, focusable=False,
                               position=Gtk.PositionType.RIGHT, css_classes=["graph-tip"])
        self.tip.set_parent(self.canvas)
        self.canvas.connect("destroy", lambda *_: self.tip.unparent())

        self.caption = Gtk.Label(css_classes=["graph-info", "caption"], halign=Gtk.Align.CENTER, valign=Gtk.Align.START,
                                 margin_top=12, visible=False)
        self.add_overlay(self.caption)

        # colour scale strip along the bottom
        self.legend = Gtk.Box(spacing=20, css_classes=["heat-strip"], halign=Gtk.Align.FILL, valign=Gtk.Align.END)
        self.legend.append(Gtk.Label(label="PATH STRENGTH", css_classes=["stat-label"],
                                     tooltip_text="Product of link strengths along the strongest path (log scale)"))
        bar = Gtk.DrawingArea(content_width=300, content_height=30, valign=Gtk.Align.CENTER)
        bar.set_draw_func(self._draw_legend)
        self.legend.append(bar)
        none = Gtk.Box(spacing=6, valign=Gtk.Align.CENTER)
        none.append(Gtk.Box(css_classes=["heat-none"], valign=Gtk.Align.CENTER))
        none.append(Gtk.Label(label="no path in 6 hops", css_classes=["caption", "dim-label"]))
        self.legend.append(none)
        self.legend.append(Gtk.Label(label="thicker line = stronger direct link", css_classes=["caption", "dim-label"]))
        self.legend_lbl = Gtk.Label(label="select or drag a node", css_classes=["caption", "mono"], hexpand=True, xalign=1,
                                    ellipsize=Pango.EllipsizeMode.END)
        self.legend.append(self.legend_lbl)
        self.add_overlay(self.legend)
        self.canvas.on_heat = self._heat_changed

    def _labels(self, on):
        self.canvas.show_labels = on
        self.canvas.queue_draw()

    def _hover(self, kind, idx, x, y):
        if kind is None or not self.describe:
            self.tip.popdown()
            return
        title, rows = self.describe(kind, idx)
        rows = list(rows)
        if kind == "node" and (hi := self.canvas.heat_info(idx)):
            rows.append(("path strength", f"{hi[0]:.2f} · {hi[1]} hop{'s' if hi[1] != 1 else ''}"))
        elif kind == "node" and self.canvas.heat and idx not in self.canvas.heat:
            rows.append(("path strength", "unreachable in 6 hops"))
        elif kind == "edge" and self.canvas.edge_s:
            rows.append(("strength", f"{self.canvas.edge_s[idx]:.2f}"))
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4, margin_top=8, margin_bottom=8, margin_start=10, margin_end=10)
        box.append(Gtk.Label(label=title, xalign=0, css_classes=["heading", "mono"]))
        grid = Gtk.Grid(column_spacing=14, row_spacing=2)
        for r, (k, v) in enumerate(rows):
            grid.attach(Gtk.Label(label=k, xalign=0, css_classes=["dim-label", "caption"]), 0, r, 1, 1)
            grid.attach(Gtk.Label(label=v, xalign=0, css_classes=["mono", "caption"]), 1, r, 1, 1)
        box.append(grid)
        self.tip.set_child(box)
        rect = Gdk.Rectangle()
        rect.x, rect.y, rect.width, rect.height = int(x) + 14, int(y) + 10, 1, 1
        self.tip.set_pointing_to(rect)
        self.tip.popup()

    @staticmethod
    def _draw_legend(area, cr, w, h):
        grad = cairo.LinearGradient(0, 0, w, 0)
        for t, c in HEAT_STOPS:
            grad.add_color_stop_rgb(t, *c)
        cr.set_source(grad)
        cr.rectangle(0, 0, w, 8)
        cr.fill()
        fg = area.get_color()
        cr.select_font_face("monospace")
        cr.set_font_size(10)
        for t, label in ((0.0, "≤0.01"), (0.25, "0.03"), (0.5, "0.1"), (0.75, "0.3"), (1.0, "1.0")):
            x = t * w
            ext = cr.text_extents(label)
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.4)
            cr.rectangle(min(max(x - 0.5, 0), w - 1), 8, 1, 4)
            cr.fill()
            cr.set_source_rgba(fg.red, fg.green, fg.blue, 0.65)
            cr.move_to(0 if t == 0 else (x - ext.x_advance if t == 1 else x - ext.x_advance / 2), 24)
            cr.show_text(label)

    def _heat_changed(self, src, reached):
        self.legend.set_opacity(1.0 if src is not None else 0.7)
        self.legend_lbl.set_text(f"from {self.canvas.nodes[src]['label']} · {reached} nodes reached" if src is not None
                                 else "select or drag a node")

    def _mode(self, is3d):
        self.canvas.set_mode("3d" if is3d else "2d")
        self.caption.set_visible(is3d)

    def set_caption(self, text):
        self.caption.set_text(text)

    def _selected(self, i):
        self.hud.set_visible(i is not None)
        if self._on_select:
            self._on_select(i)

    def set_hud(self, title, sub, actions):
        self.hud_title.set_text(title)
        self.hud_sub.set_text(sub)
        while (c := self.hud_actions.get_first_child()):
            self.hud_actions.remove(c)
        for label, cb, cls in actions:
            b = Gtk.Button(label=label, css_classes=cls)
            b.connect("clicked", lambda *_, f=cb: f())
            self.hud_actions.append(b)
