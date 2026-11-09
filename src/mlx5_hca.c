/* HCA の立ち上げと、QP 以外の資源(UAR・PD・MKey・CQ)、ポート、RoCE のアドレス。
 *
 * 手順は Linux の mlx5_core(mlx5_function_enable() / mlx5_function_setup())と同じ順:
 *   FW の初期化待ち -> コマンドキュー -> ENABLE_HCA -> ISSI -> boot のページ ->
 *   HOST_ENDIANNESS -> HCA の能力(general / atomic / roce)-> init のページ -> INIT_HCA
 * その後に、EQ・PD・MKey(自分用 / 公開用)・ポート・vport の roce_en・GID を用意する。
 * 立ち上げは状態機械(mlx5_hca_init_step())で、待つところ(FW の初期化・コマンドの完了・
 * リンク・ページ要求)では待たずに BFM_INPROGRESS を返す。
 *
 * ConnectX-4〜7 の違いの吸収:
 *   - 能力は 4096 バイトまるごと読み書きする(新しい装置ほど構造が長い)。
 *   - BlueFlame の窓の大きさは log_bf_reg_size から求める。
 *   - QP の ts_format は RoCE の能力 qp_ts_format に合わせる(ConnectX-7 は時刻の形式を
 *     実時間だけにできるので、既定の 0 = free running だと CREATE_QP が断られうる)。
 *   - num_vhca_ports が 0 でなければ SET_ROCE_ADDRESS にポート番号を入れる。
 *   - FW のページの要求量は装置で大きく違うので、上限は g_bfm_fw_pages_max。 */
#include "bfm_internal.h"

#include <string.h>

#define FW_INIT_TIMEOUT_MS 120000u   /* ConnectX-7 は FW の起動に長くかかることがある */
#define CMDQ_SETTLE_MS     10u       /* コマンドキューを登録してから FW が受け付けるまで */
#define LINK_POLL_MS       10u       /* リンクの状態(PAOS)を読み直す間隔 */
#define PAGE_QUIET_MS      100u      /* INIT_HCA の後のページ要求が、これだけ来なければ止んだとみなす */
#define PAGE_SETTLE_MS     2000u     /* ページ要求に答える上限 */

static uint64_t ms_since(uint64_t t0)
{
    return (bfm_port_time_ns() - t0) / 1000000u;
}

/* FW が初期化中か(Initialization Segment の initializing ビット)。 */
static int fw_initializing(const bfm_dev_t *d)
{
    return (int)(bfm_mmio_rd32be(d->bar0, MLX5_ISEG_INITIALIZING) >> 31);
}

static int simple_cmd(bfm_dev_t *d, bfm_cmd_t *j, uint16_t opcode, uint16_t op_mod, const char *what)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], opcode);
    bfm_wr16be(&in[6], op_mod);
    return mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), what);
}

static int query_pages(bfm_dev_t *d, bfm_cmd_t *j, uint16_t op_mod, uint16_t *func, int32_t *n)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_PAGES);
    bfm_wr16be(&in[6], op_mod);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "QUERY_PAGES");
    if (rc != BFM_OK) return rc;
    *func = bfm_rd16be(&out[10]);
    *n = (int32_t)bfm_rd32be(&out[12]);
    return BFM_OK;
}

/* ACCESS_REG。op_mod 0 = 書く、1 = 読む。data は 16 バイト(完了した呼び出しで読んだ値を書き戻す)。 */
static int access_reg(bfm_dev_t *d, bfm_cmd_t *j, uint16_t reg, int read, uint8_t data[16], const char *what)
{
    uint8_t in[32] = {0}, out[32];
    bfm_wr16be(&in[0], MLX5_CMD_ACCESS_REG);
    bfm_wr16be(&in[6], read ? 1u : 0u);
    bfm_wr16be(&in[10], reg);
    memcpy(&in[16], data, 16);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), what);
    if (rc == BFM_OK) memcpy(data, &out[16], 16);
    return rc;
}

static uint8_t s_cap_out[16 + MLX5_HCA_CAP_BYTES];  /* QUERY_HCA_CAP の応答(16 バイトの頭 + 能力)*/
static uint8_t s_cap_in[16 + MLX5_HCA_CAP_BYTES];   /* SET_HCA_CAP の入力(s_cap_out の能力を直して写す)*/

static int query_cap(bfm_dev_t *d, bfm_cmd_t *j, uint16_t type, const char *what)
{
    uint8_t in[16] = {0};
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_HCA_CAP);
    bfm_wr16be(&in[6], (uint16_t)((type << 1) | 1u));   /* current */
    return mlx5_cmd_run(d, j, in, sizeof(in), s_cap_out, sizeof(s_cap_out), what);
}

/* s_cap_out の能力(書き換え済み)をそのまま SET_HCA_CAP で書く。 */
static int set_cap(bfm_dev_t *d, bfm_cmd_t *j, uint16_t type, const char *what)
{
    uint8_t out[16];
    if (!j->posted) {
        memset(s_cap_in, 0, 16);
        bfm_wr16be(&s_cap_in[0], MLX5_CMD_SET_HCA_CAP);
        bfm_wr16be(&s_cap_in[6], (uint16_t)(type << 1));
        memcpy(&s_cap_in[16], &s_cap_out[16], MLX5_HCA_CAP_BYTES);
    }
    return mlx5_cmd_run(d, j, s_cap_in, sizeof(s_cap_in), out, sizeof(out), what);
}

