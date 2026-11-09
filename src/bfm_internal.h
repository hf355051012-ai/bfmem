#ifndef BFM_INTERNAL_H
#define BFM_INTERNAL_H

/* ライブラリの中だけで使う定義。アプリケーションは bfm.h だけを見る。 */

#include <stdint.h>
#include <stddef.h>
#include "bfm.h"
#include "bfm_config.h"
#include "bfm_port.h"

#define BFM_LOG(...) bfm_port_log(__VA_ARGS__)
#define BFM_DBG(...) do { if (g_bfm_verbose) bfm_port_log(__VA_ARGS__); } while (0)

#define BFM_PAGE 4096u   /* ConnectX が扱うページサイズ。ConnectX の仕様で規定 */

/* ---------------------------------------------------------------------
 * 鍵(複数のコアから接続・切断・bfm_poll() を呼べるように)
 *
 * x86-64 の xchg で取り、取れなければ pause しながら回って待つ(OS の機能は使わない)。
 * 回って待つので、同じ CPU の上で鍵を持ったタスクより優先度の高いタスクが鍵を待つと進まない。
 * データ経路(write / read / atomic / T10-DIF)は鍵を使わない(接続ごとの資源だけを触る)。
 * ------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t v;             /* 0 = 空き、1 = 誰かが持っている */
} bfm_lock_t;

static inline void bfm_cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline void bfm_lock(bfm_lock_t *l)
{
    while (__atomic_exchange_n(&l->v, 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&l->v, __ATOMIC_RELAXED) != 0u) bfm_cpu_relax();
    }
}

static inline void bfm_unlock(bfm_lock_t *l)
{
    __atomic_store_n(&l->v, 0u, __ATOMIC_RELEASE);
}

/* 接続管理(GSI・接続の枠の取り外し)・FW のコマンド・EQ の処理を 1 つずつにする鍵(bfm_core.c)。 */
extern bfm_lock_t g_bfm_ctl_lock;

/* ---------------------------------------------------------------------
 * リングの大きさ(log2)
 * ------------------------------------------------------------------- */
#define BFM_EQ_LOG       7u     /* 128 EQE x 64B = 8KiB */
#define BFM_RC_CQ_LOG    8u     /* 256 CQE */
#define BFM_RC_SQ_LOG    8u     /* 256 WQEBB x 64B = 16KiB */
#define BFM_RC_RQ_LOG    8u     /* 256 x 16B = 4KiB。RC では受信を使わないが、WQ の並び(RQ の後ろに SQ)を
                                 * vfio_nvme で実機確認済みの形のままにしてある */
#define BFM_GSI_CQ_LOG   8u
#define BFM_GSI_SQ_LOG   6u     /* 64 WQEBB。UD の SEND は 1 個 2 WQEBB */
#define BFM_GSI_RQ_LOG   8u     /* 256 x 16B */
#define BFM_GSI_RX_SLOT  128u   /* 受信 1 枠(GRH 40 + メッセージ 64 が入る)*/
#define BFM_GSI_PREPOST  128u   /* GSI に投稿しておく受信の数(64 本の接続が一度に REQ を送っても足りるように)*/
#define BFM_GSI_TXBUFS   16u    /* 送るメッセージの置き場(順に使い回す)*/
#define BFM_MBOX_BLOCKS  9u     /* メールボックスの鎖の長さ(16 + 9 x 512 = 4624 バイトまで)*/
#define BFM_MBOX_STRIDE  1024u
#define BFM_WQE_MAX      256u   /* 1 個の WQE の上限(= BlueFlame の窓 1 面)*/

/* ---------------------------------------------------------------------
 * mlx5 の定数(値は Linux の include/linux/mlx5/ の mlx5_ifc.h・device.h・qp.h と同じ)
 * ------------------------------------------------------------------- */
#define MLX5_VENDOR_ID 0x15b3u

/* Initialization Segment(BAR0 の先頭)*/
#define MLX5_ISEG_FW_REV         0x000u
#define MLX5_ISEG_CMDIF_REV      0x004u   /* 上位 16bit が cmdif_rev */
#define MLX5_ISEG_CMDQ_ADDR_H    0x010u
#define MLX5_ISEG_CMDQ_ADDR_L_SZ 0x014u
#define MLX5_ISEG_CMD_DBELL      0x018u
#define MLX5_ISEG_INITIALIZING   0x1fcu   /* bit31 = FW が初期化中 */
#define MLX5_CMD_IF_REV          5u

