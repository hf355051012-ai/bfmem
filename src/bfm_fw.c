/* FW の更新(ConnectX のフラッシュの FW を、FW 自身に書かせる)。
 *
 * 方式は Linux の mlx5_core(fw.c)+ mlxfw(mlxfw_fsm.c)と同じ。FW の中の更新の手順(FSM)を、
 * ACCESS_REG のレジスタで進める:
 *   MCAM  どのレジスタに対応しているか
 *   MCQS  部品(component)の一覧。FW の像の部品(identifier = BOOT_IMG)の番号を探す
 *   MCQI  部品の能力(受けられる大きさ・1 回に渡せる大きさ・PSID を確かめるか)と版
 *   MCC   FSM の命令(LOCK / UPDATE_COMPONENT / VERIFY / ACTIVATE / CANCEL / RELEASE)と状態
 *   MCDA  部品のデータを渡す
 * 流れ: 状態が IDLE か確かめる -> LOCK -> LOCKED を待つ -> UPDATE_COMPONENT -> DOWNLOAD を待つ ->
 *       MCDA で全部渡す -> VERIFY -> LOCKED を待つ -> (ACTIVATE -> LOCKED を待つ)-> RELEASE。
 * 失敗したら、部品の更新を始めていれば CANCEL、手順を取っていれば RELEASE を出してから終わる。
 *
 * 起動しなくなる危険を小さくするために:
 *   - FW は今動いている像とは別の置き場に書き、VERIFY で確かめてから ACTIVATE で切り替える。
 *     ACTIVATE は VERIFY が通った後にしか出さない。BFM_FW_VERIFY_ONLY では ACTIVATE を出さずに取り消す。
 *   - 対応していない装置(MCAM に MCQS / MCQI / MCC / MCDA が無い)では何もしない。
 *   - 像は配布の .bin(フラッシュの像)を受け、FW に渡す部分を mstflint と同じに切り出す(img_parse())。
 *     先頭の印(MTFW の 16 バイト)と ITOC(像の目次)の印・CRC が合わなければ、FW が受けられる大きさを
 *     超えれば、書かない(MFA2・zip・壊れたファイルを誤って渡したときに断る)。
 *     ボードごとの情報(MAC・GUID・VPD など。ITOC の device_data の欄)は渡さない。
 *   - FW が PSID(ボードの種類)の一致を確かめない装置には、g_bfm_fw_allow_no_psid_check が 0 なら書かない。
 *   - FSM が IDLE でなければ(ほかの道具・ほかのポートが更新している)何もしない。
 *   - 装置のリセット(MFRL)はかけない。新しい FW は電源を入れ直したときに有効になる。
 *
 * 呼び出しは待たない: FW のコマンドを出したら BFM_INPROGRESS を返し、次の呼び出しで完了を見る。
 * FSM の状態を待つところは、200ms ごとに MCC を読み、30 秒で時間切れ(Linux の mlxfw と同じ)。
 * FW のコマンドの entry と作業域はポートをまたいで使うので、g_bfm_ctl_lock の中で進める。 */
#include "bfm_internal.h"

#include <string.h>

#define FSM_POLL_MS        200u      /* FSM の状態を読み直す間隔(mlxfw の MLXFW_FSM_STATE_WAIT_CYCLE_MS)*/
#define FSM_TIMEOUT_MS     30000u    /* FSM の状態が変わるのを待つ上限(MLXFW_FSM_STATE_WAIT_TIMEOUT_MS)*/
#define CHUNK_MAX          2048u     /* MCDA 1 回で渡すデータの上限(バイト。FW の上限がこれより小さければそちら)*/
#define REG_MAX            (24u + 128u)  /* 読み書きするレジスタの最大(MCQI の頭 24 + 版の情報 124)*/

/* MCQS の identifier・MCQI の info_type(Linux の fw.c)*/
#define MCQS_IDENTIFIER_BOOT_IMG     0x1u
#define MCQI_INFO_TYPE_CAPABILITIES  0x0u
#define MCQI_INFO_TYPE_VERSION       0x1u
#define MCQI_CAP_BYTES               20u   /* struct mlx5_ifc_mcqi_cap_bits */
#define MCQI_VERSION_BYTES           124u  /* struct mlx5_ifc_mcqi_version_bits */