/*=================================================================
 * general の能力(s_cap_out)から要るものを取り出す(書き戻しは今の値のまま。
 * Linux も今の値を写してから一部を変えて書く。ここでは変えるものが無い)。
 * 位置は struct mlx5_ifc_cmd_hca_cap_bits のビット位置 / 8。
 * ===============================================================*/
static int parse_general_caps(bfm_dev_t *d)
{
    const uint8_t *c = &s_cap_out[16];
    d->page_request_disable = (uint8_t)((c[0] & 0x02u) ? 1u : 0u);   /* bit 0x6 */
    d->log_max_ra_req_qp = c[41] & 0x3fu;                             /* bit 0x14a */
    d->log_max_ra_res_qp = c[43] & 0x3fu;                             /* bit 0x15a */
    const uint8_t port_type = c[54] & 0x03u;                          /* bit 0x1b6(1 = Ethernet)*/
    const uint8_t roce = (uint8_t)((c[67] & 0x04u) ? 1u : 0u);        /* bit 0x21d */
    d->bf_cap = (uint8_t)((c[76] & 0x80u) ? 1u : 0u);                 /* bit 0x260 */
    uint8_t log_bf = c[77] & 0x1fu;                                   /* bit 0x26b log_bf_reg_size */
    d->num_vhca_ports = c[194];                                       /* bit 0x610 */
    d->sho = (uint8_t)((c[66] & 0x01u) ? 1u : 0u);                    /* bit 0x217 sho(署名のオフロード)*/
    {
        const uint8_t uf = (uint8_t)((c[65] >> 4) & 0x03u);           /* bit 0x20a umr_fence */
        /* Linux の mlx5_get_umr_fence(): 0 = STRONG、1 = SMALL、2 = 要らない */
        d->umr_fence = (uint8_t)(uf == 1u ? MLX5_FENCE_SMALL : uf == 2u ? 0u : MLX5_FENCE_STRONG);
    }
    if (port_type != 1u) {
        BFM_LOG("bfm: ポートが Ethernet ではない (port_type=%u)。RoCE は使えない\n", port_type);
        return BFM_E_HW;
    }
    if (!roce) BFM_LOG("bfm: 注意: RoCE の能力ビットが立っていない\n");
    if (log_bf < 8u || log_bf > 12u) {
        BFM_LOG("bfm: 注意: log_bf_reg_size=%u が想定外。512 バイトとして扱う\n", log_bf);
        log_bf = 9u;
    }
    d->bf_buf_size = (1u << log_bf) / 2u;   /* 窓は 2 面(rdma-core の mlx5 と同じ)*/
    if (d->bf_buf_size > BFM_WQE_MAX) d->bf_buf_size = BFM_WQE_MAX;
    BFM_DBG("bfm: log_max_ra_req/res=%u/%u bf=%u bf_buf=%u num_vhca_ports=%u sho=%u umr_fence=%u\n",
            d->log_max_ra_req_qp, d->log_max_ra_res_qp, d->bf_cap, d->bf_buf_size, d->num_vhca_ports,
            d->sho, (unsigned)((c[65] >> 4) & 0x03u));
    return BFM_OK;
}

/*=================================================================
 * atomic の能力(s_cap_out)を読む。8 バイトの atomic をホストのバイト順で扱う設定に
 * なっていなければ、s_cap_out を書き換えて 1 を返す(呼び出し側が SET_HCA_CAP で書く。
 * Linux の handle_hca_cap_atomic() と同じ。できない装置は断る)。
 *   byte 8  bit7-6 atomic_req_8B_endianness_mode(0 = BE / 1 = ホスト順)
 *   byte 8  bit1   supported_atomic_req_8B_endianness_mode_1
 *   byte 18-19     atomic_operations(bit0 CS / bit1 FA / bit2 拡張 CS / bit3 拡張 FA)
 *   byte 22-23     atomic_size_qp(bit n = 2^n バイト)
 * 戻り値: 0 = 書き戻し不要、1 = 書き戻す、負 = 断る。
 * ===============================================================*/
static int parse_atomic_caps(bfm_dev_t *d)
{
    uint8_t *c = &s_cap_out[16];
    d->atomic_ops = bfm_rd16be(&c[18]);
    d->atomic_size_qp = bfm_rd16be(&c[22]);
    if ((c[8] >> 6) == 1u) return 0;
    if (!((c[8] >> 1) & 1u)) {
        BFM_LOG("bfm: この装置は atomic をホストのバイト順にできない\n");
        return BFM_E_HW;
    }
    c[8] = (uint8_t)((c[8] & 0x3fu) | 0x40u);
    return 1;
}

/* QP の atomic_mode を決める(Linux の get_atomic_mode() と同じ規則)。拡張 atomic があれば
 * 対応する最大の大きさ(8B 未満なら 8B = 3)、なければ標準の CS / FA で IB_COMP(1)。 */
static void decide_atomic_mode(bfm_dev_t *d)
{
    int mode = 0;
    if (d->atomic_ops & 0x0cu) {
        const uint16_t m = d->atomic_size_qp & 0x1ffu;
        if (m) {
            int top = 15;
            while (top >= 0 && !(m & (1u << top))) top--;
            mode = (top > 3) ? top : 3;
        }
    }
    if (mode <= 0 && (d->atomic_ops & 0x03u) == 0x03u) mode = 1;
    d->atomic_mode = (uint8_t)mode;
    BFM_DBG("bfm: atomic ops=0x%x size_qp=0x%x atomic_mode=%u\n",
            d->atomic_ops, d->atomic_size_qp, d->atomic_mode);
    if ((d->atomic_ops & 0x03u) != 0x03u) BFM_LOG("bfm: 注意: 8 バイトの CAS / FAA に対応していない\n");
}