/* コマンド */
#define MLX5_CMD_QUERY_HCA_CAP   0x100u
#define MLX5_CMD_INIT_HCA        0x102u
#define MLX5_CMD_TEARDOWN_HCA    0x103u
#define MLX5_CMD_ENABLE_HCA      0x104u
#define MLX5_CMD_QUERY_PAGES     0x107u
#define MLX5_CMD_MANAGE_PAGES    0x108u
#define MLX5_CMD_SET_HCA_CAP     0x109u
#define MLX5_CMD_QUERY_ISSI      0x10au
#define MLX5_CMD_SET_ISSI        0x10bu
#define MLX5_CMD_CREATE_MKEY     0x200u
#define MLX5_CMD_DESTROY_MKEY    0x202u
#define MLX5_CMD_CREATE_EQ       0x301u
#define MLX5_CMD_CREATE_CQ       0x400u
#define MLX5_CMD_DESTROY_CQ      0x401u
#define MLX5_CMD_CREATE_QP       0x500u
#define MLX5_CMD_DESTROY_QP      0x501u
#define MLX5_CMD_RST2INIT_QP     0x502u
#define MLX5_CMD_INIT2RTR_QP     0x503u
#define MLX5_CMD_RTR2RTS_QP      0x504u
#define MLX5_CMD_QUERY_NIC_VPORT_CONTEXT  0x754u
#define MLX5_CMD_MODIFY_NIC_VPORT_CONTEXT 0x755u
#define MLX5_CMD_SET_ROCE_ADDRESS 0x761u
#define MLX5_CMD_ALLOC_PD        0x800u
#define MLX5_CMD_DEALLOC_PD      0x801u
#define MLX5_CMD_ALLOC_UAR       0x802u
#define MLX5_CMD_DEALLOC_UAR     0x803u
#define MLX5_CMD_ACCESS_REG      0x805u
#define MLX5_CMD_CREATE_PSV      0x600u

#define MLX5_REG_PMTU            0x5003u
#define MLX5_REG_PAOS            0x5006u
#define MLX5_REG_HOST_ENDIANNESS 0x7004u

#define MLX5_CAP_GENERAL 0u
#define MLX5_CAP_ATOMIC  3u
#define MLX5_CAP_ROCE    4u
#define MLX5_HCA_CAP_BYTES 4096u          /* QUERY / SET_HCA_CAP の capability(union 全体)*/

/* WQE */
#define MLX5_OPCODE_NOP          0x00u
#define MLX5_OPCODE_RDMA_WRITE   0x08u
#define MLX5_OPCODE_SEND         0x0au
#define MLX5_OPCODE_RDMA_READ    0x10u
#define MLX5_OPCODE_ATOMIC_CS    0x11u
#define MLX5_OPCODE_ATOMIC_FA    0x12u
#define MLX5_OPCODE_ATOMIC_MASKED_CS 0x14u          /* 拡張 atomic(大きさは ctrl の opmod で指定)*/
#define MLX5_OPCODE_ATOMIC_MASKED_FA 0x15u
#define MLX5_OPMOD_EXT_ATOMIC(log_sz) (0x08u | ((log_sz) - 2u))   /* 4 バイト = 0x08 */
#define MLX5_OPCODE_SET_PSV      0x20u
#define MLX5_OPCODE_UMR          0x25u
#define MLX5_WQE_CTRL_CQ_UPDATE  0x08u
#define MLX5_WQE_CTRL_SOLICITED  0x02u
#define MLX5_FENCE_SMALL         (1u << 5)   /* fm_ce_se の fence: INITIATOR_SMALL */
#define MLX5_FENCE_STRONG        (3u << 5)   /* STRONG_ORDERING */
#define MLX5_SEND_WQE_BB         64u
#define MLX5_BF_OFFSET           0x800u   /* UAR ページの中の BlueFlame の窓(ドアベルも同じ位置)*/
#define MLX5_INLINE_SEG          0x80000000u

