"""ConnectX の章(doc/spec/connectx.adoc)の図。gen.py から呼ばれ、images/hw_*.drawio.svg になる。

図は座標で書く(関数のフロー図・関係図のような自動の配置ではない)。箱の高さは行の数から決める。
種類(kind): grp = 枠(文字は左上)、sw = ソフトウェア、mem = ホストのメモリ、pub = 公開メモリ、
reg = レジスタ / MMIO、hw = NIC の中の働き、note = 注記(破線)。
手順の図(起動・データ経路)は gen.py のフロー図の配置(layout_flow)をそのまま使う。
"""


class Fig:
    def __init__(self, g):
        self.g = g
        self.nodes = []
        self.edges = []
        self.byid = {}

    def box(self, nid, kind, x, y, w, lines, h=None, align=None, head=False, dashed=False):
        """箱を置く。lines は文字列か行のリスト(長い行は折り返す)。"""
        if isinstance(lines, str):
            lines = [lines]
        width = w - 28                  # 字の幅の見積もりは少し狭いので、余裕を持って折り返す
        out = []
        for ln in lines:
            out += self.g.wrap(ln, width)
        need = len(out) * self.g.LINE_H + (22 if kind == "grp" else 14)
        nd = {"id": nid, "kind": kind, "text": "", "lines": out, "x": x, "y": y, "w": w,
              "h": max(h or 0, need), "dashed": dashed}
        if align:
            nd["align"] = align
        if head:
            nd["head"] = True
        if kind == "grp":
            self.nodes.insert(0, nd)    # 枠は先に描く(上に乗る箱と線を隠さない)
        else:
            self.nodes.append(nd)
        self.byid[nid] = nd
        return nd

    def edge(self, src, dst, pts, label="", lpos=None, both=False, dashed=False):
        e = {"src": src, "dst": dst, "label": label, "pts": pts, "both": both, "dashed": dashed}
        if lpos:
            e["lpos"] = lpos
        self.edges.append(e)

    # 箱の辺の上の点
    def L(self, nid, dy=None):
        n = self.byid[nid]
        return (n["x"], n["y"] + (n["h"] / 2 if dy is None else dy))

    def R(self, nid, dy=None):
        n = self.byid[nid]
        return (n["x"] + n["w"], n["y"] + (n["h"] / 2 if dy is None else dy))

    def T(self, nid, dx=None):
        n = self.byid[nid]
        return (n["x"] + (n["w"] / 2 if dx is None else dx), n["y"])

    def B(self, nid, dx=None):
        n = self.byid[nid]
        return (n["x"] + (n["w"] / 2 if dx is None else dx), n["y"] + n["h"])

    def bottom(self, nid):
        n = self.byid[nid]
        return n["y"] + n["h"]


# ---------------------------------------------------------------------------
# ブロック図
# ---------------------------------------------------------------------------