/* ---------------------------------------------------------------------
 * 資源(どれも FW のコマンド 1 つ。BFM_INPROGRESS の間は同じ j と引数で呼び直す)
 * ------------------------------------------------------------------- */

int mlx5_alloc_uar(bfm_dev_t *d, bfm_cmd_t *j, uint32_t *uarn)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_ALLOC_UAR);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "ALLOC_UAR");
    if (rc == BFM_OK) *uarn = bfm_rd24be(&out[9]);
    return rc;
}

static int alloc_pd(bfm_dev_t *d, bfm_cmd_t *j, uint32_t *pdn)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_ALLOC_PD);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "ALLOC_PD");
    if (rc == BFM_OK) *pdn = bfm_rd24be(&out[9]);
    return rc;
}

/*=================================================================
 * PA(物理アドレスをそのまま使う)モードの MKey を作る。
 *   access: mkc の byte 2(umr_en a rw rr lw lr | access_mode_1_0)
 *   len == 0 なら全アドレス(length64)。
 * 鍵の下位 8bit(mkey_7_0)は key8。
 * ===============================================================*/
static int create_mkey_pa(bfm_dev_t *d, bfm_cmd_t *j, uint8_t access, uint64_t start, uint64_t len, uint8_t key8,
                          uint32_t *mkey, const char *what)
{
    uint8_t in[272] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_MKEY);
    uint8_t *mkc = &in[16];
    mkc[2] = access;                               /* access_mode_1_0 = 0(PA)*/
    bfm_wr24be(&mkc[4], 0xffffffu);                /* qpn: 特定の QP に結びつけない */
    mkc[7] = key8;
    if (len == 0) mkc[12] = 0x80u;                 /* length64 */
    bfm_wr24be(&mkc[13], d->pdn);
    bfm_wr64be(&mkc[16], start);                   /* start_addr */
    bfm_wr64be(&mkc[24], len);                     /* len */
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), what);
    if (rc == BFM_OK) *mkey = (bfm_rd24be(&out[9]) << 8) | key8;
    return rc;
}

/*=================================================================
 * 署名(T10-DIF)のオフロードの資源(Linux の mlx5_alloc_integrity_descs() と同じ形)。
 * PSV(署名の途中の状態を持つもの)を 2 つ作る。[0] = メモリ側、[1] = ワイヤ側。
 *   create_psv_in : [8] num_psv << 4  [9..11] pd
 *   create_psv_out: [17..19] psv0_index  [21..23] psv1_index
 * ===============================================================*/
int mlx5_create_psv2(bfm_dev_t *d, bfm_cmd_t *j, uint32_t psv[2])
{
    uint8_t in[16] = {0}, out[32];
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_PSV);
    in[8] = (uint8_t)(2u << 4);
    bfm_wr24be(&in[9], d->pdn);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "CREATE_PSV");
    if (rc != BFM_OK) return rc;
    psv[0] = bfm_rd24be(&out[17]);
    psv[1] = bfm_rd24be(&out[21]);
    return BFM_OK;
}

/*=================================================================
 * 署名つきの MKey を作る。中身(KLM と BSF)は使うたびに UMR の WQE で入れるので、
 * 「空き」(free)・umr_en・KLM の形で作り、BSF 64 バイト(4 octword)と変換 4 octword の
 * 枠だけを持たせる。ローカルの読み書きだけを許す。
 *   mkc: [0] free(0x40)  [2] umr_en | lw | lr | access_mode_1_0 = KLM(2)  [4..6] qpn  [7] mkey_7_0
 *        [12] bsf_en(0x40)  [13..15] pd  [32..35] bsf_octword_size  [52..55] translations_octword_size
 * ===============================================================*/
int mlx5_create_sig_mkey(bfm_dev_t *d, bfm_cmd_t *j, uint8_t key8, uint32_t *mkey)
{
    uint8_t in[272] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_MKEY);
    uint8_t *mkc = &in[16];
    mkc[0] = 0x40u;
    mkc[2] = 0x8eu;
    bfm_wr24be(&mkc[4], 0xffffffu);
    mkc[7] = key8;
    mkc[12] = 0x40u;
    bfm_wr24be(&mkc[13], d->pdn);
    bfm_wr32be(&mkc[32], 4u);
    bfm_wr32be(&mkc[52], 4u);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "CREATE_MKEY(署名)");
    if (rc == BFM_OK) *mkey = (bfm_rd24be(&out[9]) << 8) | key8;
    return rc;
}

/*=================================================================
 * CQ を作る。CQE は 64 バイト、2^log_n 個。CQE は全部「まだ書かれていない」
 * (opcode = 0xf、owner = 0)で始める(出すときに埋める)。割り込みは使わない(arm しない)。
 * ===============================================================*/