/* CQE(64 バイト)*/
#define MLX5_CQE_REQ             0x0u
#define MLX5_CQE_RESP_SEND       0x2u
#define MLX5_CQE_SIG_ERR         0xcu
#define MLX5_CQE_REQ_ERR         0xdu
#define MLX5_CQE_RESP_ERR        0xeu
#define MLX5_CQE_INVALID         0xfu

/* QP */
#define MLX5_QP_ST_RC   0x0u
#define MLX5_QP_ST_QP1  0x8u
#define MLX5_QP_PM_MIGRATED 0x3u
#define MLX5_QPC_BYTES  232u
#define MLX5_QP1_QKEY   0x80010000u
#define MLX5_GRH_BYTES  40u

/* EQ のイベント */
#define MLX5_EVT_PATH_MIG          0x01u
#define MLX5_EVT_COMM_EST          0x02u
#define MLX5_EVT_SQ_DRAINED        0x03u
#define MLX5_EVT_CQ_ERROR          0x04u
#define MLX5_EVT_WQ_CATAS_ERROR    0x05u
#define MLX5_EVT_PATH_MIG_FAILED   0x07u
#define MLX5_EVT_PORT_CHANGE       0x09u
#define MLX5_EVT_PAGE_REQUEST      0x0bu
#define MLX5_EVT_WQ_INVAL_REQ      0x10u
#define MLX5_EVT_WQ_ACCESS_ERROR   0x11u
#define MLX5_EVT_SRQ_CATAS_ERROR   0x12u
#define MLX5_EVT_SRQ_LAST_WQE      0x13u
#define MLX5_EVT_SRQ_RQ_LIMIT      0x14u

/* ---------------------------------------------------------------------
 * FW のコマンド(待たずに進める。mlx5_cmd.c)
 * ------------------------------------------------------------------- */

/* コマンド 1 つぶんの進み具合。出してから完了を受け取るまで、呼び出しをまたいで持つ。
 * 使う手順ごとに 1 つ持ち、最初は 0 で埋めておく。 */
typedef struct {
    uint8_t  posted;                 /* 出して、まだ完了を受け取っていない */
    uint16_t opcode;                 /* 出したコマンドの番号(表示用)*/
    uint32_t outlen;                 /* 出したときの出力の長さ */
    uint64_t t0;                     /* 出した時刻(ns。答えない FW を見つける)*/
    const char *what;                /* 出したコマンドの名前(表示用)*/
} bfm_cmd_t;

/* FW へページを渡す手順(MANAGE_PAGES を 512 ページずつ出す)。 */
typedef struct {
    bfm_cmd_t cmd;                   /* 出している MANAGE_PAGES */
    uint16_t func;                   /* 渡す先の function(QUERY_PAGES・ページ要求の値)*/
    uint32_t left;                   /* まだ渡していないページの数 */
    uint32_t n;                      /* 出している MANAGE_PAGES で渡すページの数 */
    uint8_t  cant;                   /* 渡す代わりに CANT_GIVE を伝える */
} bfm_pages_t;

/* ---------------------------------------------------------------------
 * 装置
 * ------------------------------------------------------------------- */