def block(g):
    f = Fig(g)
    # ホスト
    f.box("cpu", "sw", 40, 50, 460, ["CPU(x86-64)", "bfmem(本体・移植層)"], h=64, head=True)
    col = [("cmdq", ["コマンドキュー", "(4KiB。entry 0 だけ)"]), ("mbox", ["メールボックス", "(入力 / 出力)"]),
           ("fwp", ["FW に渡すページ", "(最大 64MiB)"]), ("eqr", ["EQ のリング", "(64B x 128)"]),
           ("gsi", ["GSI(QP1)の", "WQ・CQ・バッファ"])]
    col2 = [("wq", ["WQ(接続ごと)", "SQ 16KiB + RQ 4KiB"]), ("cqr", ["CQ のリング(接続ごと)", "64B x 256"]),
            ("dbr", ["doorbell record", "(QP・CQ ごとに 64B)"]), ("stage", ["中継バッファ", "(接続ごと。64KiB)"]),
            ("difb", ["T10-DIF の中継バッファ", "(接続ごと。128KiB)"])]
    y = 186
    for (a, ta), (b, tb) in zip(col, col2):
        f.box(a, "mem", 64, y, 206, ta, h=44)
        f.box(b, "mem", 284, y, 206, tb, h=44)
        y += 54
    f.box("dma", "grp", 54, 156, 446, "DMA 領域(g_bfm_dma_base。既定 96MiB)", h=y - 156 + 4)
    f.box("pub", "pub", 54, y + 16, 446, ["公開メモリ(g_bfm_mr_base。既定 1GiB)", "相手が RDMA で直接読み書きする"], h=48)
    f.box("hmem", "grp", 40, 128, 474, "ホストのメモリ(論理 = 物理。Linux は 仮想 = IOVA)",
          h=f.bottom("pub") + 14 - 128)
    f.box("host", "grp", 20, 20, 514, "ホスト", h=f.bottom("hmem") + 16 - 20)

    # ConnectX
    X1, X2, W = 720, 960, 220
    f.box("cfg", "reg", X1, 50, W, ["PCIe の設定空間", "ID・Command・BAR0・PCIe Capability(ECAM / VFIO)"], h=64)
    f.box("iseg", "reg", X1 + 10, 168, W - 20, ["Initialization Segment", "(BAR0 + 0x0000)"], h=48)
    f.box("uar", "reg", X1 + 10, 226, W - 20,
          ["UAR ページ(4KiB ずつ)", "+0x048 EQ のドアベル", "+0x800 ドアベル・BlueFlame"], h=70)
    f.box("bar0", "grp", X1, 136, W, "BAR0(32MiB〜。64bit)", h=f.bottom("uar") + 10 - 136)
    f.box("dmae", "hw", X1, f.bottom("bar0") + 22, W, ["DMA(バスマスタ)", "ホストのメモリを読み書きする"], h=50)
    f.box("fw", "hw", X2, 50, W, ["FW", "コマンドの処理・ページの管理・イベント"], h=64)
    f.box("ctx", "hw", X2, 136, W, ["QP / CQ / EQ / MKey の状態", "(ICM = FW に渡したページの中)"], h=56)
    f.box("exe", "hw", X2, 214, W, ["送受信の処理", "WQE の実行・CQE / EQE の書き込み・相手からの要求への応答"], h=70)
    f.box("roce", "hw", X2, 306, W, "RoCE v2(UDP / IPv4)・RC の再送", h=40)
    f.box("port", "hw", X2, 368, W, "Ethernet のポート(PAOS・PMTU)", h=40)
    f.box("nic", "grp", 700, 20, X2 + W + 20 - 700, "ConnectX(mlx5。ConnectX-4〜7 の PF)",
          h=max(f.bottom("port"), f.bottom("dmae")) + 16 - 20)
    f.box("peer", "sw", X2, f.bottom("nic") + 40, W, ["相手のノード(bfmem)", "同じ作りの ConnectX"], h=50)

    cr = f.R("cpu")[0]
    f.edge("cpu", "cfg", [(cr, 66), (X1, 66)], "① 設定の読み書き", lpos=(cr + 30, 61))
    f.edge("cpu", "iseg", [(cr, 86), (684, 86), (684, 192), (X1 + 10, 192)],
           "② コマンドキューの\n登録・コマンドの\nドアベル", lpos=(546, 112))
    f.edge("cpu", "uar", [(cr, 104), (670, 104), (670, 262), (X1 + 10, 262)],
           "③ WQE のドアベル /\nBlueFlame(WQE を\nMMIO で書く)", lpos=(546, 206))
    dy = f.byid["dmae"]["y"]
    f.edge("dmae", "dma", [(X1, dy + 18), (500, dy + 18)], "④ DMA(コマンド・WQE・データ・\nCQE・EQE・FW のページ)",
           lpos=(530, dy - 22), both=True)
    py = f.byid["pub"]["y"] + 24
    f.edge("dmae", "pub", [(X1 + 30, dy + 50), (X1 + 30, py), (500, py)], "⑤ 相手からの\nwrite / read / atomic",
           lpos=(530, py - 34), both=True)
    f.edge("fw", "ctx", [f.B("fw"), f.T("ctx")])
    f.edge("ctx", "exe", [f.B("ctx"), f.T("exe")], both=True)
    f.edge("exe", "roce", [f.B("exe"), f.T("roce")], both=True)
    f.edge("roce", "port", [f.B("roce"), f.T("port")], both=True)
    f.edge("exe", "dmae", [f.L("exe", 46), (X1 + W + 12, f.L("exe", 46)[1]), (X1 + W + 12, dy + 25), (X1 + W, dy + 25)],
           both=True)
    f.edge("port", "peer", [f.B("port"), f.T("peer")], "Ethernet", lpos=(X2 + W / 2 + 6, f.bottom("nic") + 26), both=True)
    return f