/* MCC の命令(Linux の fw.c の enum mlx5_reg_mcc_instruction)*/
#define MCC_LOCK                     0x01u
#define MCC_RELEASE                  0x02u
#define MCC_UPDATE_COMPONENT         0x03u
#define MCC_VERIFY_COMPONENT         0x04u
#define MCC_ACTIVATE                 0x06u
#define MCC_CANCEL                   0x08u

/* FW の像(FS3。ConnectX-4 / 4 Lx)の形(mstflint の fs3_ops.cpp・cibfw_layouts.c)*/
#define ITOC_SEARCH_END              0x10000u  /* ITOC を探す範囲(4KiB ごと)*/
#define ITOC_ENTRY_BYTES             32u       /* ITOC の頭・欄の大きさ */
#define ITOC_MAX_ENTRIES             64u       /* 欄の数の上限(mstflint の MAX_TOCS_NUM)*/
#define ITOC_TYPE_END                0xffu     /* 最後の欄の種類 */
#define FW_MASK_MAX                  16u       /* 0xFF で消すボードの情報の欄の数の上限 */

/* MCC の control_state(Linux の mlxfw.h の enum mlxfw_fsm_state)*/
#define FSM_IDLE                     0u
#define FSM_LOCKED                   1u
#define FSM_DOWNLOAD                 3u

/* 手順 */
enum {
    F_IDLE = 0,       /* 始めていない(次の呼び出しで始める)*/
    F_MCAM,           /* MCAM: 対応しているレジスタを確かめる */
    F_MCQS,           /* MCQS: FW の像の部品の番号を探す */
    F_CAP,            /* MCQI: 部品の能力を読み、像を確かめる */
    F_VER_RUN,        /* MCQI: 今動いている版を表示する(対応していれば)*/
    F_QUERY,          /* MCC 読み: FSM が IDLE か確かめ、手順の番号(handle)を得る */
    F_LOCK,           /* MCC: LOCK_UPDATE_HANDLE */
    F_WAIT_LOCK,      /* LOCKED を待つ */
    F_UPDATE,         /* MCC: UPDATE_COMPONENT */
    F_WAIT_DL,        /* DOWNLOAD を待つ */
    F_DATA,           /* MCDA: データを渡す */
    F_VERIFY,         /* MCC: VERIFY_COMPONENT */
    F_WAIT_VERIFY,    /* LOCKED を待つ(FW が確かめて書き終える)*/
    F_ACTIVATE,       /* MCC: ACTIVATE */
    F_WAIT_ACT,       /* LOCKED を待つ */
    F_CANCEL,         /* MCC: CANCEL(失敗・取り消し・BFM_FW_VERIFY_ONLY)*/
    F_RELEASE,        /* MCC: RELEASE_UPDATE_HANDLE */
    F_VER_NEW,        /* MCQI: 次の起動の版を表示する(切り替えたとき)*/
    F_DONE            /* 済んだ(結果を返して F_IDLE に戻る)*/
};

typedef struct {
    int step;                        /* 手順(F_*)*/
    int rc;                          /* 結果(失敗の理由。取り消してから返す)*/
    int cancel;                      /* bfm_fw_update_cancel() で取り消しを頼まれた */
    int locked;                      /* LOCK が通った(終わるときに RELEASE が要る)*/
    int started;                     /* UPDATE_COMPONENT が通った(失敗したら CANCEL が要る)*/
    int activated;                   /* ACTIVATE が通った(もう取り消さない)*/
    int mode;                        /* BFM_FW_VERIFY_ONLY / BFM_FW_ACTIVATE */
    const uint8_t *img;              /* 書く像(配布の .bin)*/
    uint32_t len;                    /* FW に渡す長さ(img_parse() が ITOC から求める)*/
    uint32_t mask[FW_MASK_MAX];      /* 0xFF で消して渡す ITOC の欄の位置(ボードの情報の欄)*/
    uint32_t nmask;                  /* mask[] の数 */
    uint32_t off;                    /* 次に渡す位置 */
    uint32_t chunk;                  /* MCDA 1 回で渡す長さ */
    uint16_t comp;                   /* FW の像の部品の番号(MCQS の component_index)*/
    uint32_t handle;                 /* 更新の手順の番号(MCC の update_handle)*/
    uint32_t info_mask;              /* MCQI で読める情報の種類(supported_info_bitmask)*/
    uint32_t pct;                    /* 最後に表示した進み具合(%)*/
    uint8_t  want;                   /* 待っている FSM の状態 */
    int      next;                   /* 待ち終えたら移る手順 */
    uint64_t t_wait;                 /* 待ち始めた時刻 */
    uint64_t t_poll;                 /* 最後に MCC を読んだ時刻 */
    bfm_cmd_t cmd;                   /* 出している ACCESS_REG */
} fw_state_t;