typedef struct {
    /* PCIe */
    uint16_t vendor;                 /* PCI のベンダ ID(0x15b3 = Mellanox / NVIDIA)*/
    uint16_t device;                 /* PCI のデバイス ID(対応する ConnectX の表で name を引く)*/
    const char *name;                /* 装置の名前(表示用。例 "ConnectX-4 Lx")*/
    uint64_t bar0_phys;              /* BAR0 の物理アドレス */
    uint64_t bar0_size;              /* BAR0 の大きさ(バイト)*/
    volatile uint8_t *bar0;          /* BAR0 を張った先(UC。初期化セグメント・UAR のドアベル)*/

    /* コマンド */
    volatile uint8_t *cmdq;          /* FW へのコマンドキュー(DMA 領域の 1 ページ。入口 1 つだけ使う)*/
    volatile uint8_t *mbox_in;       /* 入力のメールボックスの鎖(BFM_MBOX_BLOCKS 個 x BFM_MBOX_STRIDE)*/
    volatile uint8_t *mbox_out;      /* 出力のメールボックスの鎖(同上)*/
    bfm_cmd_t *cmd_owner;            /* entry に出しているコマンドの持ち主(NULL = 空き)*/
    int cmd_dead;                    /* FW がコマンドに答えなくなった(もう出さない)*/
    int hca_enabled;                 /* ENABLE_HCA が通り、まだ TEARDOWN していない */

    /* FW に渡すページ */
    uint8_t *fw_pages;               /* FW に渡すページの置き場(DMA 領域の g_bfm_fw_pages_max ページ)*/
    uint32_t fw_pages_used;          /* そのうち FW に渡したページの数(先頭から順に渡す)*/

    /* 能力 */
    uint8_t  log_max_ra_req_qp;      /* QP 1 本が同時に出せる read / atomic の数の log2(能力)*/
    uint8_t  log_max_ra_res_qp;      /* QP 1 本が同時に受けられる read / atomic の数の log2(能力)*/
    uint8_t  num_vhca_ports;         /* vHCA のポートの数(0 でなければ SET_ROCE_ADDRESS にポート番号を入れる)*/
    uint8_t  page_request_disable;   /* FW がページの要求のイベントを出さない(1 なら EQ で受けない)*/
    uint8_t  bf_cap;                 /* BlueFlame がある */
    uint32_t bf_buf_size;            /* BlueFlame の窓 1 面の大きさ(2 面を交互に使う)*/
    uint8_t  qp_ts_format;           /* QPC の ts_format に書く値 */
    uint16_t atomic_ops;             /* 対応する atomic(bit0 CAS / bit1 FAA / bit2 拡張 CAS / bit3 拡張 FAA)*/
    uint16_t atomic_size_qp;         /* 拡張 atomic で扱える大きさ(bit n = 2^n バイト)*/
    uint8_t  atomic_mode;            /* QPC の atomic_mode(0 = 相手からの atomic を受けない)*/
    uint8_t  sho;                    /* 署名(T10-DIF)のオフロードがある */
    uint8_t  umr_fence;              /* UMR の WQE に付ける fence(fm_ce_se の値)*/

    /* 資源 */
    uint32_t eq_uarn;                /* EQ のドアベルに使う UAR の番号 */
    uint32_t eqn;                    /* EQ の番号 */
    uint32_t eq_cc;                  /* EQ の消費の数(次に読む EQE の位置と、所有の印の向きを決める)*/
    uint64_t eq_mask;                /* EQ で受けるイベントの種類(bit n = イベントの型 n)*/
    volatile uint8_t *eq_buf;        /* EQ のリング(DMA 領域)*/
    int eq_ready;                    /* EQ を作った(mlx5_eq_poll() が読んでよい)*/
    uint32_t pdn;                    /* 保護ドメインの番号(MKey・QP が属する)*/
    uint32_t lkey;                   /* 自分のメモリ全部(PA、ローカルの読み書き)*/
    uint32_t rkey;                   /* 公開メモリ(PA、[mr_base, mr_base + mr_size))*/
    uint8_t  rkey_key8;              /* 公開メモリの鍵の下位 8 ビット(作るときの時刻から決める)*/

    /* ポート */
    uint8_t  mac[6];                 /* 自分の MAC(g_bfm_local_mac。全部 0 なら装置から読む)*/
    uint8_t  gid[16];                /* 自分の GID(ip から作る ::ffff:a.b.c.d)*/
    uint32_t ip;                     /* 自分の IPv4 アドレス(g_bfm_local_ip。ホスト順)*/
    int      link_up;                /* リンクが上がっている(ポートの状態のイベントで更新する)*/

    /* 接続の枠 */
    uint32_t nconn;                  /* 用意した接続の枠の数(g_bfm_max_conn を BFM_MAX_CONN までに直したもの)*/
} bfm_dev_t;

extern bfm_dev_t g_bfm;