# ---------------------------------------------------------------------------
# コマンドキューとメールボックス
# ---------------------------------------------------------------------------

def mailbox(g):
    f = Fig(g)
    f.box("iseg", "reg", 20, 30, 260, ["Initialization Segment", "0x010 cmdq_phy_addr_high",
                                        "0x014 cmdq_phy_addr_low | log_cmdq_size | log_cmdq_stride",
                                        "0x018 コマンドのドアベル(bit n = entry n)",
                                        "0x1fc bit31 initializing"], align="left", head=True)
    EX = 400
    f.box("ent", "reg", EX, 30, 330, ["コマンドの entry 0(64 バイト。コマンドキューの先頭)",
                                      "[0] type = 7(PCIe)", "[4..7] 入力の長さ", "[8..15] 入力のメールボックスのアドレス",
                                      "[16..31] 入力の先頭 16 バイト(オペコード・op_mod など)",
                                      "[32..47] 出力の先頭 16 バイト(status・syndrome など)",
                                      "[48..55] 出力のメールボックスのアドレス", "[56..59] 出力の長さ",
                                      "[60] token  [61] signature",
                                      "[63] bit7..1 delivery status / bit0 ownership(1 = FW)"],
          align="left", head=True)
    yb = f.bottom("ent") + 50
    f.box("mi0", "mem", EX, yb, 150, ["入力のブロック 0", "(576 バイト)"])
    f.box("mi1", "mem", EX, yb + 80, 150, ["入力のブロック 1", "…"])
    f.box("mo0", "mem", EX + 180, yb, 150, ["出力のブロック 0", "(576 バイト)"])
    f.box("mo1", "mem", EX + 180, yb + 80, 150, ["出力のブロック 1", "…"])
    f.box("blk", "mem", EX + 360, yb, 270, ["ブロック(576 バイト)の中", "[0..511] データ(512 バイト)",
                                           "[560..567] 次のブロックのアドレス", "[568..571] ブロックの番号(0 から)",
                                           "[573] token  [574] ctrl_signature  [575] signature"],
          align="left", head=True)
    f.box("note", "note", 20, f.bottom("iseg") + 30, 260,
          ["入力・出力とも、先頭 16 バイトは entry の中に、残りはメールボックスに 512 バイトずつ入れる。",
           "bfmem は 16 + 9 x 512 = 4,624 バイトまで(ブロックは 1KiB おきに置く)。"])
    ey = f.byid["ent"]["y"]
    f.edge("iseg", "ent", [f.R("iseg", 30), (EX, ey + 30)], "コマンドキューの\n物理アドレス", lpos=(288, ey + 6))
    f.edge("ent", "mi0", [(EX + 40, f.bottom("ent")), (EX + 40, yb)], "in_ptr", lpos=(EX + 46, yb - 18))
    f.edge("ent", "mo0", [(EX + 220, f.bottom("ent")), (EX + 220, yb)], "out_ptr", lpos=(EX + 226, yb - 18))
    f.edge("mi0", "mi1", [f.B("mi0"), f.T("mi1")], "next", lpos=(EX + 81, yb + 68))
    f.edge("mo0", "mo1", [f.B("mo0"), f.T("mo1")], "next", lpos=(EX + 261, yb + 68))
    return f


# ---------------------------------------------------------------------------
# QP の持ち方
# ---------------------------------------------------------------------------