static fw_state_t s_fw[BFM_MAX_PORT];
static uint8_t s_in[BFM_MAX_PORT][16u + 16u + CHUNK_MAX];   /* ACCESS_REG の入力(頭 16 + レジスタ)*/
static uint8_t s_out[BFM_MAX_PORT][16u + REG_MAX];          /* ACCESS_REG の出力 */

static uint64_t ms_since(uint64_t t0)
{
    return (bfm_port_time_ns() - t0) / 1000000u;
}

/* これから出す ACCESS_REG のレジスタの部分(0 で埋めて返す)。出した後の呼び出しでは触らない。 */
static uint8_t *reg_in(unsigned port)
{
    memset(s_in[port], 0, sizeof(s_in[port]));
    return &s_in[port][16];
}

/* ACCESS_REG を出す・完了を見る(read: 1 = 読む、0 = 書く)。レジスタの入力は reg_in() で用意しておく。
 * 読んだレジスタは s_out[port][16..]。inlen / outlen はレジスタの部分の長さ。 */
static int access_reg(bfm_dev_t *d, fw_state_t *f, uint16_t reg, int read, uint32_t inlen, uint32_t outlen,
                      const char *what)
{
    if (!f->cmd.posted) {
        bfm_wr16be(&s_in[d->port][0], MLX5_CMD_ACCESS_REG);
        bfm_wr16be(&s_in[d->port][6], read ? 1u : 0u);
        bfm_wr16be(&s_in[d->port][10], reg);
    }
    return mlx5_cmd_run(d, &f->cmd, s_in[d->port], 16u + inlen, s_out[d->port], 16u + outlen, what);
}

/* MCC に命令を書く(Linux の mlx5_reg_mcc_set())。 */
static int mcc_set(bfm_dev_t *d, fw_state_t *f, uint8_t instr, uint16_t comp, uint32_t size, const char *what)
{
    if (!f->cmd.posted) {
        uint8_t *r = reg_in(d->port);
        r[3] = instr;                                  /* instruction */
        bfm_wr16be(&r[6], comp);                       /* component_index */
        bfm_wr24be(&r[9], f->handle & 0xffffffu);      /* update_handle */
        bfm_wr32be(&r[16], size);                      /* component_size */
    }
    return access_reg(d, f, MLX5_REG_MCC, 0, 32u, 32u, what);
}

/* FSM のエラーの番号(MCC の error_code。Linux の mlxfw の enum mlxfw_fsm_state_err)を文にする。 */
static const char *fsm_err_str(uint8_t e)
{
    switch (e) {
    case 1: return "一般のエラー";
    case 2: return "像のハッシュが合わない";
    case 3: return "この装置には当てはまらない像(PSID などが違う)";
    case 4: return "知らない鍵";
    case 5: return "認証に失敗した";
    case 6: return "署名の無い像";
    case 7: return "当てはまらない鍵";
    case 8: return "像の形が正しくない";
    case 9: return "前の更新の後、リセットを待っている";
    default: return "知らないエラー";
    }
}

/* MCQI の版の情報(s_out の 16 + 24 から)を表示する。 */
static void show_version(const bfm_dev_t *d, const char *what)
{
    const uint8_t *v = &s_out[d->port][16u + 24u];
    const uint32_t ver = bfm_rd32be(&v[4]);
    uint32_t n = v[3];                                  /* version_string_length */
    char s[93];
    if (n > 92u) n = 92u;
    memcpy(s, &v[32], n);
    s[n] = '\0';
    BFM_LOG("bfm: ポート %u: %s FW %u.%u.%u%s%s\n", d->port, what, ver >> 24, (ver >> 16) & 0xffu, ver & 0xffffu,
            n ? " " : "", s);
}