/* QP 1 本ぶんの資源(RC と GSI で共通)。 */
typedef struct {
    uint32_t qpn;                    /* QP の番号 */
    uint32_t cqn;                    /* CQ の番号(送受信で 1 つを共用)*/
    uint32_t uarn;                   /* ドアベルに使う UAR の番号 */
    uint32_t rq_log;                 /* RQ の WQE の数の log2 */
    uint32_t sq_log;                 /* SQ の WQEBB の数の log2 */
    uint32_t cq_log;                 /* CQ の CQE の数の log2 */
    volatile uint8_t *wq;            /* RQ(16B x 2^rq_log)の直後に SQ(64B x 2^sq_log)*/
    volatile uint8_t *sq;            /* SQ の先頭(wq + RQ の大きさ)*/
    volatile uint32_t *dbr;          /* [0] = RQ のカウンタ、[1] = SQ のカウンタ(BE)*/
    volatile uint8_t *cq;            /* CQ のリング(64B x 2^cq_log)*/
    volatile uint32_t *cq_dbr;       /* [0] = consumer counter(BE)*/
    uint32_t sq_pc;                  /* SQ に積んだ WQEBB の数(次に書く位置)*/
    uint32_t rq_pc;                  /* RQ に積んだ WQE の数(次に書く位置)*/
    uint32_t cq_cc;                  /* CQ から読んだ CQE の数(次に読む位置と所有の印の向き)*/
    uint32_t local_psn;              /* 自分が名乗る最初の PSN(作るときに時刻から決める)*/
    uint8_t  has_uar, has_cq, has_qp;   /* 番号 0 も有効な番号なので、作ったかは別に持つ */
} bfm_qp_t;

/* ---------------------------------------------------------------------
 * 接続
 * ------------------------------------------------------------------- */
enum { BFM_CS_FREE = 0, BFM_CS_SETUP, BFM_CS_UP, BFM_CS_ERROR, BFM_CS_CLOSED };

/* T10-DIF の中継バッファの面の数(片方を NIC が運ぶ間に、もう片方を CPU が用意する)。 */
#define BFM_DIF_SLOTS 2u

/* 署名の誤り(SIG_ERR の CQE)。同じ面に何個来ても、中身は最初の 1 個のもの。 */
struct bfm_sig_rec {
    uint32_t hits;                   /* 来た SIG_ERR の数(0 = 誤りなし)*/
    uint16_t synd;                   /* bit13 guard / bit12 app tag / bit11 ref tag */
    uint32_t exp_ts;                 /* guard << 16 | app tag(あるべき値)*/
    uint32_t act_ts;                 /* guard << 16 | app tag(実際の値)*/
    uint32_t exp_ref;                /* ref tag(あるべき値)*/
    uint32_t act_ref;                /* ref tag(実際の値)*/
    uint64_t off;                    /* 誤りの位置(署名つき MKey の中のバイト)*/
};

/* 接続 1 本。コアごとに別の接続を使うので、隣の接続とキャッシュラインを分ける。 */
struct __attribute__((aligned(64))) bfm_conn {
    int state;                       /* 接続の状態(BFM_CS_FREE / SETUP / UP / ERROR / CLOSED)*/
    int index;                       /* 枠の番号(g_bfm_conn の添字。表示に使う)*/
    bfm_qp_t qp;                     /* この接続の RC の QP・CQ・UAR */

    /* データ経路(接続のときに求めておく)*/
    volatile uint8_t *db;            /* UC の UAR ページ + 0x800(8 バイトのドアベル)*/
    volatile uint8_t *bf;            /* WC の UAR ページ + 0x800(NULL = BlueFlame なし)*/
    uint32_t bf_off;                 /* 2 面ある窓のどちらを使うか */
    uint32_t qpn8;                   /* qpn << 8 */
    uint32_t sq_done;                /* 完了が確かめられた WQEBB の位置 */
    uint32_t unsig;                  /* 完了通知を求めずに出した WQE の数 */
    uint8_t  wqe_bbs[1u << BFM_RC_SQ_LOG];  /* 位置ごとの WQE の大きさ(WQEBB の数)*/
    uint8_t  cs_req;                 /* scatter to CQE を有効にしたか */
    uint8_t  max_rd;                 /* 同時に出してよい read / atomic の数 */
    uint8_t  last_synd;              /* 最後のエラー完了の syndrome(0 = なし)*/
    uint8_t  last_vsynd;             /* 最後のエラー完了の vendor syndrome */
    volatile uint8_t *stage;         /* 中継バッファ(DMA 領域)*/
    uint32_t stage_size;             /* 中継バッファのバイト数(g_bfm_stage_size を 4KiB に切り上げたもの)*/
    const volatile uint8_t *last_scat;  /* 直前の CQE が応答データを抱えていればその CQE */

