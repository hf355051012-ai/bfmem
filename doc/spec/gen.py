#!/usr/bin/env python3
"""bfmem の関数仕様書を作る。

  python3 doc/spec/gen.py            # 全部を検査して doc/spec/bfmem_spec.adoc と images/*.drawio.svg を作る
                                     # (asciidoctor があれば bfmem_spec.html も作る)
  python3 doc/spec/gen.py --check    # 検査だけ(記述と関数の対応、引数の名前、フローの書き方)

入力:
  - ソース(下の FILES)。関数の定義・宣言・引数・呼び出し(関係図)はソースから自動で取り出す。
  - doc/spec/desc/*.json。関数ごとの説明(処理内容・引数の意味・戻り値・前提条件・注意事項)とフローの記述。
    書き方は doc/spec/desc/README.md。

出力:
  - doc/spec/bfmem_spec.adoc(1 つのファイル)
  - doc/spec/images/flow_*.drawio.svg / call_*.drawio.svg
    (draw.io の「編集できる SVG」。SVG として表示でき、draw.io でそのまま開いて編集できる)
  - doc/spec/bfmem_spec.html(asciidoctor があるときだけ。図を埋め込んだ 1 ファイル)
"""
import html
import json
import os
import re
import shutil
import subprocess
import sys
import uuid
from xml.sax.saxutils import escape as xesc

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SPEC = os.path.join(ROOT, "doc", "spec")
IMG = os.path.join(SPEC, "images")
DESC = os.path.join(SPEC, "desc")

# 章の順番。group はリンクの単位(同じ名前の関数がどちらの移植層のものかを決める)。
FILES = [
    ("include/bfm_port.h", ["linux", "vxworks"]),
    ("src/bfm_core.c", ["linux", "vxworks"]),
    ("src/bfm_mem.c", ["linux", "vxworks"]),
    ("src/bfm_pci.c", ["linux", "vxworks"]),
    ("src/mlx5_cmd.c", ["linux", "vxworks"]),
    ("src/mlx5_hca.c", ["linux", "vxworks"]),
    ("src/mlx5_qp.c", ["linux", "vxworks"]),
    ("src/bfm_cm.c", ["linux", "vxworks"]),
    ("src/bfm_ops.c", ["linux", "vxworks"]),
    ("port/linux_vfio/port_linux.c", ["linux"]),
    ("port/vxworks/port_vxworks.c", ["vxworks"]),
    ("test/bfm_test.c", ["linux"]),
    ("tools/bfm_peer.c", ["peer"]),
]

# 関数として扱うマクロ(呼び出しの関係図では、展開先の関数を呼んだことにする)。
MACRO_ALIAS = {
    "BFM_LOG": "bfm_port_log",
    "BFM_DBG": "bfm_port_log",
    "bfm_be32_to_cpu": "bfm_cpu_to_be32",
    "bfm_be64_to_cpu": "bfm_cpu_to_be64",
}
# 関数ではないマクロ(バリア)。関係図には「マクロ」として出す。
MACROS = {"bfm_dma_wmb", "bfm_wc_flush", "bfm_mb", "bfm_rmb"}
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "volatile", "__asm__", "asm",
            "_Static_assert", "__attribute__", "defined", "case", "do", "else"}


# ---------------------------------------------------------------------------
# ソースの解析
# ---------------------------------------------------------------------------