/* FW の像の目次(ITOC)の頭・欄の CRC(mstflint の CalcImageCRC。ビッグエンディアンの語 n 個、多項式 0x100b)。 */
static uint16_t itoc_crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffu;
    for (uint32_t i = 0; i < n + 1u; i++) {
        uint32_t o = (i < n) ? bfm_rd32be(&p[i * 4u]) : 0u;
        for (uint32_t b = 0; b < ((i < n) ? 32u : 16u); b++) {   /* 最後に 0 を 16 bit 流す */
            c = (c << 1) | (o >> 31);
            if (c & 0x10000u) c ^= 0x100bu;
            c &= 0xffffu;
            o <<= 1;
        }
    }
    return (uint16_t)(c ^ 0xffffu);
}

/* 配布の .bin(フラッシュの像)から、FW に渡す部分を決める(mstflint の FwExtract4MBImage(img, true)と同じ)。
 * 渡すのは先頭から、ITOC の相対番地の部分の終わりまで。先頭の 16 バイト(印)と、ボードの情報を指す
 * ITOC の欄(device_data)は 0xFF で消して渡す(f->mask[])。印・ITOC の CRC が合わなければ断る。 */
static int img_parse(fw_state_t *f, const uint8_t *b, uint32_t len)
{
    static const uint8_t magic[16] = {'M', 'T', 'F', 'W', 0x8c, 0xdf, 0xd0, 0x00,
                                      0xde, 0xad, 0x92, 0x70, 0x41, 0x54, 0xbe, 0xef};
    static const uint8_t itoc_sig[16] = {'I', 'T', 'O', 'C', 0x04, 0x08, 0x15, 0x16,
                                         0x23, 0x42, 0xca, 0xfa, 0xba, 0xca, 0xfe, 0x00};
    if (len < 0x2000u || memcmp(b, magic, sizeof(magic)) != 0) {
        BFM_LOG("bfm: bfm_fw_update: FW の像 (.bin) ではない (先頭が MTFW の印でない。zip は展開した .bin を渡す)\n");
        return BFM_E_PARAM;
    }
    uint32_t it = 0;
    for (uint32_t a = 0x1000u; a < ITOC_SEARCH_END && a + 2u * ITOC_ENTRY_BYTES <= len; a += 0x1000u) {
        if (memcmp(&b[a], itoc_sig, sizeof(itoc_sig)) == 0 && itoc_crc(&b[a], 7u) == bfm_rd16be(&b[a + 30u])) {
            it = a;
            break;
        }
    }
    if (it == 0u) {
        BFM_LOG("bfm: bfm_fw_update: 像の目次 (ITOC) が見つからない (ConnectX-4 / 4 Lx の形の像だけを扱う)\n");
        return BFM_E_PARAM;
    }
    uint32_t end = 0;
    f->nmask = 0;
    for (uint32_t i = 0;; i++) {
        const uint32_t ea = it + ITOC_ENTRY_BYTES * (i + 1u);
        if (i >= ITOC_MAX_ENTRIES || ea + ITOC_ENTRY_BYTES > len) {
            BFM_LOG("bfm: bfm_fw_update: 像の目次 (ITOC) が終わらない\n");
            return BFM_E_PARAM;
        }
        const uint8_t *e = &b[ea];
        if (e[0] == ITOC_TYPE_END) break;
        if (itoc_crc(e, 7u) != bfm_rd16be(&e[30])) {
            BFM_LOG("bfm: bfm_fw_update: 像の目次 (ITOC) の %u 番の欄の CRC が合わない (壊れた像)\n", i);
            return BFM_E_PARAM;
        }
        /* 欄の位置は adb の数え方(各語の上の bit から): type = bit 0..7、size = bit 8..29(語の数)、
         * relative_addr = bit 160、flash_addr = bit 161..189(語の番地)、device_data = bit 206 */
        const uint32_t size = ((bfm_rd32be(&e[0]) >> 2) & 0x3fffffu) * 4u;
        const uint32_t w5 = bfm_rd32be(&e[20]);
        const uint32_t addr = ((w5 >> 2) & 0x1fffffffu) * 4u;
        const int relative = (int)(w5 >> 31);
        const int device = (e[25] >> 1) & 1;
        if (relative) {
            if (addr > len || size > len - addr) {
                BFM_LOG("bfm: bfm_fw_update: 像の目次 (ITOC) の %u 番の欄がファイルの外を指す\n", i);
                return BFM_E_PARAM;
            }
            if (addr + size > end) end = addr + size;
        }
        if (device) {
            if (f->nmask >= FW_MASK_MAX) {
                BFM_LOG("bfm: bfm_fw_update: 像のボードの情報の欄が多すぎる\n");
                return BFM_E_PARAM;
            }
            f->mask[f->nmask++] = ea;
        }
    }
    if (end <= it || (end & 3u) != 0u) {
        BFM_LOG("bfm: bfm_fw_update: 像の目次 (ITOC) から FW の像の長さが決まらない\n");
        return BFM_E_PARAM;
    }
    f->len = end;
    return BFM_OK;
}