int mlx5_cq_create(bfm_dev_t *d, bfm_cmd_t *j, uint32_t uarn, volatile uint8_t *buf, uint32_t log_n,
                   volatile uint32_t *dbr, uint32_t *cqn)
{
    if (log_n > 10u) return BFM_E_PARAM;           /* 1024 個 x 64 バイト = 16 ページまで(in[] の PAS の数)*/
    const uint32_t nent = 1u << log_n;
    const uint32_t npages = (nent * 64u + BFM_PAGE - 1u) / BFM_PAGE;
    if (!j->posted) {
        for (uint32_t i = 0; i < nent; i++) buf[i * 64u + 63u] = (uint8_t)(MLX5_CQE_INVALID << 4);
        dbr[0] = 0;
        dbr[1] = 0;
    }
    uint8_t in[272 + 16 * 8] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_CQ);
    uint8_t *cqc = &in[16];
    cqc[12] = (uint8_t)log_n;                      /* log_cq_size */
    bfm_wr24be(&cqc[13], uarn);                    /* uar_page */
    bfm_wr32be(&cqc[20], d->eqn);                  /* c_eqn */
    cqc[24] = 0;                                   /* log_page_size = 4KiB */
    bfm_wr64be(&cqc[56], bfm_dma_addr(dbr));       /* dbr_addr */
    for (uint32_t p = 0; p < npages; p++) bfm_wr64be(&in[272 + p * 8u], bfm_dma_addr(buf + p * BFM_PAGE));
    const int rc = mlx5_cmd_run(d, j, in, 272u + npages * 8u, out, sizeof(out), "CREATE_CQ");
    if (rc == BFM_OK) *cqn = bfm_rd24be(&out[9]);
    return rc;
}

/* ---------------------------------------------------------------------
 * ポートと RoCE
 * ------------------------------------------------------------------- */

/* PMTU の admin_mtu / oper_mtu はフレーム長(IP の MTU + Ethernet のヘッダ 14 + VLAN 4 + FCS 4)。
 * Linux の mlx5e の MLX5E_SW2HW_MTU() と同じ換算。 */
#define PORT_MTU_OVERHEAD 22u

/* フレーム長を IP の MTU に直す(オーバーヘッドより小さければ 0)。 */
static unsigned frame_to_ip_mtu(uint16_t frame)
{
    return (frame > PORT_MTU_OVERHEAD) ? (unsigned)(frame - PORT_MTU_OVERHEAD) : 0u;
}

/* vport の roce_en を立てる(立てないと INIT2RTR が断られる)。 */
static int vport_enable_roce(bfm_dev_t *d, bfm_cmd_t *j)
{
    static uint8_t in[516];
    uint8_t out[16];
    memset(in, 0, sizeof(in));
    bfm_wr16be(&in[0], MLX5_CMD_MODIFY_NIC_VPORT_CONTEXT);
    in[15] = 0x02u;                                /* field_select.roce_en */
    in[256 + 3] = 0x01u;                           /* nic_vport_context.roce_en */
    return mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "MODIFY_NIC_VPORT_CONTEXT(roce_en)");
}

/* vport の permanent_address(nic_vport_context のビット 0x7b0 から 6 バイト)。 */
static int vport_query_mac(bfm_dev_t *d, bfm_cmd_t *j, uint8_t mac[6])
{
    uint8_t in[16] = {0};
    static uint8_t out[272];
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_NIC_VPORT_CONTEXT);
    const int rc = mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "QUERY_NIC_VPORT_CONTEXT");
    if (rc == BFM_OK) memcpy(mac, &out[16 + 246], 6);
    return rc;
}

/* GID の表の 0 番に、自分の IPv4 から作った RoCE v2 の GID と MAC を入れる。 */
static int set_roce_address(bfm_dev_t *d, bfm_cmd_t *j)
{
    uint8_t in[48] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_SET_ROCE_ADDRESS);
    bfm_wr16be(&in[8], 0u);                        /* roce_address_index */
    if (d->num_vhca_ports) in[11] = 1u;            /* vhca_port_num */
    memcpy(&in[16], d->gid, 16);                   /* source_l3_address */
    memcpy(&in[34], d->mac, 6);                    /* source_mac */
    in[42] = 0u;                                   /* roce_l3_type = IPv4 */
    in[43] = 2u;                                   /* roce_version = v2 */
    return mlx5_cmd_run(d, j, in, sizeof(in), out, sizeof(out), "SET_ROCE_ADDRESS");
}

void mlx5_gid_from_ipv4(uint32_t ip, uint8_t gid[16])
{
    memset(gid, 0, 16);
    gid[10] = 0xff;
    gid[11] = 0xff;
    bfm_wr32be(&gid[12], ip);
}

/* ---------------------------------------------------------------------
 * 立ち上げと停止
 * ------------------------------------------------------------------- */

