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

#define BFM_PAGE 4096u

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
#define BFM_GSI_RX_SLOT  512u   /* 受信 1 枠(GRH 40 + MAD 256 が入る)*/
#define BFM_GSI_PREPOST  64u    /* GSI に投稿しておく受信の数 */
#define BFM_GSI_TXBUFS   16u    /* 送る MAD の置き場(順に使い回す)*/
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
#define MLX5_WQE_CTRL_CQ_UPDATE  0x08u
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
 * 装置
 * ------------------------------------------------------------------- */
typedef struct {
    /* PCIe */
    uint16_t vendor, device;
    const char *name;
    uint64_t bar0_phys, bar0_size;
    volatile uint8_t *bar0;          /* UC */

    /* コマンド */
    volatile uint8_t *cmdq;
    volatile uint8_t *mbox_in, *mbox_out;
    int hca_enabled;                 /* ENABLE_HCA が通り、まだ TEARDOWN していない */

    /* FW に渡すページ */
    uint8_t *fw_pages;
    uint32_t fw_pages_used;

    /* 能力 */
    uint8_t  log_max_ra_req_qp, log_max_ra_res_qp;
    uint8_t  num_vhca_ports;
    uint8_t  page_request_disable;
    uint8_t  bf_cap;
    uint32_t bf_buf_size;            /* BlueFlame の窓 1 面の大きさ(2 面を交互に使う)*/
    uint8_t  qp_ts_format;           /* QPC の ts_format に書く値 */
    uint16_t atomic_ops, atomic_size_qp;
    uint8_t  atomic_mode;            /* QPC の atomic_mode(0 = 相手からの atomic を受けない)*/
    uint8_t  atomic_host_order;      /* 1 = atomic の値をホストのバイト順で扱う設定になった */

    /* 資源 */
    uint32_t eq_uarn, eqn, eq_cc;
    uint64_t eq_mask;
    volatile uint8_t *eq_buf;
    uint32_t pdn;
    uint32_t lkey;                   /* 自分のメモリ全部(PA、ローカルの読み書き)*/
    uint32_t rkey;                   /* 公開メモリ(PA、[mr_base, mr_base + mr_size))*/

    /* ポート */
    uint8_t  mac[6];
    uint8_t  gid[16];
    uint32_t ip;
    int      link_up;
} bfm_dev_t;

extern bfm_dev_t g_bfm;

/* QP 1 本ぶんの資源(RC と GSI で共通)。 */
typedef struct {
    uint32_t qpn, cqn, uarn;
    uint32_t rq_log, sq_log, cq_log;
    volatile uint8_t *wq;            /* RQ(16B x 2^rq_log)の直後に SQ(64B x 2^sq_log)*/
    volatile uint8_t *sq;
    volatile uint32_t *dbr;          /* [0] = RQ のカウンタ、[1] = SQ のカウンタ(BE)*/
    volatile uint8_t *cq;
    volatile uint32_t *cq_dbr;       /* [0] = consumer counter(BE)*/
    uint32_t sq_pc, rq_pc, cq_cc;
    uint32_t local_psn;
    uint8_t  has_uar, has_cq, has_qp;   /* 番号 0 も有効な番号なので、作ったかは別に持つ */
} bfm_qp_t;

/* ---------------------------------------------------------------------
 * 接続
 * ------------------------------------------------------------------- */
enum { BFM_CS_FREE = 0, BFM_CS_SETUP, BFM_CS_UP, BFM_CS_ERROR, BFM_CS_CLOSED };

struct bfm_conn {
    int state;
    int index;
    bfm_qp_t qp;

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
    uint8_t  last_synd, last_vsynd;
    volatile uint8_t *stage;         /* 中継バッファ(DMA 領域)*/
    uint32_t stage_size;
    const volatile uint8_t *last_scat;  /* 直前の CQE が応答データを抱えていればその CQE */

    /* 相手 */
    uint32_t peer_ip;
    uint8_t  peer_mac[6];
    uint8_t  peer_gid[16];
    uint32_t peer_qpn, peer_psn;
    uint64_t raddr, rlen;
    uint32_t rkey;
    uint8_t  path_mtu;
    uint8_t  peer_rr;                /* 相手が受け付ける同時 read / atomic の数 */

    /* CM */
    uint32_t local_comm_id, remote_comm_id;
    uint64_t tid;
    uint16_t port;
};

extern struct bfm_conn g_bfm_conn[BFM_MAX_CONN];

/* ---------------------------------------------------------------------
 * 内部の関数
 * ------------------------------------------------------------------- */

/* bfm_mem.c: DMA 領域からの静的な切り出し */
void  bfm_mem_reset(void);
void *bfm_mem_alloc(uint64_t size, uint64_t align);
uint64_t bfm_mem_used(void);

/* bfm_pci.c */
int  bfm_pci_setup(bfm_dev_t *d);
void bfm_pci_bus_master(int on);

/* mlx5_cmd.c */
int  mlx5_cmd_init(bfm_dev_t *d);
int  mlx5_cmd(bfm_dev_t *d, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outlen,
              const char *what);
int  mlx5_give_pages(bfm_dev_t *d, uint16_t func, int32_t npages);
int  mlx5_eq_create(bfm_dev_t *d);
int  mlx5_eq_poll(bfm_dev_t *d);
int  mlx5_destroy_obj(bfm_dev_t *d, uint16_t opcode, uint32_t id, const char *what);

/* mlx5_hca.c */
int  mlx5_hca_init(bfm_dev_t *d);
void mlx5_hca_teardown(bfm_dev_t *d);
int  mlx5_alloc_uar(bfm_dev_t *d, uint32_t *uarn);
int  mlx5_cq_create(bfm_dev_t *d, uint32_t uarn, volatile uint8_t *buf, uint32_t log_n,
                    volatile uint32_t *dbr, uint32_t *cqn);

/* mlx5_qp.c */
int  mlx5_qp_create(bfm_dev_t *d, bfm_qp_t *qp, uint8_t st, uint8_t cs_req);
int  mlx5_qp_rst2init(bfm_dev_t *d, bfm_qp_t *qp, int ud);
int  mlx5_qp_init2rtr_rc(bfm_dev_t *d, bfm_qp_t *qp, struct bfm_conn *c);
int  mlx5_qp_rtr2rts_rc(bfm_dev_t *d, bfm_qp_t *qp, struct bfm_conn *c);
int  mlx5_qp_ud_to_rts(bfm_dev_t *d, bfm_qp_t *qp);
void mlx5_qp_destroy(bfm_dev_t *d, bfm_qp_t *qp);
uint16_t mlx5_udp_sport(uint32_t lqpn, uint32_t rqpn);
void mlx5_gid_from_ipv4(uint32_t ip, uint8_t gid[16]);

/* bfm_cm.c */
int  bfm_cm_init(void);
void bfm_cm_fini(void);
int  bfm_cm_poll(void);

/* bfm_ops.c */
int  bfm_ops_attach(struct bfm_conn *c);   /* 接続ができた後、データ経路の準備 */
void bfm_ops_detach(struct bfm_conn *c);
void bfm_conn_mark_error(uint32_t qpn, const char *why);
/* 性能の試験用(test/bfm_test.c の bench)。 */
int  bfm_ops_post_rw(struct bfm_conn *c, int is_read, uint64_t off, uint32_t stage_off,
                     const void *src, uint32_t len);
int  bfm_ops_poll1(struct bfm_conn *c);

#endif /* BFM_INTERNAL_H */