/* FW に渡す像の off からの 4 バイト(先頭の印とボードの情報の欄は 0xFF にする)。 */
static uint32_t img_word(const fw_state_t *f, uint32_t off)
{
    uint32_t w;
    if (off < 16u) return 0xffffffffu;
    for (uint32_t i = 0; i < f->nmask; i++)
        if (off >= f->mask[i] && off < f->mask[i] + ITOC_ENTRY_BYTES) return 0xffffffffu;
    memcpy(&w, f->img + off, 4);
    return w;
}

/* 失敗・取り消しで終わる手順へ移る(取った手順を返してから rc を返す)。 */
static void fail(fw_state_t *f, int rc)
{
    if (f->rc == BFM_OK) f->rc = rc;
    f->step = (f->started && !f->activated) ? F_CANCEL : f->locked ? F_RELEASE : F_DONE;
}

/* FSM が want になるのを待つ手順に入る(待ち終えたら next へ)。 */
static void wait_state(fw_state_t *f, uint8_t want, int next, int step)
{
    f->want = want;
    f->next = next;
    f->t_wait = bfm_port_time_ns();
    f->t_poll = 0;
    f->step = step;
}

/*=================================================================
 * 手順を 1 つ進める(g_bfm_ctl_lock の中で)。
 * 戻り値: BFM_INPROGRESS = 途中。それ以外 = 終わった(結果)。
 * ===============================================================*/