    /* T10-DIF(bfm_dif.c)。DIF 用の中継バッファは 2 面(BFM_DIF_SLOTS)に分けて交互に使う。 */
    volatile uint8_t *dif_buf;       /* DIF 用の中継バッファ(DMA 領域。面 i は + i * dif_slot_size)*/
    uint32_t dif_slot_size;          /* 1 面のバイト数 */
    uint32_t sig_mkey[BFM_DIF_SLOTS];        /* 面ごとの署名つき MKey(bfm_dif_attach() が入れる)*/
    struct bfm_sig_rec sig[BFM_DIF_SLOTS];   /* 面ごとの署名の誤り(poll_one() が入れ、bfm_dif.c が見て消す)*/

    /* 相手 */
    uint32_t peer_ip;                /* 相手の IPv4(ホスト順)*/
    uint8_t  peer_mac[6];            /* 相手の MAC(掛け側は呼び出し側が渡し、受け側は REQ から取る)*/
    uint8_t  peer_gid[16];           /* 相手の GID(peer_ip から作る ::ffff:a.b.c.d)*/
    uint32_t peer_qpn;               /* 相手の RC の QP の番号 */
    uint32_t peer_psn;               /* 相手が名乗った最初の PSN */
    uint64_t raddr;                  /* 相手の公開メモリの先頭(相手の g_bfm_mr_base)*/
    uint64_t rlen;                   /* 相手の公開メモリの大きさ */
    uint32_t rkey;                   /* 相手の公開メモリの rkey */
    uint8_t  path_mtu;               /* 経路 MTU(IB の符号。自分と相手の小さいほう)*/
    uint8_t  peer_rr;                /* 相手が受け付ける同時 read / atomic の数 */

    /* 接続の確立と切断(bfm_cm.c の状態機械)*/
    uint32_t local_id, peer_id;      /* 接続を見分ける乱数(0 = まだ無い)*/
    uint16_t port;                   /* 待ち受けの番号 */
    uint8_t  got_rep;                /* 掛け側: REP を受け取った */
    uint8_t  step;                   /* 状態機械の今の手順(bfm_cm.c の K_*)*/
    uint8_t  role;                   /* 1 = 掛け側(bfm_connect())、2 = 受け側(bfm_accept())*/
    uint8_t  abort;                  /* 畳む(bfm_disconnect() か確立の失敗)*/
    uint8_t  amode;                  /* RTR2RTS に付ける atomic_mode(断られたら 0 にして出し直す)*/
    int      result;                 /* 畳み終えたときに返す値 */
    uint32_t timeout_ms;             /* 掛け側: 確立の上限。受け側: REQ を待つ上限(0 = 待ち続ける)*/
    uint64_t t0;                     /* 確立を始めた時刻 */
    uint64_t t_sent;                 /* 掛け側: REQ を最後に送った時刻。畳む: BYE を出し始めた時刻 */
    bfm_cmd_t cmd;                   /* この接続が出している FW のコマンド */
};

extern struct bfm_conn g_bfm_conn[BFM_MAX_CONN];

/* ---------------------------------------------------------------------
 * 内部の関数
 * ------------------------------------------------------------------- */

/* bfm_mem.c: DMA 領域からの静的な切り出し */
void  bfm_mem_reset(void);
void *bfm_mem_alloc(uint64_t size, uint64_t align);
uint64_t bfm_mem_used(void);

/* 以下、戻り値が int で「待たずに進める」関数は、BFM_INPROGRESS の間は同じ引数で呼び直す。 */

/* bfm_pci.c */
void bfm_pci_setup_reset(void);
int  bfm_pci_setup_step(bfm_dev_t *d);
void bfm_pci_bus_master(int on);

/* mlx5_cmd.c */
int  mlx5_cmd_init(bfm_dev_t *d);
int  mlx5_cmd_run(bfm_dev_t *d, bfm_cmd_t *j, const uint8_t *in, uint32_t inlen, uint8_t *out,
                  uint32_t outlen, const char *what);