/* 立ち上げの状態(mlx5_hca_init_step() が 1 つずつ進める)。 */
enum {
    H_START = 0,      /* FW の版とコマンドの版を確かめる */
    H_FW_WAIT1,       /* FW の初期化を待つ(コマンドキューの登録の前)*/
    H_CMDQ,           /* コマンドキューを登録する */
    H_CMDQ_WAIT,      /* 登録から 10ms 待つ */
    H_FW_WAIT2,       /* FW の初期化を待つ(登録の後)*/
    H_ENABLE,         /* ENABLE_HCA */
    H_QUERY_ISSI,     /* QUERY_ISSI */
    H_SET_ISSI,       /* SET_ISSI(ISSI 1 に対応していれば)*/
    H_BOOT_QUERY,     /* QUERY_PAGES(boot)*/
    H_BOOT_GIVE,      /* MANAGE_PAGES(boot のページを渡す)*/
    H_HOST_ENDIAN,    /* ACCESS_REG(HOST_ENDIANNESS)*/
    H_GEN_QUERY,      /* QUERY_HCA_CAP(general)*/
    H_GEN_SET,        /* SET_HCA_CAP(general)*/
    H_ATOM_QUERY,     /* QUERY_HCA_CAP(atomic)*/
    H_ATOM_SET,       /* SET_HCA_CAP(atomic。ホスト順にするとき)*/
    H_ROCE_QUERY,     /* QUERY_HCA_CAP(roce)*/
    H_INIT_QUERY,     /* QUERY_PAGES(init)*/
    H_INIT_GIVE,      /* MANAGE_PAGES(init のページを渡す)*/
    H_INIT_HCA,       /* INIT_HCA */
    H_EQ_UAR,         /* EQ のドアベルの UAR(ALLOC_UAR)*/
    H_EQ_CREATE,      /* CREATE_EQ */
    H_PD,             /* ALLOC_PD */
    H_LKEY,           /* CREATE_MKEY(自分用。全アドレス)*/
    H_RKEY,           /* CREATE_MKEY(公開メモリ)*/
    H_MTU_READ,       /* ACCESS_REG(PMTU 読み。g_bfm_port_mtu が 0 なら飛ばす)*/
    H_MTU_WRITE,      /* ACCESS_REG(PMTU 書き)*/
    H_LINK_UP,        /* ACCESS_REG(PAOS 書き。admin_status = UP)*/
    H_LINK_READ,      /* ACCESS_REG(PAOS 読み。リンクが上がったか)*/
    H_LINK_PAUSE,     /* 読み直すまで 10ms 待つ */
    H_MTU_LOG,        /* ACCESS_REG(PMTU 読み。表示するため)*/
    H_ROCE_EN,        /* MODIFY_NIC_VPORT_CONTEXT(roce_en)*/
    H_MAC,            /* QUERY_NIC_VPORT_CONTEXT(g_bfm_local_mac が全部 0 のとき)*/
    H_ROCE_ADDR,      /* SET_ROCE_ADDRESS */
    H_SETTLE,         /* 追加のページ要求に答え、100ms 来なくなるまで待つ(上限 2 秒)*/
    H_DONE            /* 済んだ */
};

static struct {
    int step;                        /* 今の状態(H_*)*/
    bfm_cmd_t cmd;                   /* 出しているコマンド */
    bfm_pages_t pages;               /* boot / init のページを渡す手順 */
    uint64_t t0;                     /* 時間で待つ状態の起点 */
    uint64_t t1;                     /* リンク: 最後に読んだ時刻。ページ要求: 最後に来た時刻 */
    uint8_t reg[16];                 /* ACCESS_REG のデータ(完了した呼び出しで読んだ値が入る)*/
    uint32_t frame;                  /* ポートに設定するフレーム長 */
    int read_ok;                     /* PAOS が読めた */
} s_hi;

/* 停止の状態(mlx5_hca_teardown())。 */
static struct {
    uint16_t prof;                   /* 0 = graceful、1 = force */
    bfm_cmd_t cmd;                   /* 出している TEARDOWN_HCA */
} s_td;

/* 立ち上げと停止の状態を最初に戻す(bfm_init() が始めるときに呼ぶ)。 */
void mlx5_hca_init_reset(void)
{
    memset(&s_hi, 0, sizeof(s_hi));
    memset(&s_td, 0, sizeof(s_td));
    mlx5_eq_reset();
}

/*=================================================================
 * HCA の立ち上げを、待つところまで進める。待つところ(FW の初期化・コマンドの完了・
 * リンク・ページ要求)では BFM_INPROGRESS を返すので、BFM_OK か失敗になるまで呼び続ける。
 * 戻り値: BFM_INPROGRESS = 途中。BFM_OK = 済んだ。負 = 失敗(後始末は呼び出し側)。
 * ===============================================================*/