def qp(g):
    f = Fig(g)
    MX, RX = 290, 640          # 真ん中の列(NIC の中の状態)、右の列(ホストのメモリ)
    MW, RW = 260, 300
    f.box("conn", "sw", 20, 40, 200, ["接続 i(struct bfm_conn)", "qp(bfm_qp_t)・相手の QPN / PSN・相手の公開メモリ(raddr / rkey)"],
          head=True)
    f.box("qpc", "hw", MX, 30, MW, ["QP コンテキスト(NIC の中)", "st = RC、pd", "log_sq_size = 8、log_rq_size = 8",
                                    "uar_page = この接続の UAR", "cqn_snd = cqn_rcv = この接続の CQ",
                                    "dbr_addr", "cs_req = 0x11(scatter to CQE)"], align="left", head=True)
    f.box("wq", "mem", RX, 20, RW, ["WQ(DMA 領域。ページの表で渡す)", "RQ: 16B x 256 = 4KiB(RC では使わない)",
                                    "SQ: WQEBB 64B x 256 = 16KiB(RQ の直後)"], align="left", head=True)
    f.box("dbr", "mem", RX, f.bottom("wq") + 14, RW, ["QP の doorbell record(64B)",
                                                       "[0] RQ のカウンタ / [1] SQ のカウンタ(BE、下位 16bit)"],
          align="left", head=True)
    f.box("cqc", "hw", MX, f.bottom("qpc") + 40, MW, ["CQ コンテキスト(NIC の中)", "log_cq_size = 8、uar_page",
                                                       "c_eqn = EQ(全部の CQ で 1 本)", "dbr_addr"],
          align="left", head=True)
    cy = f.byid["cqc"]["y"]
    f.box("cqr", "mem", RX, cy - 6, RW, ["CQ のリング(64B x 256 = 16KiB)", "byte 63 = opcode << 4 | 形式 | owner"],
          align="left", head=True)
    f.box("cqd", "mem", RX, f.bottom("cqr") + 14, RW, ["CQ の doorbell record", "[0] consumer counter(BE、24bit)"],
          align="left", head=True)
    f.box("uar", "reg", MX, f.bottom("cqc") + 40, MW, ["UAR ページ(BAR0 + uarn x 4KiB)", "+0x800 BlueFlame の窓 0(ドアベル)",
                                                        "+0x800 + bf_buf_size 窓 1(交互に使う)"],
          align="left", head=True)
    f.box("eq", "hw", RX, max(f.bottom("cqd"), f.bottom("cqc")) + 40, RW,
          ["EQ(1 本)", "全部の CQ の通知先と、非同期のイベント(エラー・リンク・ページ要求)"], head=True)
    f.box("mk", "hw", 20, f.byid["uar"]["y"], 200, ["MKey(全部の接続で共有)", "lkey: 自分のメモリ全部(PA)",
                                                     "rkey: 公開メモリ(PA)"], head=True)
    q = f.byid["qpc"]
    mr = MX + MW
    f.edge("conn", "qpc", [f.R("conn", 30), (MX, f.byid["conn"]["y"] + 30)], "qpn", lpos=(228, f.byid["conn"]["y"] + 24))
    f.edge("qpc", "wq", [(mr, q["y"] + 40), (RX, q["y"] + 40)], "ページの表", lpos=(mr + 6, q["y"] + 34))
    f.edge("qpc", "dbr", [(mr, q["y"] + 108), (RX - 30, q["y"] + 108), (RX - 30, f.L("dbr")[1]), (RX, f.L("dbr")[1])],
           "dbr_addr", lpos=(mr + 6, q["y"] + 102))
    f.edge("qpc", "cqc", [f.B("qpc"), f.T("cqc")], "cqn", lpos=(MX + MW / 2 + 6, f.bottom("qpc") + 24))
    f.edge("cqc", "cqr", [(mr, cy + 20), (RX, cy + 20)])
    f.edge("cqc", "cqd", [(mr, cy + 50), (RX - 30, cy + 50), (RX - 30, f.L("cqd")[1]), (RX, f.L("cqd")[1])])
    ey = f.L("eq")[1]
    f.edge("cqc", "eq", [(mr, cy + 66), (RX - 50, cy + 66), (RX - 50, ey), (RX, ey)], "c_eqn", lpos=(RX - 46, ey - 6))
    ux = MX - 20
    f.edge("qpc", "uar", [(MX, q["y"] + 76), (ux, q["y"] + 76), (ux, f.byid["uar"]["y"] + 20), (MX, f.byid["uar"]["y"] + 20)],
           "uar_page", lpos=(ux - 56, q["y"] + 120))
    f.edge("conn", "mk", [f.B("conn"), f.T("mk")], "lkey / rkey を\nWQE に書く", lpos=(126, f.bottom("conn") + 30))
    return f