static int fw_step(bfm_dev_t *d, fw_state_t *f)
{
    const unsigned p = d->port;
    int rc;
    if (f->cancel && f->step != F_CANCEL && f->step != F_RELEASE && f->step != F_DONE && f->step != F_VER_NEW &&
        !f->cmd.posted && !f->activated) {
        BFM_LOG("bfm: ポート %u: FW の更新を取り消す\n", p);
        fail(f, BFM_E_STATE);
    }
    switch (f->step) {
    case F_MCAM: {
        if (!d->mcam_reg) {
            BFM_LOG("bfm: ポート %u: この FW には MCAM が無い (MCC の方式の更新に対応していない)\n", p);
            fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        if (!f->cmd.posted) (void)reg_in(p);          /* feature_group = 0・access_reg_group = 0 */
        if ((rc = access_reg(d, f, MLX5_REG_MCAM, 1, 72u, 72u, "ACCESS_REG(MCAM 読み)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        const uint8_t a = s_out[p][16u + 11u];         /* access_regs の DW0 の下位: mcqs / mcqi / mcc / mcda */
        if ((a & 0x0fu) != 0x0fu) {
            BFM_LOG("bfm: ポート %u: この FW は MCC の方式の更新に対応していない (MCAM 0x%x)\n", p, a & 0x0fu);
            fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        f->comp = 0;
        f->step = F_MCQS;
        return BFM_INPROGRESS;
    }
    case F_MCQS: {
        if (!f->cmd.posted) {
            uint8_t *r = reg_in(p);
            bfm_wr16be(&r[2], f->comp);                /* component_index */
        }
        if ((rc = access_reg(d, f, MLX5_REG_MCQS, 1, 16u, 16u, "ACCESS_REG(MCQS 読み)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        const uint8_t *r = &s_out[p][16];
        const uint16_t ident = bfm_rd16be(&r[6]);
        if (ident == MCQS_IDENTIFIER_BOOT_IMG) {
            BFM_DBG("bfm: ポート %u: FW の像の部品は %u 番 (更新の状態 %u)\n", p, f->comp, r[11] & 0x0fu);
            f->step = F_CAP;
            return BFM_INPROGRESS;
        }
        if ((r[0] & 0x80u) || f->comp >= 64u) {        /* last_index_flag */
            BFM_LOG("bfm: ポート %u: FW の像の部品が見つからない\n", p);
            fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        f->comp++;
        return BFM_INPROGRESS;
    }
    case F_CAP: {
        if (!f->cmd.posted) {
            uint8_t *r = reg_in(p);
            bfm_wr16be(&r[2], f->comp);                /* component_index */
            r[11] = MCQI_INFO_TYPE_CAPABILITIES;       /* info_type */
            bfm_wr16be(&r[22], MCQI_CAP_BYTES);        /* data_size */
        }
        if ((rc = access_reg(d, f, MLX5_REG_MCQI, 1, 24u, 24u + MCQI_CAP_BYTES, "ACCESS_REG(MCQI 能力)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        const uint8_t *c = &s_out[p][16u + 24u];
        f->info_mask = bfm_rd32be(&c[0]);
        const uint32_t max_size = bfm_rd32be(&c[8]);
        const uint32_t align = 1u << (c[12] >> 4);    /* log_mcda_word_size */
        const uint32_t max_write = bfm_rd16be(&c[14]);
        const int match_psid = (c[16] & 0x10u) != 0;
        BFM_LOG("bfm: ポート %u: FW の像の部品: 上限 %u バイト・1 回 %u バイト・単位 %u バイト・PSID を%s\n", p,
                max_size, max_write, align, match_psid ? "確かめる" : "確かめない");
        if (f->len > max_size) {
            BFM_LOG("bfm: ポート %u: 像 (%u バイト) が FW の受けられる大きさを超える\n", p, f->len);
            fail(f, BFM_E_PARAM);
            return BFM_INPROGRESS;
        }
        if (!match_psid && !g_bfm_fw_allow_no_psid_check) {
            BFM_LOG("bfm: ポート %u: この FW は像の PSID を確かめないので書かない "
                    "(別のボード用の像を書く危険がある。g_bfm_fw_allow_no_psid_check)\n", p);
            fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        uint32_t chunk = max_write < CHUNK_MAX ? max_write : CHUNK_MAX;
        if (align > 4u) chunk &= ~(align - 1u);
        chunk &= ~3u;
        if (chunk == 0u) {
            BFM_LOG("bfm: ポート %u: MCDA で渡せる大きさが 0\n", p);
            fail(f, BFM_E_NOTSUP);
            return BFM_INPROGRESS;
        }
        f->chunk = chunk;
        f->step = (f->info_mask & (1u << MCQI_INFO_TYPE_VERSION)) ? F_VER_RUN : F_QUERY;
        return BFM_INPROGRESS;
    }
    case F_VER_RUN:
    case F_VER_NEW: {
        const int pending = (f->step == F_VER_NEW);
        if (!f->cmd.posted) {
            uint8_t *r = reg_in(p);
            r[0] = pending ? 0x80u : 0u;               /* read_pending_component(1 = 次の起動の像)*/
            bfm_wr16be(&r[2], f->comp);
            r[11] = MCQI_INFO_TYPE_VERSION;
            bfm_wr16be(&r[22], MCQI_VERSION_BYTES);
        }
        rc = access_reg(d, f, MLX5_REG_MCQI, 1, 24u, 24u + MCQI_VERSION_BYTES, "ACCESS_REG(MCQI 版)");
        if (rc == BFM_INPROGRESS) return BFM_INPROGRESS;
        if (rc == BFM_OK) show_version(d, pending ? "次の起動の" : "今の");
        /* 版は表示するだけ。読めなくても進める */
        f->step = pending ? F_DONE : F_QUERY;
        return BFM_INPROGRESS;
    }
    case F_QUERY: {
        if (!f->cmd.posted) (void)reg_in(p);          /* update_handle = 0 */
        if ((rc = access_reg(d, f, MLX5_REG_MCC, 1, 32u, 32u, "ACCESS_REG(MCC 読み)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        const uint8_t *r = &s_out[p][16];
        const uint8_t st = r[15] & 0x0fu;
        if (st != FSM_IDLE) {
            BFM_LOG("bfm: ポート %u: FW の更新の手順がほかで使われている (状態 %u)。何もしない\n", p, st);
            fail(f, BFM_E_BUSY);
            return BFM_INPROGRESS;
        }
        f->handle = bfm_rd32be(&r[8]) & 0xffffffu;
        f->step = F_LOCK;
        return BFM_INPROGRESS;
    }
    case F_LOCK:
        if ((rc = mcc_set(d, f, MCC_LOCK, 0, 0, "ACCESS_REG(MCC LOCK)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        f->locked = 1;
        wait_state(f, FSM_LOCKED, F_UPDATE, F_WAIT_LOCK);
        return BFM_INPROGRESS;
    case F_WAIT_LOCK:
    case F_WAIT_DL:
    case F_WAIT_VERIFY:
    case F_WAIT_ACT: {
        if (!f->cmd.posted) {
            if (f->t_poll != 0 && ms_since(f->t_poll) < FSM_POLL_MS) return BFM_INPROGRESS;
            uint8_t *r = reg_in(p);
            bfm_wr24be(&r[9], f->handle & 0xffffffu);
        }
        if ((rc = access_reg(d, f, MLX5_REG_MCC, 1, 32u, 32u, "ACCESS_REG(MCC 読み)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        f->t_poll = bfm_port_time_ns();
        const uint8_t *r = &s_out[p][16];
        const uint8_t err = r[14], st = r[15] & 0x0fu;
        if (err != 0u) {
            BFM_LOG("bfm: ポート %u: FW の更新に失敗した: %s (error_code %u、状態 %u)\n", p, fsm_err_str(err), err, st);
            fail(f, BFM_E_HW);
            return BFM_INPROGRESS;
        }
        if (st == f->want) {
            f->step = f->next;
            return BFM_INPROGRESS;
        }
        if (ms_since(f->t_wait) > FSM_TIMEOUT_MS) {
            BFM_LOG("bfm: ポート %u: FW の更新の状態が %u ms 変わらない (状態 %u、待つ状態 %u)\n", p, FSM_TIMEOUT_MS, st,
                    f->want);
            fail(f, BFM_E_TIMEOUT);
        }
        return BFM_INPROGRESS;
    }
    case F_UPDATE:
        if ((rc = mcc_set(d, f, MCC_UPDATE_COMPONENT, f->comp, f->len, "ACCESS_REG(MCC UPDATE_COMPONENT)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        f->started = 1;
        f->off = 0;
        f->pct = 0;
        wait_state(f, FSM_DOWNLOAD, F_DATA, F_WAIT_DL);
        return BFM_INPROGRESS;
    case F_DATA: {
        const uint32_t n = (f->len - f->off < f->chunk) ? f->len - f->off : f->chunk;
        if (!f->cmd.posted) {
            uint8_t *r = reg_in(p);
            bfm_wr24be(&r[1], f->handle & 0xffffffu);   /* update_handle */
            bfm_wr32be(&r[4], f->off);                 /* offset */
            bfm_wr16be(&r[10], (uint16_t)n);           /* size */
            /* データは 4 バイトずつ、ホスト順の値として読んでビッグエンディアンで書く
             * (Linux の mlx5_reg_mcda_set() の htonl(*(u32 *)&data[j]) と同じ)。 */
            for (uint32_t j = 0; j < n; j += 4u) bfm_wr32be(&r[16u + j], img_word(f, f->off + j));
        }
        if ((rc = access_reg(d, f, MLX5_REG_MCDA, 0, 16u + n, 16u, "ACCESS_REG(MCDA)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        f->off += n;
        const uint32_t pct = (uint32_t)((uint64_t)f->off * 100u / f->len);
        if (pct / 10u != f->pct / 10u) BFM_LOG("bfm: ポート %u: FW の像を渡した %u%%\n", p, pct);
        f->pct = pct;
        if (f->off >= f->len) f->step = F_VERIFY;
        return BFM_INPROGRESS;
    }
    case F_VERIFY:
        if ((rc = mcc_set(d, f, MCC_VERIFY_COMPONENT, f->comp, 0, "ACCESS_REG(MCC VERIFY_COMPONENT)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        BFM_LOG("bfm: ポート %u: FW が像を確かめている\n", p);
        wait_state(f, FSM_LOCKED, F_ACTIVATE, F_WAIT_VERIFY);
        return BFM_INPROGRESS;
    case F_ACTIVATE:
        if (f->mode != BFM_FW_ACTIVATE) {
            BFM_LOG("bfm: ポート %u: FW が像を確かめた。確かめるだけなので切り替えずに取り消す\n", p);
            f->step = F_CANCEL;
            return BFM_INPROGRESS;
        }
        if ((rc = mcc_set(d, f, MCC_ACTIVATE, 0, 0, "ACCESS_REG(MCC ACTIVATE)")) != BFM_OK) {
            if (rc != BFM_INPROGRESS) fail(f, rc);
            return BFM_INPROGRESS;
        }
        f->activated = 1;
        wait_state(f, FSM_LOCKED, F_RELEASE, F_WAIT_ACT);
        return BFM_INPROGRESS;
    case F_CANCEL:
        if ((rc = mcc_set(d, f, MCC_CANCEL, 0, 0, "ACCESS_REG(MCC CANCEL)")) == BFM_INPROGRESS) return BFM_INPROGRESS;
        if (rc != BFM_OK) BFM_LOG("bfm: ポート %u: FW の更新の取り消しに失敗した\n", p);
        f->started = 0;
        f->step = F_RELEASE;
        return BFM_INPROGRESS;
    case F_RELEASE:
        if ((rc = mcc_set(d, f, MCC_RELEASE, 0, 0, "ACCESS_REG(MCC RELEASE)")) == BFM_INPROGRESS) return BFM_INPROGRESS;
        if (rc != BFM_OK) BFM_LOG("bfm: ポート %u: FW の更新の手順を手放せなかった\n", p);
        f->locked = 0;
        if (f->rc == BFM_OK && f->activated) {
            BFM_LOG("bfm: ポート %u: FW を書き、次の起動から使うよう切り替えた。電源を入れ直すと有効になる\n", p);
            f->step = (f->info_mask & (1u << MCQI_INFO_TYPE_VERSION)) ? F_VER_NEW : F_DONE;
        } else {
            if (f->rc == BFM_OK) BFM_LOG("bfm: ポート %u: FW の像の確かめが済んだ (次の起動の像は変えていない)\n", p);
            f->step = F_DONE;
        }
        return BFM_INPROGRESS;
    default:
        f->step = F_DONE;
        return BFM_INPROGRESS;
    }
}

int bfm_fw_update(unsigned port, const void *img, uint32_t len, int mode)
{
    if (port >= BFM_MAX_PORT) return BFM_E_PARAM;
    fw_state_t *f = &s_fw[port];
    bfm_dev_t *d = &g_bfm_dev[port];
    int rc0;
    if (f->step == F_IDLE) {
        const uint8_t *b = (const uint8_t *)img;
        if (b == NULL || (mode != BFM_FW_VERIFY_ONLY && mode != BFM_FW_ACTIVATE)) {
            BFM_LOG("bfm: bfm_fw_update: 引数が正しくない\n");
            return BFM_E_PARAM;
        }
        if (!d->used || !bfm_dev_ready(d)) return BFM_E_STATE;
        memset(f, 0, sizeof(*f));
        if ((rc0 = img_parse(f, b, len)) != BFM_OK) return rc0;
        f->img = b;
        f->mode = mode;
        f->rc = BFM_OK;
        f->step = F_MCAM;
        BFM_LOG("bfm: ポート %u: FW の更新を始める (.bin %u バイトのうち %u バイト、ボードの情報の欄 %u を除く、%s)\n",
                port, len, f->len, f->nmask, mode == BFM_FW_ACTIVATE ? "確かめて切り替える" : "確かめるだけ");
    }
    if (!bfm_trylock(&g_bfm_ctl_lock)) return BFM_INPROGRESS;
    int rc = BFM_INPROGRESS;
    if (!bfm_dev_ready(d) && !f->cmd.posted && f->step != F_DONE) {
        /* 途中でポートが止まった: もうコマンドは出せない(FW は手順の番号を放っておけば時間で手放す)*/
        BFM_LOG("bfm: ポート %u: FW の更新の途中でポートが使えなくなった\n", port);
        if (f->rc == BFM_OK) f->rc = BFM_E_STATE;
        f->step = F_DONE;
    }
    if (f->step != F_DONE) (void)fw_step(d, f);
    if (f->step == F_DONE) {
        rc = f->rc;
        f->step = F_IDLE;
    }
    bfm_unlock(&g_bfm_ctl_lock);
    return rc;
}

void bfm_fw_update_cancel(unsigned port)
{
    if (port < BFM_MAX_PORT && s_fw[port].step != F_IDLE) s_fw[port].cancel = 1;
}