int mlx5_hca_init_step(bfm_dev_t *d)
{
    int rc;
    for (;;) {
        switch (s_hi.step) {
        case H_START: {
            const uint32_t fw = bfm_mmio_rd32be(d->bar0, MLX5_ISEG_FW_REV);
            const uint32_t cmdif = bfm_mmio_rd32be(d->bar0, MLX5_ISEG_CMDIF_REV);
            if (fw == 0xffffffffu) {
                BFM_LOG("bfm: BAR0 が全部 1 で読める(装置が見えない)\n");
                return BFM_E_HW;
            }
            BFM_LOG("bfm: FW %u.%u.%04u cmdif_rev=%u\n", fw & 0xffffu, fw >> 16, cmdif & 0xffffu, cmdif >> 16);
            if ((cmdif >> 16) != MLX5_CMD_IF_REV) {
                BFM_LOG("bfm: コマンドの版が %u(%u を想定)\n", cmdif >> 16, MLX5_CMD_IF_REV);
                return BFM_E_HW;
            }
            s_hi.t0 = bfm_port_time_ns();
            s_hi.step = H_FW_WAIT1;
            break;
        }
        case H_FW_WAIT1:
        case H_FW_WAIT2:
            if (fw_initializing(d)) {
                if (ms_since(s_hi.t0) <= FW_INIT_TIMEOUT_MS) return BFM_INPROGRESS;
                BFM_LOG("bfm: FW の初期化(%s)が %u ms で終わらない\n",
                        s_hi.step == H_FW_WAIT1 ? "pre-init" : "init", FW_INIT_TIMEOUT_MS);
                return BFM_E_TIMEOUT;
            }
            s_hi.step = (s_hi.step == H_FW_WAIT1) ? H_CMDQ : H_ENABLE;
            break;
        case H_CMDQ:
            if ((rc = mlx5_cmd_init(d)) != BFM_OK) return rc;
            s_hi.t0 = bfm_port_time_ns();
            s_hi.step = H_CMDQ_WAIT;
            break;
        case H_CMDQ_WAIT:
            if (ms_since(s_hi.t0) < CMDQ_SETTLE_MS) return BFM_INPROGRESS;
            s_hi.t0 = bfm_port_time_ns();
            s_hi.step = H_FW_WAIT2;
            break;
        case H_ENABLE:
            if ((rc = simple_cmd(d, &s_hi.cmd, MLX5_CMD_ENABLE_HCA, 0, "ENABLE_HCA")) != BFM_OK) return rc;
            d->hca_enabled = 1;
            s_hi.step = H_QUERY_ISSI;
            break;
        case H_QUERY_ISSI: {
            uint8_t in[16] = {0}, out[112];
            bfm_wr16be(&in[0], MLX5_CMD_QUERY_ISSI);
            if ((rc = mlx5_cmd_run(d, &s_hi.cmd, in, sizeof(in), out, sizeof(out), "QUERY_ISSI")) != BFM_OK) return rc;
            if (bfm_rd32be(&out[108]) & 0x2u) {
                s_hi.step = H_SET_ISSI;
            } else {
                BFM_LOG("bfm: ISSI 1 に対応していない。ISSI 0 のまま進む\n");
                s_hi.step = H_BOOT_QUERY;
            }
            break;
        }
        case H_SET_ISSI: {
            uint8_t in[16] = {0}, out[16];
            bfm_wr16be(&in[0], MLX5_CMD_SET_ISSI);
            bfm_wr16be(&in[10], 1u);
            if ((rc = mlx5_cmd_run(d, &s_hi.cmd, in, sizeof(in), out, sizeof(out), "SET_ISSI")) != BFM_OK) return rc;
            s_hi.step = H_BOOT_QUERY;
            break;
        }
        case H_BOOT_QUERY:
        case H_INIT_QUERY: {
            const int boot = (s_hi.step == H_BOOT_QUERY);
            if (boot && d->fw_pages == NULL) {
                d->fw_pages = (uint8_t *)bfm_mem_alloc((uint64_t)g_bfm_fw_pages_max * BFM_PAGE, BFM_PAGE);
                if (d->fw_pages == NULL) return BFM_E_NOMEM;
                d->fw_pages_used = 0;
            }
            uint16_t func = 0;
            int32_t n = 0;
            if ((rc = query_pages(d, &s_hi.cmd, boot ? 1u : 2u, &func, &n)) != BFM_OK) return rc;
            BFM_DBG("bfm: FW が %s のページを %d 求めた\n", boot ? "boot" : "init", (int)n);
            if ((rc = mlx5_pages_start(d, &s_hi.pages, func, n)) != BFM_OK) return rc;
            s_hi.step = boot ? H_BOOT_GIVE : H_INIT_GIVE;
            break;
        }
        case H_BOOT_GIVE:
        case H_INIT_GIVE:
            if ((rc = mlx5_pages_step(d, &s_hi.pages)) != BFM_OK) return rc;
            s_hi.step = (s_hi.step == H_BOOT_GIVE) ? H_HOST_ENDIAN : H_INIT_HCA;
            break;
        case H_HOST_ENDIAN:
            /* HOST_ENDIANNESS(Linux の set_hca_ctrl())。x86 はリトルエンディアンなので 0。 */
            if (!s_hi.cmd.posted) memset(s_hi.reg, 0, sizeof(s_hi.reg));
            if ((rc = access_reg(d, &s_hi.cmd, MLX5_REG_HOST_ENDIANNESS, 0, s_hi.reg,
                                 "ACCESS_REG(HOST_ENDIANNESS)")) != BFM_OK) return rc;
            s_hi.step = H_GEN_QUERY;
            break;
        case H_GEN_QUERY:
            if ((rc = query_cap(d, &s_hi.cmd, MLX5_CAP_GENERAL, "QUERY_HCA_CAP(general)")) != BFM_OK) return rc;
            if ((rc = parse_general_caps(d)) != BFM_OK) return rc;
            s_hi.step = H_GEN_SET;
            break;
        case H_GEN_SET:
            if ((rc = set_cap(d, &s_hi.cmd, MLX5_CAP_GENERAL, "SET_HCA_CAP(general)")) != BFM_OK) return rc;
            s_hi.step = H_ATOM_QUERY;
            break;
        case H_ATOM_QUERY:
            rc = query_cap(d, &s_hi.cmd, MLX5_CAP_ATOMIC, "QUERY_HCA_CAP(atomic)");
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) return BFM_E_HW;
            if ((rc = parse_atomic_caps(d)) < 0) return rc;
            if (rc == 0) decide_atomic_mode(d);
            s_hi.step = (rc == 1) ? H_ATOM_SET : H_ROCE_QUERY;
            break;
        case H_ATOM_SET:
            rc = set_cap(d, &s_hi.cmd, MLX5_CAP_ATOMIC, "SET_HCA_CAP(atomic)");
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) return BFM_E_HW;
            decide_atomic_mode(d);
            s_hi.step = H_ROCE_QUERY;
            break;
        case H_ROCE_QUERY:
            /* RoCE の能力の qp_ts_format(byte 3 の下位 2bit)が 0 でなければ、QP は ts_format = 1(DEFAULT)。
             * 読めなければ 0 のまま進む。 */
            rc = query_cap(d, &s_hi.cmd, MLX5_CAP_ROCE, "QUERY_HCA_CAP(roce)");
            if (rc == BFM_INPROGRESS) return rc;
            d->qp_ts_format = (rc == BFM_OK && (s_cap_out[16 + 3] & 0x03u)) ? 1u : 0u;
            s_hi.step = H_INIT_QUERY;
            break;
        case H_INIT_HCA: {
            uint8_t in[32] = {0}, out[16];
            bfm_wr16be(&in[0], MLX5_CMD_INIT_HCA);
            if ((rc = mlx5_cmd_run(d, &s_hi.cmd, in, sizeof(in), out, sizeof(out), "INIT_HCA")) != BFM_OK) return rc;
            if ((rc = mlx5_eq_prepare(d)) != BFM_OK) return rc;
            s_hi.step = H_EQ_UAR;
            break;
        }
        case H_EQ_UAR:
            if ((rc = mlx5_alloc_uar(d, &s_hi.cmd, &d->eq_uarn)) != BFM_OK) return rc;
            s_hi.step = H_EQ_CREATE;
            break;
        case H_EQ_CREATE:
            if ((rc = mlx5_eq_create(d, &s_hi.cmd)) != BFM_OK) return rc;
            s_hi.step = H_PD;
            break;
        case H_PD:
            if ((rc = alloc_pd(d, &s_hi.cmd, &d->pdn)) != BFM_OK) return rc;
            s_hi.step = H_LKEY;
            break;
        case H_LKEY:
            if ((rc = create_mkey_pa(d, &s_hi.cmd, 0x0cu, 0, 0, 0, &d->lkey, "CREATE_MKEY(local)")) != BFM_OK)
                return rc;
            d->rkey_key8 = (uint8_t)(bfm_port_time_ns() >> 10);
            s_hi.step = H_RKEY;
            break;
        case H_RKEY:
            if ((rc = create_mkey_pa(d, &s_hi.cmd, 0x7cu, g_bfm_mr_base, g_bfm_mr_size, d->rkey_key8, &d->rkey,
                                     "CREATE_MKEY(公開メモリ)")) != BFM_OK) return rc;
            s_hi.step = g_bfm_port_mtu ? H_MTU_READ : H_LINK_UP;
            break;
        case H_MTU_READ: {
            /* ポートの MTU を IP の MTU(Linux の ip link set mtu と同じ意味)で設定する。上限を先に読む。 */
            if (!s_hi.cmd.posted) {
                memset(s_hi.reg, 0, sizeof(s_hi.reg));
                s_hi.reg[1] = 1u;                              /* local_port */
            }
            rc = access_reg(d, &s_hi.cmd, MLX5_REG_PMTU, 1, s_hi.reg, "ACCESS_REG(PMTU 読み)");
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) return BFM_E_HW;
            uint32_t max = bfm_rd16be(&s_hi.reg[4]);
            if (max == 0) max = 0xffffu;
            s_hi.frame = (uint32_t)g_bfm_port_mtu + PORT_MTU_OVERHEAD;
            if (s_hi.frame > max) {
                BFM_LOG("bfm: g_bfm_port_mtu=%u がポートの上限(フレーム長 %u)を超えるので切り詰める\n",
                        g_bfm_port_mtu, max);
                s_hi.frame = max;
            }
            s_hi.step = H_MTU_WRITE;
            break;
        }
        case H_MTU_WRITE:
            if (!s_hi.cmd.posted) {
                memset(s_hi.reg, 0, sizeof(s_hi.reg));
                s_hi.reg[1] = 1u;
                bfm_wr16be(&s_hi.reg[8], (uint16_t)s_hi.frame);  /* admin_mtu */
            }
            if ((rc = access_reg(d, &s_hi.cmd, MLX5_REG_PMTU, 0, s_hi.reg, "ACCESS_REG(PMTU 書き)")) != BFM_OK)
                return rc;
            s_hi.step = H_LINK_UP;
            break;
        case H_LINK_UP:
            if (!s_hi.cmd.posted) {
                memset(s_hi.reg, 0, sizeof(s_hi.reg));
                s_hi.reg[1] = 1u;                              /* local_port */
                s_hi.reg[2] = 1u;                              /* admin_status = UP */
                s_hi.reg[4] = 0x80u;                           /* ase */
            }
            rc = access_reg(d, &s_hi.cmd, MLX5_REG_PAOS, 0, s_hi.reg, "ACCESS_REG(PAOS 書き)");
            if (rc == BFM_INPROGRESS) return rc;
            if (rc != BFM_OK) return BFM_E_HW;
            s_hi.t0 = bfm_port_time_ns();
            s_hi.read_ok = 0;
            s_hi.step = H_LINK_READ;
            break;
        case H_LINK_READ:
            if (!s_hi.cmd.posted) {
                memset(s_hi.reg, 0, sizeof(s_hi.reg));
                s_hi.reg[1] = 1u;
            }
            rc = access_reg(d, &s_hi.cmd, MLX5_REG_PAOS, 1, s_hi.reg, "ACCESS_REG(PAOS 読み)");
            if (rc == BFM_INPROGRESS) return rc;
            s_hi.read_ok = (rc == BFM_OK);
            if (s_hi.read_ok && (s_hi.reg[3] & 0x0fu) == 1u) {
                d->link_up = 1;
                s_hi.step = H_MTU_LOG;
            } else if (ms_since(s_hi.t0) > g_bfm_link_wait_ms) {
                if (!s_hi.read_ok) {
                    BFM_LOG("bfm: ポートの状態(PAOS)が読めない\n");
                    return BFM_E_HW;
                }
                BFM_LOG("bfm: 注意: リンクが %u ms で上がらない(このまま進む。上がれば EQ で分かる)\n",
                        g_bfm_link_wait_ms);
                s_hi.step = H_MTU_LOG;
            } else {
                s_hi.t1 = bfm_port_time_ns();
                s_hi.step = H_LINK_PAUSE;
            }
            break;
        case H_LINK_PAUSE:
            if (ms_since(s_hi.t1) < LINK_POLL_MS) return BFM_INPROGRESS;
            s_hi.step = H_LINK_READ;
            break;
        case H_MTU_LOG:
            /* 今のポートの MTU(IP の MTU に直したもの)を表示する。読めなければ表示しない。 */
            if (!s_hi.cmd.posted) {
                memset(s_hi.reg, 0, sizeof(s_hi.reg));
                s_hi.reg[1] = 1u;
            }
            rc = access_reg(d, &s_hi.cmd, MLX5_REG_PMTU, 1, s_hi.reg, "ACCESS_REG(PMTU 読み)");
            if (rc == BFM_INPROGRESS) return rc;
            if (rc == BFM_OK)
                BFM_LOG("bfm: ポートの MTU: 設定 %u / 実際 %u(IP の MTU。上限 %u)\n",
                        frame_to_ip_mtu(bfm_rd16be(&s_hi.reg[8])), frame_to_ip_mtu(bfm_rd16be(&s_hi.reg[12])),
                        frame_to_ip_mtu(bfm_rd16be(&s_hi.reg[4])));
            s_hi.step = H_ROCE_EN;
            break;
        case H_ROCE_EN: {
            static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
            if ((rc = vport_enable_roce(d, &s_hi.cmd)) != BFM_OK) return rc;
            if (memcmp(g_bfm_local_mac, zero, 6) != 0) {
                memcpy(d->mac, g_bfm_local_mac, 6);
                s_hi.step = H_ROCE_ADDR;
            } else {
                s_hi.step = H_MAC;
            }
            d->ip = g_bfm_local_ip;
            mlx5_gid_from_ipv4(d->ip, d->gid);
            break;
        }
        case H_MAC:
            if ((rc = vport_query_mac(d, &s_hi.cmd, d->mac)) != BFM_OK) return rc;
            s_hi.step = H_ROCE_ADDR;
            break;
        case H_ROCE_ADDR:
            if ((rc = set_roce_address(d, &s_hi.cmd)) != BFM_OK) return rc;
            s_hi.t0 = s_hi.t1 = bfm_port_time_ns();
            s_hi.step = H_SETTLE;
            break;
        case H_SETTLE:
            /* INIT_HCA の後に FW がページを追加で求めてくる。100ms 来なくなるまで答える(上限 2 秒)。 */
            if (mlx5_eq_poll(d) > 0 || mlx5_eq_busy()) s_hi.t1 = bfm_port_time_ns();
            if (ms_since(s_hi.t0) < PAGE_SETTLE_MS && ms_since(s_hi.t1) <= PAGE_QUIET_MS) return BFM_INPROGRESS;
            BFM_LOG("bfm: HCA 準備完了: MAC %02x:%02x:%02x:%02x:%02x:%02x IP %u.%u.%u.%u リンク %s"
                    " FW ページ %u 公開メモリ 0x%llx+0x%llx rkey 0x%08x\n",
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                    d->ip >> 24, (d->ip >> 16) & 0xffu, (d->ip >> 8) & 0xffu, d->ip & 0xffu,
                    d->link_up ? "UP" : "DOWN", d->fw_pages_used,
                    (unsigned long long)g_bfm_mr_base, (unsigned long long)g_bfm_mr_size, d->rkey);
            s_hi.step = H_DONE;
            break;
        default:   /* H_DONE */
            return BFM_OK;
        }
    }
}