# ---------------------------------------------------------------------------
# 手順の図(フロー図の配置を使う)
# ---------------------------------------------------------------------------

BOOT = [
    {"do": "PCIe: FLR(g_bfm_pcie_flr)→ BAR0 の大きさを測って g_bfm_bar0_addr を書く → MRRS → "
           "Command に Memory Space・Bus Master・INTx 無効"},
    {"do": "BAR0 を張る(UC。UAR のページは後で WC の別名も)"},
    {"do": "fw_rev(0x000)・cmdif_rev(0x004 の上位 16bit = 5 か)を読む"},
    {"while": "initializing(0x1fc の bit31)が 1 ?", "body": [{"do": "1ms 待つ(最大 120 秒)"}]},
    {"do": "コマンドキュー(4KiB)の物理アドレスを 0x010 / 0x014 に書く(log_cmdq_stride = 6 を確かめる)"},
    {"while": "initializing が 1 ?", "body": [{"do": "1ms 待つ"}]},
    {"call": "ENABLE_HCA"},
    {"call": "QUERY_ISSI → SET_ISSI(1)"},
    {"call": "QUERY_PAGES(boot)→ MANAGE_PAGES(GIVE)"},
    {"call": "ACCESS_REG(HOST_ENDIANNESS)"},
    {"call": "QUERY / SET_HCA_CAP(general・atomic)、QUERY_HCA_CAP(roce)"},
    {"call": "QUERY_PAGES(init)→ MANAGE_PAGES(GIVE)"},
    {"call": "INIT_HCA"},
    {"call": "ALLOC_UAR → CREATE_EQ(arm しない)"},
    {"call": "ALLOC_PD → CREATE_MKEY(自分用 lkey・公開用 rkey)"},
    {"call": "ACCESS_REG(PMTU)→ ACCESS_REG(PAOS)でポートを上げ、リンクを待つ"},
    {"call": "MODIFY_NIC_VPORT_CONTEXT(roce_en)→ SET_ROCE_ADDRESS(GID の 0 番)"},
    {"do": "EQ のページ要求に答える(100ms 来なくなるまで。最大 2 秒)"},
    {"call": "GSI: ALLOC_UAR・CREATE_CQ・CREATE_QP(QP1)・RST2INIT・INIT2RTR・RTR2RTS"},
    {"call": "接続の枠ごとに ALLOC_UAR(RC の QP は接続のときに作る)"},
]

DATAPATH = [
    {"do": "WQE を組む(ctrl 16B + raddr 16B + inline のデータ / data セグメント / atomic セグメント)"},
    {"do": "SQ のリングの sq_pc 番目の WQEBB へ写す"},
    {"do": "doorbell record[1] = sq_pc(BE)"},
    {"if": "BlueFlame の窓があり、WQE が窓 1 面に収まる ?",
     "then": [{"do": "WQE 全体を窓(WC)へ 8 バイトずつ MMIO で書く"}],
     "else": [{"do": "WQE の先頭 8 バイトをドアベル(UC)へ書く"}]},
    {"do": "sfence(WC のバッファを吐き出す)、窓の面を切り替える"},
    {"do": "NIC: WQE を実行して相手へ送る(BlueFlame で受けた WQE はリングを読みに来ない)"},
    {"do": "NIC: 相手の ACK / 応答を受け、通知を求めた WQE の CQE を書く(32 バイトまでの応答は CQE の中へ)"},
    {"do": "CPU: CQE の owner を見て完了を拾い、consumer counter(CQ の doorbell record)を進める"},
]


def build(g):
    """図を作って {名前: (配置, 図の名前)} を返す。名前は images/hw_<名前>.drawio.svg になる。"""
    return {
        "block": (block(g), "ConnectX ブロック図"),
        "mailbox": (mailbox(g), "コマンドとメールボックス"),
        "qp": (qp(g), "QP の持ち方"),
        "boot": (g.layout_flow("", BOOT, "開始: bfm_init()"), "立ち上げの手順"),
        "datapath": (g.layout_flow("", DATAPATH, "開始: bfm_write() など"), "データ経路"),
    }