def strip_code(src):
    """コメント・文字列・文字定数を同じ長さの空白に置き換える(行と位置は保つ)。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            j += 1
            out.append(c + re.sub(r"[^\n]", " ", src[i + 1:j - 1]) + c)
            i = j
        elif c == "#" and (i == 0 or src[i - 1] == "\n"):
            j = i
            while True:  # 前処理の行(継続行を含む)は消す
                k = src.find("\n", j)
                if k < 0:
                    k = n
                if k > 0 and src[k - 1] == "\\":
                    j = k + 1
                    continue
                break
            out.append(re.sub(r"[^\n]", " ", src[i:k]))
            i = k
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_params(s):
    parts, depth, cur = [], 0, ""
    for c in s:
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        parts.append(cur)
    res = []
    for p in parts:
        p = " ".join(p.split())
        if p in ("void", ""):
            continue
        if p == "...":
            res.append(("...", "..."))
            continue
        m = re.match(r"^(.*?)([A-Za-z_]\w*)\s*((?:\[[^\]]*\])*)$", p)
        typ = (m.group(1) + m.group(3)).strip() if m else p
        res.append((typ.replace(" *", " *").strip(), m.group(2) if m else p))
    return res


def parse_file(rel):
    path = os.path.join(ROOT, rel)
    src = open(path, encoding="utf-8").read()
    code = strip_code(src)
    funcs = []
    i, n, depth = 0, len(code), 0
    while i < n:
        c = code[i]
        if c == "{":
            if depth == 0:
                # 直前の宣言部(前の ; か } の後ろから { まで)
                start = max(code.rfind(";", 0, i), code.rfind("}", 0, i)) + 1
                head = code[start:i]
                m = re.search(r"([A-Za-z_]\w*)\s*\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*$", head)
                if m and not re.search(r"\b(struct|enum|union)\s+\w*\s*$", head.strip()) and "=" not in head:
                    # 本体の終わり
                    d, j = 0, i
                    while j < n:
                        if code[j] == "{":
                            d += 1
                        elif code[j] == "}":
                            d -= 1
                            if d == 0:
                                break
                        j += 1
                    name = m.group(1)
                    decl = " ".join(src[start:i].split())
                    decl = re.sub(r"^.*?\*/\s*", "", decl) if "*/" in decl else decl
                    line = src.count("\n", 0, start + len(head) - len(head.lstrip())) + 1
                    body = code[i + 1:j]
                    params = split_params(m.group(2))
                    funcs.append({
                        "file": rel, "name": name, "decl": " ".join(code[start:i].split()),
                        "line": line, "body": body, "params": params,
                        "static": bool(re.search(r"\bstatic\b", head)),
                    })
                    i = j + 1
                    continue
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return funcs


def calls_in(body, params):
    names = []
    pnames = {p[1] for p in params}
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body):
        nm = m.group(1)
        if nm in KEYWORDS:
            continue
        # 型変換 (uint32_t)(x) の型名は ( の直前が ) なので拾わない。宣言の型 + 名前( は関数の定義だけ。
        nm = MACRO_ALIAS.get(nm, nm)
        if nm not in names:
            names.append(nm)
    return names, pnames


def build_graph():
    allf = []
    for rel, groups in FILES:
        for f in parse_file(rel):
            f["groups"] = groups
            f["id"] = (rel, f["name"])
            allf.append(f)
    by_group = {}
    for f in allf:
        for g in f["groups"]:
            by_group.setdefault(g, []).append(f)

    def resolve(caller, nm, group):
        cands = [f for f in by_group[group] if f["name"] == nm]
        same = [f for f in cands if f["file"] == caller["file"]]
        if same:
            return same[0]
        glob = [f for f in cands if not f["static"]]
        return glob[0] if glob else None

    for f in allf:
        f["callees"] = []      # [(表示名, 関数 or None, 種別)]
        f["callers"] = []
    for f in allf:
        names, pnames = calls_in(f["body"], f["params"])
        seen = set()
        for nm in names:
            if nm in MACROS:
                key = ("macro", nm)
                if key not in seen:
                    f["callees"].append((nm, None, "macro"))
                    seen.add(key)
                continue
            if nm in pnames:
                continue  # 関数ポインタの引数(記述の indirect で補う)
            targets = []
            for g in f["groups"]:
                t = resolve(f, nm, g)
                if t is not None and t not in targets:
                    targets.append(t)
            if targets:
                for t in targets:
                    if ("f", t["id"]) not in seen:
                        f["callees"].append((nm, t, "func"))
                        seen.add(("f", t["id"]))
                        t["callers"].append((f, "direct"))
            elif ("x", nm) not in seen:
                f["callees"].append((nm, None, "ext"))
                seen.add(("x", nm))
    # 同じファイルに同じ名前の定義が 2 つ(#if の両側。ビルドに入るのはどちらか 1 つ)。呼び出し元は両方に付ける。
    dups = {}
    for f in allf:
        dups.setdefault(f["id"], []).append(f)
    for group in dups.values():
        if len(group) > 1:
            callers = [c for g in group for c in g["callers"]]
            for g in group:
                g["dup"] = True
                g["callers"] = list(callers)
    return allf


# ---------------------------------------------------------------------------
# 文字の幅と折り返し
# ---------------------------------------------------------------------------

FONT = 12
LINE_H = 15


def tw(s):
    return sum(6.8 if ord(ch) < 0x80 else 12.2 for ch in s)


def wrap(text, maxw):
    lines = []
    for para in str(text).split("\n"):
        cur = ""
        for ch in para:
            if tw(cur + ch) > maxw and cur:
                # 英数字の途中なら、直前の区切りで折る
                k = max(cur.rfind(" "), cur.rfind(","), cur.rfind("("), cur.rfind("="))
                if k > len(cur) // 2 and ord(ch) < 0x80 and ch != " ":
                    lines.append(cur[:k + 1].rstrip())
                    cur = cur[k + 1:].lstrip() + ch
                else:
                    lines.append(cur.rstrip())
                    cur = ch.lstrip()
            else:
                cur += ch
        lines.append(cur)
    return lines


# ---------------------------------------------------------------------------
# フロー図の配置
# ---------------------------------------------------------------------------

VGAP, HGAP = 26, 36
BOX_W, DIA_W = 220, 250

KINDS = {"do", "call", "if", "while", "loop", "dowhile", "switch", "return", "break", "continue",
         "goto", "label", "end"}


class FlowError(Exception):
    pass


def step_kind(st):
    if not isinstance(st, dict):
        raise FlowError("手順は {...} で書く: %r" % (st,))
    ks = [k for k in st if k in KINDS]
    if len(ks) != 1:
        raise FlowError("手順の種類が 1 つでない: %r" % (st,))
    allowed = {"do": set(), "call": set(), "return": set(), "break": set(), "continue": set(),
               "goto": set(), "label": set(), "end": set(),
               "if": {"then", "else", "yes", "no"}, "while": {"body"}, "loop": {"body"},
               "dowhile": {"body"}, "switch": {"cases"}}[ks[0]]
    extra = set(st) - {ks[0]} - allowed
    if extra:
        raise FlowError("%s に使えない項目 %s" % (ks[0], sorted(extra)))
    return ks[0]


def expand_switch(st):
    """switch を if の連なりに直す。cases: [{"case": "値", "body": [...]}, ..., {"default": true, "body": [...]}]"""
    expr = st["switch"]
    cases = st["cases"]
    default = [c for c in cases if c.get("default")]
    normal = [c for c in cases if not c.get("default")]
    tail = default[0]["body"] if default else []
    for c in reversed(normal):
        if set(c) - {"case", "body"}:
            raise FlowError("switch の case に使えない項目: %r" % (c,))
        tail = [{"if": "%s が %s ?" % (expr, c["case"]), "then": c["body"], "else": tail}]
    return tail


def node_size(kind, text):
    if kind in ("if", "while", "dowhile"):
        lines = wrap(text, DIA_W - 90)
        return DIA_W, max(56, len(lines) * LINE_H + 34), lines
    if kind in ("start", "end", "return"):
        lines = wrap(text, 190)
        return 210, max(34, len(lines) * LINE_H + 14), lines
    if kind == "label":
        lines = wrap(text, 110)
        return 130, max(30, len(lines) * LINE_H + 12), lines
    if kind == "loop":
        lines = wrap(text, BOX_W - 50)
        return BOX_W, max(40, len(lines) * LINE_H + 16), lines
    lines = wrap(text, BOX_W - (34 if kind == "call" else 16))
    return BOX_W, max(36, len(lines) * LINE_H + 14), lines


class Layout:
    def __init__(self):
        self.nodes = []
        self.edges = []
        self.labels = {}      # goto のラベル名 -> node
        self.gotos = []       # (prefix, label, edge_label)
        self.loops = []       # ループの文脈
        self.n = 0
        self.max_x = 0

    def new_node(self, kind, text, cx, y):
        w, h, lines = node_size(kind, text)
        self.n += 1
        nd = {"id": "n%d" % self.n, "kind": kind, "text": text, "lines": lines,
              "x": cx - w / 2, "y": y, "w": w, "h": h}
        self.nodes.append(nd)
        self.max_x = max(self.max_x, cx + w / 2)
        return nd

    def side_route(self, src, pts, lane_x):
        """break / goto の線を、右の通路 lane_x まで横へ出す。四角い箱から直接出るときは箱の右の辺の中央から出す
        (下へ出すと、次の箱へ入る合流の線と重なって読み違える)。"""
        nd = next((n for n in self.nodes if n["id"] == src), None)
        if len(pts) == 1 and nd is not None and nd["kind"] not in ("if", "loop"):
            y = nd["y"] + nd["h"] / 2
            return [(nd["x"] + nd["w"], y), (lane_x, y)]
        lx, ly = pts[-1]
        return list(pts) + [(lx, ly + 8), (lane_x, ly + 8)]

    # --- 大きさを測る(中心からの左右の幅と高さ)---
    def measure(self, steps):
        L = R = H = 0
        first = True
        for st in steps:
            k = step_kind(st)
            if k in ("break", "continue", "goto"):
                continue
            l, r, h = self.measure1(st, k)
            L, R = max(L, l), max(R, r)
            H += h + (0 if first else VGAP)
            first = False
        return L, R, H

    def measure1(self, st, k):
        if k == "switch":
            return self.measure(expand_switch(st))
        if k in ("do", "call", "return", "label", "end"):
            w, h, _ = node_size(k, st[k] if isinstance(st[k], str) else "")
            return w / 2, w / 2, h
        if k == "if":
            dw, dh, _ = node_size("if", st["if"])
            L1, R1, H1 = self.measure(st.get("then", []))
            L2, R2, H2 = self.measure(st.get("else", []))
            L = max(L1, dw / 2)
            if st.get("else"):
                off = max(R1, dw / 2) + HGAP + L2
                R = off + R2
            else:
                R = max(R1, dw / 2) + HGAP
            H = dh + VGAP + max(H1, H2) + VGAP
            return L, R, H
        if k in ("while", "loop", "dowhile"):
            kind = "loop" if k == "loop" else "while"
            dw, dh, _ = node_size(kind, st[k])
            Lb, Rb, Hb = self.measure(st.get("body", []))
            L = max(Lb, dw / 2) + HGAP
            R = max(Rb, dw / 2) + HGAP + 14
            H = dh + VGAP + Hb + VGAP * 2
            return L, R, H
        raise FlowError("不明な手順 %r" % (st,))

    # --- 置く ---
    def connect(self, outs, target, tx=None, ty=None):
        """outs(前の手順から伸びる線)を target の上辺の中央へつなぐ。"""
        if tx is None:
            tx, ty = target["x"] + target["w"] / 2, target["y"]
        for src, pts, lab in outs:
            lx, ly = pts[-1]
            route = list(pts)
            if abs(lx - tx) > 0.5:
                midy = ty - VGAP / 2
                if ly > midy:
                    midy = ly + 6
                route += [(lx, midy), (tx, midy)]
            route.append((tx, ty))
            self.edges.append({"src": src, "dst": target["id"], "pts": dedup(route), "label": lab})

    def place(self, steps, cx, y, outs):
        """steps を中心 cx・上端 y から置く。outs は入ってくる線。戻り値: (出ていく線, 下端 y)"""
        first = True
        for st in steps:
            k = step_kind(st)
            if k == "break":
                if not self.loops:
                    raise FlowError("ループの外で break")
                self.loops[-1]["breaks"] += outs
                outs = []
                continue
            if k == "continue":
                if not self.loops:
                    raise FlowError("ループの外で continue")
                self.loops[-1]["conts"] += outs
                outs = []
                continue
            if k == "goto":
                for o in outs:
                    self.gotos.append((o, st["goto"]))
                outs = []
                continue
            if not first:
                y += VGAP
            first = False
            outs, y = self.place1(st, k, cx, y, outs)
        return outs, y

    def place1(self, st, k, cx, y, outs):
        if k == "switch":
            return self.place(expand_switch(st), cx, y, outs)
        if k in ("do", "call", "label"):
            nd = self.new_node(k, st[k], cx, y)
            self.connect(outs, nd)
            if k == "label":
                self.labels[st["label"]] = nd
            return [(nd["id"], [(cx, y + nd["h"])], "")], y + nd["h"]
        if k in ("return", "end"):
            text = st[k] if k == "end" else "戻る: %s" % st[k] if st[k] not in ("", True) else "戻る"
            nd = self.new_node("return", text, cx, y)
            self.connect(outs, nd)
            return [], y + nd["h"]
        if k == "if":
            yes, no = st.get("yes", "はい"), st.get("no", "いいえ")
            d = self.new_node("if", st["if"], cx, y)
            self.connect(outs, d)
            dw, dh = d["w"], d["h"]
            cy = y + dh / 2
            top = y + dh + VGAP
            L1, R1, H1 = self.measure(st.get("then", []))
            L2, R2, H2 = self.measure(st.get("else", []))
            bottom = top + max(H1, H2)
            res = []
            # はい: 真下
            if st.get("then"):
                o1, b1 = self.place(st["then"], cx, top, [(d["id"], [(cx, y + dh)], yes)])
            else:
                o1 = [(d["id"], [(cx, y + dh)], yes)]
            for src, pts, lab in o1:
                res.append((src, pts + [(pts[-1][0], bottom)], lab))
            # いいえ: 右
            if st.get("else"):
                ecx = cx + max(R1, dw / 2) + HGAP + L2
                o2, b2 = self.place(st["else"], ecx, top, [(d["id"], [(cx + dw / 2, cy), (ecx, cy)], no)])
                for src, pts, lab in o2:
                    res.append((src, pts + [(pts[-1][0], bottom)], lab))
            else:
                lane = cx + max(R1, dw / 2) + HGAP / 2
                res.append((d["id"], [(cx + dw / 2, cy), (lane, cy), (lane, bottom)], no))
            return res, bottom
        if k in ("while", "loop", "dowhile"):
            Lb, Rb, Hb = self.measure(st.get("body", []))
            kind = "loop" if k == "loop" else "while"
            dw, _, _ = node_size(kind, st[k])
            lane_l = cx - max(Lb, dw / 2) - HGAP / 2
            lane_r = cx + max(Rb, dw / 2) + HGAP / 2
            ctx = {"breaks": [], "conts": []}
            if k in ("while", "loop"):
                d = self.new_node("if" if k == "while" else "loop", st[k], cx, y)
                self.connect(outs, d)
                cy = d["y"] + d["h"] / 2
                self.loops.append(ctx)
                first_out = [(d["id"], [(cx, y + d["h"])], "はい" if k == "while" else "")]
                ob, bb = self.place(st.get("body", []), cx, y + d["h"] + VGAP, first_out)
                self.loops.pop()
                back_y = bb + VGAP / 2
                left_x = d["x"]
                for src, pts, lab in ob:
                    lx, ly = pts[-1]
                    yy = max(back_y, ly + 6)
                    self.edges.append({"src": src, "dst": d["id"], "label": lab,
                                       "pts": dedup(pts + [(lx, yy), (lane_l, yy), (lane_l, cy), (left_x, cy)])})
                for src, pts, lab in ctx["conts"]:
                    # continue: その場ですぐ左の通路へ出る(下へ伸ばすと後ろの箱を貫く)
                    lx, ly = pts[-1]
                    self.edges.append({"src": src, "dst": d["id"], "label": lab,
                                       "pts": dedup(pts + [(lx, ly + 8), (lane_l - 6, ly + 8), (lane_l - 6, cy),
                                                           (left_x, cy)])})
                bottom = bb + VGAP * 2
                res = []
                if k == "while":
                    res.append((d["id"], [(cx + d["w"] / 2, cy), (lane_r, cy), (lane_r, bottom)], "いいえ"))
                for src, pts, lab in ctx["breaks"]:
                    side = self.side_route(src, pts, lane_r + 8)
                    res.append((src, side + [(lane_r + 8, bottom)], lab))
                return res, bottom
            # dowhile: 本体 -> 条件。はい = 本体の先頭へ戻る
            self.loops.append(ctx)
            entry_y = y
            ob, bb = self.place(st.get("body", []), cx, y, outs)
            self.loops.pop()
            d = self.new_node("if", st[k], cx, bb + VGAP)
            self.connect(ob + ctx["conts"], d)
            cy = d["y"] + d["h"] / 2
            body_entry = [nd for nd in self.nodes if abs(nd["y"] - entry_y) < 0.5 and abs(nd["x"] + nd["w"] / 2 - cx) < 0.5]
            if body_entry:
                tgt = body_entry[0]
                self.edges.append({"src": d["id"], "dst": tgt["id"], "label": "はい",
                                   "pts": [(d["x"], cy), (lane_l, cy), (lane_l, entry_y - 6),
                                           (cx - 10, entry_y - 6), (cx - 10, entry_y)]})
            bottom = d["y"] + d["h"] + VGAP
            res = [(d["id"], [(cx, d["y"] + d["h"]), (cx, bottom)], "いいえ")]
            for src, pts, lab in ctx["breaks"]:
                side = self.side_route(src, pts, lane_r + 8)
                res.append((src, side + [(lane_r + 8, bottom)], lab))
            return res, bottom
        raise FlowError("不明な手順 %r" % (st,))


def dedup(pts):
    out = []
    for p in pts:
        if not out or abs(out[-1][0] - p[0]) > 0.01 or abs(out[-1][1] - p[1]) > 0.01:
            out.append(p)
    # 一直線に並ぶ途中の点を外す
    res = []
    for p in out:
        while len(res) >= 2 and ((abs(res[-2][0] - res[-1][0]) < 0.01 and abs(res[-1][0] - p[0]) < 0.01) or
                                 (abs(res[-2][1] - res[-1][1]) < 0.01 and abs(res[-1][1] - p[1]) < 0.01)):
            res.pop()
        res.append(p)
    return res


def layout_flow(fname, steps):
    lay = Layout()
    L, R, H = lay.measure(steps)
    cx = max(L, 105) + 30
    start = lay.new_node("start", "開始: %s()" % fname, cx, 20)
    outs, y = lay.place(steps, cx, start["y"] + start["h"] + VGAP,
                        [(start["id"], [(cx, start["y"] + start["h"])], "")])
    if outs:
        end = lay.new_node("end", "終了", cx, y + VGAP)
        lay.connect(outs, end)
    # goto: 右の外側の通路を通してラベルへ
    lane = lay.max_x + 24
    for (src, pts, lab), name in lay.gotos:
        if name not in lay.labels:
            raise FlowError("goto の行き先のラベル %s が無い" % name)
        t = lay.labels[name]
        ty = t["y"] + t["h"] / 2
        side = lay.side_route(src, pts, lane)
        lay.edges.append({"src": src, "dst": t["id"], "label": lab,
                          "pts": dedup(side + [(lane, ty), (t["x"] + t["w"], ty)])})
        lane += 10
    return lay


# ---------------------------------------------------------------------------
# 関係図の配置
# ---------------------------------------------------------------------------

def layout_call(f, extra_callers, extra_callees, entry_note):
    lay = Layout()
    rows_l, rows_r = [], []
    for c, how in f["callers"]:
        rows_l.append(("%s()" % c["name"], c["file"], "func", how == "indirect"))
    for nm, how in extra_callers:
        rows_l.append((nm, "", "note" if how == "note" else "func", True))
    for nm, t, kind in f["callees"]:
        if kind == "func":
            rows_r.append(("%s()" % nm, t["file"], "func", False))
        elif kind == "macro":
            rows_r.append(("%s()" % nm, "マクロ(バリア)", "ext", False))
        else:
            rows_r.append(("%s()" % nm, "外部", "ext", False))
    for nm in extra_callees:
        rows_r.append(("%s()" % nm, "関数ポインタ経由", "func", True))
    W = 230
    gap = 12

    def h_of(r):
        return 2 * LINE_H + 12 if r[1] else LINE_H + 16

    hl = sum(h_of(r) + gap for r in rows_l)
    hr = sum(h_of(r) + gap for r in rows_r)
    total = max(hl, hr, 60)
    xl, xc, xr = 20, 20 + W + 90, 20 + 2 * (W + 90)
    ch = 2 * LINE_H + 16
    center = {"id": "c", "kind": "center", "text": "%s()\n%s" % (f["name"], f["file"]),
              "lines": ["%s()" % f["name"], f["file"]], "x": xc, "y": 20 + total / 2 - ch / 2,
              "w": W, "h": ch}
    lay.nodes.append(center)
    ccy = center["y"] + ch / 2
    y = 20 + (total - hl) / 2
    for i, r in enumerate(rows_l):
        h = h_of(r)
        nd = {"id": "l%d" % i, "kind": r[2], "text": "", "lines": [r[0]] + ([r[1]] if r[1] else []),
              "x": xl, "y": y, "w": W, "h": h, "dashed": r[3]}
        lay.nodes.append(nd)
        ny = y + h / 2
        mx = xl + W + 45
        lay.edges.append({"src": nd["id"], "dst": "c", "label": "", "dashed": r[3],
                          "pts": dedup([(xl + W, ny), (mx, ny), (mx, ccy), (xc, ccy)])})
        y += h + gap
    y = 20 + (total - hr) / 2
    for i, r in enumerate(rows_r):
        h = h_of(r)
        nd = {"id": "r%d" % i, "kind": r[2], "text": "", "lines": [r[0]] + ([r[1]] if r[1] else []),
              "x": xr, "y": y, "w": W, "h": h, "dashed": r[3]}
        lay.nodes.append(nd)
        ny = y + h / 2
        mx = xc + W + 45
        lay.edges.append({"src": "c", "dst": nd["id"], "label": "", "dashed": r[3],
                          "pts": dedup([(xc + W, ccy), (mx, ccy), (mx, ny), (xr, ny)])})
        y += h + gap
    # 見出し
    lay.heads = [(xl + W / 2, "呼び出し元" if rows_l or entry_note else ""), (xr + W / 2, "呼び出し先" if rows_r else "")]
    if not rows_l:
        txt = entry_note or "(このソースの中からは呼ばれない)"
        nd = {"id": "l0", "kind": "note", "text": "", "lines": wrap(txt, W - 16), "x": xl,
              "y": ccy - 20, "w": W, "h": 40, "dashed": True}
        lay.nodes.append(nd)
        lay.edges.append({"src": "l0", "dst": "c", "label": "", "dashed": True,
                          "pts": [(xl + W, ccy), (xc, ccy)]})
        lay.heads[0] = (xl + W / 2, "呼び出し元")
    if not rows_r:
        nd = {"id": "r0", "kind": "note", "text": "", "lines": ["(ほかの関数を呼ばない)"], "x": xr,
              "y": ccy - 15, "w": W, "h": 30, "dashed": True}
        lay.nodes.append(nd)
        lay.heads[1] = (xr + W / 2, "呼び出し先")
    lay.max_x = xr + W
    return lay


# ---------------------------------------------------------------------------
# SVG と draw.io の XML
# ---------------------------------------------------------------------------

STYLE = {
    "start": "rounded=1;arcSize=50;whiteSpace=wrap;html=1;fillColor=#dae8fc;strokeColor=#6c8ebf;",
    "end": "rounded=1;arcSize=50;whiteSpace=wrap;html=1;fillColor=#dae8fc;strokeColor=#6c8ebf;",
    "return": "rounded=1;arcSize=50;whiteSpace=wrap;html=1;fillColor=#dae8fc;strokeColor=#6c8ebf;",
    "do": "rounded=0;whiteSpace=wrap;html=1;",
    "call": "shape=process;whiteSpace=wrap;html=1;backgroundOutline=1;size=0.06;fillColor=#fff2cc;strokeColor=#d6b656;",
    "if": "rhombus;whiteSpace=wrap;html=1;fillColor=#f5f5f5;",
    "loop": "shape=hexagon;perimeter=hexagonPerimeter2;whiteSpace=wrap;html=1;size=0.08;fillColor=#e1d5e7;strokeColor=#9673a6;",
    "label": "ellipse;whiteSpace=wrap;html=1;fillColor=#d5e8d4;strokeColor=#82b366;",
    "center": "rounded=1;whiteSpace=wrap;html=1;fillColor=#dae8fc;strokeColor=#6c8ebf;fontStyle=1;",
    "func": "rounded=1;whiteSpace=wrap;html=1;",
    "ext": "rounded=1;whiteSpace=wrap;html=1;fillColor=#f5f5f5;strokeColor=#999999;fontColor=#555555;",
    "note": "rounded=0;whiteSpace=wrap;html=1;dashed=1;strokeColor=#999999;fontColor=#555555;",
}
FILL = {"start": "#dae8fc", "end": "#dae8fc", "return": "#dae8fc", "do": "#ffffff", "call": "#fff2cc",
        "if": "#f5f5f5", "loop": "#e1d5e7", "label": "#d5e8d4", "center": "#dae8fc", "func": "#ffffff",
        "ext": "#f5f5f5", "note": "#ffffff"}
STROKE = {"start": "#6c8ebf", "end": "#6c8ebf", "return": "#6c8ebf", "call": "#d6b656", "loop": "#9673a6",
          "label": "#82b366", "center": "#6c8ebf", "ext": "#999999", "note": "#999999"}


def svg_shape(nd):
    x, y, w, h = nd["x"], nd["y"], nd["w"], nd["h"]
    fill, stroke = FILL[nd["kind"]], STROKE.get(nd["kind"], "#000000")
    dash = ' stroke-dasharray="4 3"' if nd.get("dashed") or nd["kind"] == "note" else ""
    k = nd["kind"]
    a = 'fill="%s" stroke="%s"%s' % (fill, stroke, dash)
    if k in ("start", "end", "return"):
        s = '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f" %s/>' % (x, y, w, h, min(h / 2, 17), a)
    elif k == "if":
        s = '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f" %s/>' % (
            x + w / 2, y, x + w, y + h / 2, x + w / 2, y + h, x, y + h / 2, a)
    elif k == "loop":
        d = w * 0.08
        s = '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f" %s/>' % (
            x + d, y, x + w - d, y, x + w, y + h / 2, x + w - d, y + h, x + d, y + h, x, y + h / 2, a)
    elif k == "label":
        s = '<ellipse cx="%.1f" cy="%.1f" rx="%.1f" ry="%.1f" %s/>' % (x + w / 2, y + h / 2, w / 2, h / 2, a)
    elif k == "call":
        d = w * 0.06
        s = ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" %s/>' % (x, y, w, h, a) +
             '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s"/>' % (x + d, y, x + d, y + h, stroke) +
             '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s"/>' % (x + w - d, y, x + w - d, y + h, stroke))
    elif k in ("center", "func", "ext"):
        s = '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="6" %s/>' % (x, y, w, h, a)
    else:
        s = '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" %s/>' % (x, y, w, h, a)
    lines = nd["lines"]
    color = "#555555" if k in ("ext", "note") else "#000000"
    top = y + h / 2 - (len(lines) - 1) * LINE_H / 2
    t = []
    for i, ln in enumerate(lines):
        weight = ' font-weight="bold"' if k == "center" and i == 0 else ""
        size = FONT - 2 if (k in ("center", "func", "ext") and i > 0) else FONT
        t.append('<text x="%.1f" y="%.1f" font-size="%d" text-anchor="middle" dominant-baseline="central" '
                 'fill="%s"%s>%s</text>' % (x + w / 2, top + i * LINE_H, size, color, weight, xesc(ln)))
    return s + "".join(t)


def svg_edge(e):
    pts = e["pts"]
    dash = ' stroke-dasharray="5 4"' if e.get("dashed") else ""
    s = '<polyline points="%s" fill="none" stroke="#333333"%s marker-end="url(#a)"/>' % (
        " ".join("%.1f,%.1f" % p for p in pts), dash)
    if e.get("label"):
        (x0, y0), (x1, y1) = pts[0], pts[1]
        if abs(x0 - x1) < 0.5:   # 下へ出る
            lx, ly, anc = x0 + 5, y0 + 11, "start"
        else:                    # 横へ出る
            lx, ly, anc = x0 + (6 if x1 > x0 else -6), y0 - 5, "start" if x1 > x0 else "end"
        s += '<text x="%.1f" y="%.1f" font-size="%d" text-anchor="%s" fill="#333333">%s</text>' % (
            lx, ly, FONT - 1, anc, xesc(e["label"]))
    return s


def mx_xml(lay, name):
    cells = ['<mxCell id="0"/>', '<mxCell id="1" parent="0"/>']
    byid = {}
    for nd in lay.nodes:
        byid[nd["id"]] = nd
        style = STYLE[nd["kind"]] + ("dashed=1;" if nd.get("dashed") else "")
        if nd["kind"] in ("center", "func", "ext"):
            val = html.escape(nd["lines"][0]) + "".join(
                '<br><font style="font-size:10px">%s</font>' % html.escape(l) for l in nd["lines"][1:])
        else:
            val = "<br>".join(html.escape(l) for l in nd["lines"])
        cells.append('<mxCell id="%s" value="%s" style="%s" vertex="1" parent="1">'
                     '<mxGeometry x="%.1f" y="%.1f" width="%.1f" height="%.1f" as="geometry"/></mxCell>' % (
                         nd["id"], html.escape(val, quote=True), style, nd["x"], nd["y"], nd["w"], nd["h"]))
    for i, e in enumerate(lay.edges):
        s, t = byid[e["src"]], byid[e["dst"]]
        (sx, sy), (ex, ey) = e["pts"][0], e["pts"][-1]
        style = ("edgeStyle=none;html=1;rounded=0;endArrow=block;endFill=1;exitX=%.4f;exitY=%.4f;exitDx=0;exitDy=0;"
                 "exitPerimeter=0;entryX=%.4f;entryY=%.4f;entryDx=0;entryDy=0;entryPerimeter=0;%s" % (
                     (sx - s["x"]) / s["w"], (sy - s["y"]) / s["h"], (ex - t["x"]) / t["w"], (ey - t["y"]) / t["h"],
                     "dashed=1;" if e.get("dashed") else ""))
        mids = "".join('<mxPoint x="%.1f" y="%.1f"/>' % p for p in e["pts"][1:-1])
        cells.append('<mxCell id="e%d" value="%s" style="%s" edge="1" parent="1" source="%s" target="%s">'
                     '<mxGeometry relative="1" as="geometry"><Array as="points">%s</Array></mxGeometry></mxCell>' % (
                         i, html.escape(e.get("label", ""), quote=True), style, e["src"], e["dst"], mids))
    for i, (hx, txt) in enumerate(getattr(lay, "heads", [])):
        if txt:
            cells.append('<mxCell id="h%d" value="%s" style="text;html=1;align=center;fontStyle=1;" vertex="1" '
                         'parent="1"><mxGeometry x="%.1f" y="0" width="160" height="18" as="geometry"/></mxCell>' % (
                             i, html.escape(txt), hx - 80))
    model = ('<mxGraphModel dx="800" dy="600" grid="0" gridSize="10" guides="1" tooltips="1" connect="1" arrows="1" '
             'fold="1" page="0" pageScale="1" math="0" shadow="0"><root>%s</root></mxGraphModel>' % "".join(cells))
    return ('<mxfile host="bfmem-gen" type="device"><diagram id="%s" name="%s">%s</diagram></mxfile>' % (
        uuid.uuid5(uuid.NAMESPACE_URL, name).hex[:20], html.escape(name), model))


def write_drawio_svg(lay, path, name):
    W = max(max(nd["x"] + nd["w"] for nd in lay.nodes),
            max((max(p[0] for p in e["pts"]) for e in lay.edges), default=0)) + 30
    H = max(max(nd["y"] + nd["h"] for nd in lay.nodes),
            max((max(p[1] for p in e["pts"]) for e in lay.edges), default=0)) + 20
    body = ['<defs><marker id="a" viewBox="0 0 10 10" refX="10" refY="5" markerWidth="8" markerHeight="8" '
            'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#333333"/></marker></defs>',
            '<rect x="0" y="0" width="%.0f" height="%.0f" fill="#ffffff"/>' % (W, H)]
    for hx, txt in getattr(lay, "heads", []):
        if txt:
            body.append('<text x="%.1f" y="13" font-size="12" font-weight="bold" text-anchor="middle">%s</text>' % (
                hx, xesc(txt)))
    body += [svg_edge(e) for e in lay.edges]
    body += [svg_shape(nd) for nd in lay.nodes]
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" version="1.1" width="%.0f" height="%.0f" viewBox="0 0 %.0f %.0f" '
           'font-family="Noto Sans CJK JP, Meiryo, sans-serif" content="%s">%s</svg>\n' % (
               W, H, W, H, html.escape(mx_xml(lay, name), quote=True), "".join(body)))
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(svg)


# ---------------------------------------------------------------------------
# 記述の読み込みと検査
# ---------------------------------------------------------------------------

REQUIRED = ["func", "summary", "args", "ret", "pre", "notes", "flow"]


def load_desc():
    descs = {}
    errs = []
    for fn in sorted(os.listdir(DESC)):
        if not fn.endswith(".json"):
            continue
        try:
            d = json.load(open(os.path.join(DESC, fn), encoding="utf-8"))
        except Exception as ex:
            errs.append("%s: JSON として読めない: %s" % (fn, ex))
            continue
        rel = d.get("file")
        if not rel:
            errs.append("%s: file が無い" % fn)
            continue
        descs[rel] = d
    return descs, errs


def slug(rel, name):
    return re.sub(r"[^A-Za-z0-9]+", "_", rel.rsplit(".", 1)[0]) + "__" + name


def fslug(f):
    """図のファイル名と節のアンカー。同じファイルに同じ名前の定義が 2 つある(#if の両側)ときは行番号を足す。"""
    return slug(f["file"], f["name"]) + ("_L%d" % f["line"] if f.get("dup") else "")


def check(allf, descs, partial=False):
    errs = []
    files = {rel for rel, _ in FILES}
    for rel in files:
        if rel not in descs and not partial:
            errs.append("%s: 記述のファイルが無い" % rel)
    for rel, d in descs.items():
        if rel not in files:
            errs.append("%s: 対象外のファイル" % rel)
            continue
        if not d.get("summary"):
            errs.append("%s: ファイルの summary が無い" % rel)
        srcf = {f["name"]: f for f in allf if f["file"] == rel}
        got = {}
        for e in d.get("functions", []):
            miss = [k for k in REQUIRED if k not in e]
            if miss:
                errs.append("%s:%s: 項目が足りない %s" % (rel, e.get("func"), miss))
                continue
            extra = set(e) - set(REQUIRED) - {"indirect", "entry_note"}
            if extra:
                errs.append("%s:%s: 使えない項目 %s" % (rel, e["func"], sorted(extra)))
            got[e["func"]] = e
        for nm in srcf:
            if nm not in got:
                errs.append("%s:%s: 記述が無い(ソースにある関数)" % (rel, nm))
        for nm, e in got.items():
            if nm not in srcf:
                errs.append("%s:%s: ソースに無い関数の記述" % (rel, nm))
                continue
            pn = [p[1] for p in srcf[nm]["params"]]
            if not isinstance(e["args"], dict):
                errs.append("%s:%s: args は {名前: 意味} で書く" % (rel, nm))
            elif list(e["args"]) != pn:
                errs.append("%s:%s: 引数の名前が違う。ソース %s / 記述 %s" % (rel, nm, pn, list(e["args"])))
            for k in ("summary", "ret", "pre", "notes"):
                if not isinstance(e[k], str) or not e[k].strip():
                    errs.append("%s:%s: %s が空" % (rel, nm, k))
            try:
                layout_flow(nm, e["flow"])
            except FlowError as ex:
                errs.append("%s:%s: フロー: %s" % (rel, nm, ex))
            for ind in e.get("indirect", []):
                if not any(f["name"] == ind for f in allf):
                    errs.append("%s:%s: indirect の %s がソースに無い" % (rel, nm, ind))
    return errs


# ---------------------------------------------------------------------------
# AsciiDoc
# ---------------------------------------------------------------------------

def a_esc(s):
    """表のセルに入れる文字列(| を逃がす)。`...` はそのまま等幅になる。"""
    return str(s).replace("|", "\\|")


def write_adoc(allf, descs):
    out = []
    out.append("""= bfmem 関数仕様書
:toc: left
:toclevels: 3
:sectnums:
:sectnumlevels: 3
:data-uri:
:imagesdir: images
:table-caption: 表
:figure-caption: 図
:toc-title: 目次

bfmem(ConnectX の BlueFlame でノード間のメモリへ write / read / atomic を出すライブラリ)の、
すべてのソースファイルの全関数の仕様。ソースファイルごとに章を、関数ごとに節を分ける。

== この文書の読み方

* 各関数の節には、*表*(関数名・処理内容・引数・戻り値・前提条件・注意事項)、*フロー図*、*関係図* を置く。
* *フロー図* はソースの制御の流れ(if・ループ・break・continue・goto・return)をそのまま写したもの。
** 角の丸い箱 = 開始・終了・戻る(戻り値)、四角 = 処理、両側に線のある箱 = このソースの関数の呼び出し、
   ひし形 = 判断(下へ「はい」、右へ「いいえ」)、六角形 = 終わりの条件を持たない繰り返し、緑の楕円 = goto の行き先。
** ループのひし形から下が繰り返す本体。本体の終わりからは左側の線でひし形へ戻る。
* *関係図* は、左がこの関数を呼ぶ関数、右がこの関数が呼ぶ関数。ソースを解析して作った。
** 白 = このソースの関数(下の小さい字は定義しているファイル)、灰色 = 外部の関数(C の標準ライブラリ・OS・コンパイラ組み込み)とバリアのマクロ、
   破線 = 関数ポインタ経由・OS から呼ばれるもの。
** `BFM_LOG()` / `BFM_DBG()` は `bfm_port_log()` の呼び出しとして数える。
** 移植層(`bfm_port_*`)は Linux 版と VxWorks 版の 2 つの実装がある。ライブラリの関数の関係図には両方を出す(下の小さい字のファイル名で見分ける)。
* 図はすべて draw.io の「編集できる SVG」(`images/*.drawio.svg`)。そのまま表示でき、draw.io で開くと編集できる。
* この文書と図は `doc/spec/gen.py` で、ソースと `doc/spec/desc/*.json` から作る。ソースを直したら作り直すこと。
""")
    for rel, _ in FILES:
        if rel not in descs:
            continue
        d = descs[rel]
        out.append("\n== `%s`\n\n%s\n" % (rel, d["summary"].strip()))
        fs = sorted([f for f in allf if f["file"] == rel], key=lambda f: f["line"])
        dmap = {e["func"]: e for e in d["functions"]}
        for f in fs:
            e = dmap[f["name"]]
            sl = fslug(f)
            dup = "(%d 行目の定義)" % f["line"] if f.get("dup") else ""
            out.append("\n[[%s]]\n=== `%s()`%s\n" % (sl, f["name"], dup))
            out.append('[cols="1,5",options="autowidth"]\n.%s()\n|===\n' % f["name"])
            out.append("|関数名 |`%s`(`%s` %d 行目)\n" % (f["name"], rel, f["line"]))
            out.append("|宣言 |`%s`\n" % a_esc(f["decl"]))
            out.append("|処理内容 a|%s\n" % e["summary"].strip())
            if f["params"]:
                rows = "".join("!`%s` !`%s` !%s\n" % (a_esc(t).replace("!", "\\!"), n, e["args"][n].replace("!", "\\!"))
                               for t, n in f["params"])
                out.append('|引数 a|\n[cols="2,1,4"]\n!===\n!型 !名称 !意味\n%s!===\n' % rows)
            else:
                out.append("|引数 |なし\n")
            out.append("|戻り値 a|%s\n" % e["ret"].strip())
            out.append("|前提条件 a|%s\n" % e["pre"].strip())
            out.append("|注意事項 a|%s\n" % e["notes"].strip())
            out.append("|===\n")
            out.append("\n.%s() のフロー図\nimage::flow_%s.drawio.svg[%s() のフロー図]\n" % (f["name"], sl, f["name"]))
            out.append("\n.%s() の関係図\nimage::call_%s.drawio.svg[%s() の関係図]\n" % (f["name"], sl, f["name"]))
    with open(os.path.join(SPEC, "bfmem_spec.adoc"), "w", encoding="utf-8") as fp:
        fp.write("".join(out))


def main():
    allf = build_graph()
    if "--list" in sys.argv:   # --list [ファイル]: 解析した関数・引数・呼び出し先を出す(記述を書くときの手がかり)
        want = [a for a in sys.argv[1:] if not a.startswith("--")]
        for f in allf:
            if want and f["file"] not in want:
                continue
            print("%s:%d %s(%s)" % (f["file"], f["line"], f["name"], ", ".join(n for _, n in f["params"])))
            print("    呼ぶ: %s" % ", ".join(nm for nm, _, _ in f["callees"]))
            print("    呼ばれる: %s" % ", ".join(sorted({c["name"] for c, _ in f["callers"]})))
        return
    descs, errs = load_desc()
    # 関数ポインタ経由の呼び出し(記述の indirect)を関係図に足す
    by_name = {}
    for f in allf:
        by_name.setdefault(f["name"], []).append(f)
    extra_callers = {}
    for rel, d in descs.items():
        for e in d.get("functions", []):
            for ind in e.get("indirect", []):
                for t in by_name.get(ind, []):
                    extra_callers.setdefault(t["id"], []).append(("%s()" % e["func"], "indirect"))
    partial = "--partial" in sys.argv   # 記述のあるファイルだけを検査して作る(書いている途中の確認用)
    errs += check(allf, descs, partial)
    if errs:
        print("\n".join(errs))
        print("NG: %d 件" % len(errs))
        sys.exit(1)
    if "--check" in sys.argv:
        print("OK: %d 関数" % len(allf))
        return
    os.makedirs(IMG, exist_ok=True)
    for fn in os.listdir(IMG):
        if fn.endswith(".drawio.svg"):
            os.remove(os.path.join(IMG, fn))
    for f in allf:
        if f["file"] not in descs:
            continue
        e = {x["func"]: x for x in descs[f["file"]]["functions"]}[f["name"]]
        sl = fslug(f)
        write_drawio_svg(layout_flow(f["name"], e["flow"]), os.path.join(IMG, "flow_%s.drawio.svg" % sl),
                         "%s フロー" % f["name"])
        ex_callers = [(c, "indirect") for c, _ in extra_callers.get(f["id"], [])]
        write_drawio_svg(layout_call(f, ex_callers, e.get("indirect", []), e.get("entry_note")),
                         os.path.join(IMG, "call_%s.drawio.svg" % sl), "%s 関係" % f["name"])
    write_adoc(allf, descs)
    print("OK: %d 関数、図 %d 枚" % (len(allf), 2 * len(allf)))
    write_html()


def write_html():
    """asciidoctor があれば bfmem_spec.html も作る(:data-uri: で図を埋め込むので 1 ファイルで見られる)。
    警告が 1 つでも出たら失敗にする(表の崩れなどを見逃さないため)。"""
    exe = shutil.which("asciidoctor")
    if exe is None:
        print("注意: asciidoctor が無いので HTML は作らない(sudo apt install asciidoctor)")
        return
    src = os.path.join(SPEC, "bfmem_spec.adoc")
    dst = os.path.join(SPEC, "bfmem_spec.html")
    r = subprocess.run([exe, "--failure-level=WARN", "-o", dst, src], cwd=SPEC)
    if r.returncode != 0:
        print("NG: asciidoctor が失敗した(上の警告を見ること)")
        sys.exit(1)
    print("OK: %s(%d KiB)" % (os.path.relpath(dst, ROOT), os.path.getsize(dst) // 1024))


if __name__ == "__main__":
    main()