/*=================================================================
 * FW を止める(TEARDOWN_HCA。graceful が断られたら force)。待たずに進める。この後は FW に
 * コマンドを出さない(資源を壊さずに止めた後の MANAGE_PAGES・DISABLE_HCA は
 * 返らないことがある。vfio_nvme で実機確認済み)。呼び出し側が Bus Master を落とす。
 * 戻り値: BFM_INPROGRESS = まだ。BFM_OK = 済んだ(通らなかったときも表示して BFM_OK)。
 * ===============================================================*/
int mlx5_hca_teardown(bfm_dev_t *d)
{
    if (!d->hca_enabled) return BFM_OK;
    for (;;) {
        uint8_t in[16] = {0}, out[16];
        bfm_wr16be(&in[0], MLX5_CMD_TEARDOWN_HCA);
        bfm_wr16be(&in[10], s_td.prof);
        const int rc = mlx5_cmd_run(d, &s_td.cmd, in, sizeof(in), out, sizeof(out),
                                    s_td.prof ? "TEARDOWN_HCA(force)" : "TEARDOWN_HCA(graceful)");
        if (rc == BFM_INPROGRESS) return rc;
        if (rc == BFM_OK && !(s_td.prof == 1u && (out[15] & 1u))) break;
        if (s_td.prof == 0u) {
            s_td.prof = 1u;
            continue;
        }
        BFM_LOG("bfm: 注意: TEARDOWN_HCA が graceful / force とも通らなかった(次の立ち上げの FLR で戻す)\n");
        break;
    }
    s_td.prof = 0u;
    d->hca_enabled = 0;
    return BFM_OK;
}