int  mlx5_cmd_drain(bfm_dev_t *d);
int  mlx5_destroy_obj(bfm_dev_t *d, bfm_cmd_t *j, uint16_t opcode, uint32_t id, const char *what);
int  mlx5_pages_start(bfm_dev_t *d, bfm_pages_t *p, uint16_t func, int32_t npages);
int  mlx5_pages_step(bfm_dev_t *d, bfm_pages_t *p);
void mlx5_eq_reset(void);
int  mlx5_eq_prepare(bfm_dev_t *d);
int  mlx5_eq_create(bfm_dev_t *d, bfm_cmd_t *j);
int  mlx5_eq_poll(bfm_dev_t *d);
int  mlx5_eq_busy(void);

/* mlx5_hca.c */
void mlx5_hca_init_reset(void);
int  mlx5_hca_init_step(bfm_dev_t *d);
int  mlx5_hca_teardown(bfm_dev_t *d);
int  mlx5_alloc_uar(bfm_dev_t *d, bfm_cmd_t *j, uint32_t *uarn);
int  mlx5_cq_create(bfm_dev_t *d, bfm_cmd_t *j, uint32_t uarn, volatile uint8_t *buf, uint32_t log_n,
                    volatile uint32_t *dbr, uint32_t *cqn);
int  mlx5_create_psv2(bfm_dev_t *d, bfm_cmd_t *j, uint32_t psv[2]);
int  mlx5_create_sig_mkey(bfm_dev_t *d, bfm_cmd_t *j, uint8_t key8, uint32_t *mkey);

/* mlx5_qp.c */
int  mlx5_qp_create(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp, uint8_t st, uint8_t cs_req);
int  mlx5_qp_rst2init(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp, int ud);
int  mlx5_qp_init2rtr_rc(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp, struct bfm_conn *c);
int  mlx5_qp_rtr2rts_rc(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp, struct bfm_conn *c, uint8_t amode);
int  mlx5_qp_ud_init2rtr(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp);
int  mlx5_qp_ud_rtr2rts(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp);
int  mlx5_qp_destroy(bfm_dev_t *d, bfm_cmd_t *j, bfm_qp_t *qp);
uint16_t mlx5_udp_sport(uint32_t lqpn, uint32_t rqpn);
void mlx5_gid_from_ipv4(uint32_t ip, uint8_t gid[16]);

/* bfm_cm.c */
void bfm_cm_reset(void);
int  bfm_cm_init_step(void);
int  bfm_cm_fini_step(void);
int  bfm_cm_poll(void);

/* bfm_ops.c */
int  bfm_ops_attach(struct bfm_conn *c);   /* 接続ができた後、データ経路の準備 */
void bfm_ops_detach(struct bfm_conn *c);
void bfm_conn_mark_error(uint32_t qpn, const char *why);
/* 性能の試験用(test/bfm_test.c の bench)。 */
int  bfm_ops_post_rw(struct bfm_conn *c, int is_read, uint64_t off, uint32_t stage_off,
                     const void *src, uint32_t len);
int  bfm_ops_poll1(struct bfm_conn *c);
/* bfm_dif.c から使う。 */
int  bfm_ops_check_range(const struct bfm_conn *c, uint64_t off, uint64_t len);
int  bfm_ops_post(struct bfm_conn *c, const uint64_t *w, uint32_t bbs);
int  bfm_ops_reap(struct bfm_conn *c);
void bfm_ops_ctrl(struct bfm_conn *c, uint64_t *w, uint32_t opcode, uint32_t ds, uint8_t fm_ce_se,
                  uint32_t imm);
void bfm_ops_build_rdma(struct bfm_conn *c, uint64_t *w, int is_read, uint64_t off, uint32_t lkey,
                        uint64_t addr, uint32_t len, uint8_t fence);

/* bfm_dif.c */
void bfm_dif_init_reset(void);
int  bfm_dif_init_step(void);
void bfm_dif_attach(struct bfm_conn *c);   /* 接続ができたとき(bfm_ops_attach() から)*/

#endif /* BFM_INTERNAL_H */
