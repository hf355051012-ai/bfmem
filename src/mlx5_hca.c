/* HCA の立ち上げと、QP 以外の資源(UAR・PD・MKey・CQ)、ポート、RoCE のアドレス。
 *
 * 手順は Linux の mlx5_core(mlx5_function_enable() / mlx5_function_setup())と同じ順:
 *   FW の初期化待ち -> コマンドキュー -> ENABLE_HCA -> ISSI -> boot のページ ->
 *   HOST_ENDIANNESS -> HCA の能力(general / atomic / roce)-> init のページ -> INIT_HCA
 * その後に、EQ・PD・MKey(自分用 / 公開用)・ポート・vport の roce_en・GID を用意する。
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

static int wait_fw_init(bfm_dev_t *d, const char *phase)
{
    const uint64_t t0 = bfm_port_time_ns();
    while (bfm_mmio_rd32be(d->bar0, MLX5_ISEG_INITIALIZING) >> 31) {
        if (bfm_port_time_ns() - t0 > (uint64_t)FW_INIT_TIMEOUT_MS * 1000000u) {
            BFM_LOG("bfm: FW の初期化(%s)が %u ms で終わらない\n", phase, FW_INIT_TIMEOUT_MS);
            return BFM_E_TIMEOUT;
        }
        bfm_port_delay_us(1000u);
    }
    return BFM_OK;
}

static int simple_cmd(bfm_dev_t *d, uint16_t opcode, uint16_t op_mod, const char *what)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], opcode);
    bfm_wr16be(&in[6], op_mod);
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), what);
}

static int query_pages(bfm_dev_t *d, uint16_t op_mod, uint16_t *func, int32_t *n)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_PAGES);
    bfm_wr16be(&in[6], op_mod);
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "QUERY_PAGES");
    if (rc != BFM_OK) return rc;
    *func = bfm_rd16be(&out[10]);
    *n = (int32_t)bfm_rd32be(&out[12]);
    return BFM_OK;
}

static int satisfy_pages(bfm_dev_t *d, int boot)
{
    uint16_t func = 0;
    int32_t n = 0;
    int rc = query_pages(d, boot ? 1u : 2u, &func, &n);
    if (rc != BFM_OK) return rc;
    BFM_DBG("bfm: FW が %s のページを %d 求めた\n", boot ? "boot" : "init", (int)n);
    return mlx5_give_pages(d, func, n);
}

static int set_issi(bfm_dev_t *d)
{
    uint8_t in[16] = {0}, out[112];
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_ISSI);
    int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "QUERY_ISSI");
    if (rc != BFM_OK) return rc;
    if (!(bfm_rd32be(&out[108]) & 0x2u)) {
        BFM_LOG("bfm: ISSI 1 に対応していない。ISSI 0 のまま進む\n");
        return BFM_OK;
    }
    memset(in, 0, sizeof(in));
    bfm_wr16be(&in[0], MLX5_CMD_SET_ISSI);
    bfm_wr16be(&in[10], 1u);
    return mlx5_cmd(d, in, sizeof(in), out, 16u, "SET_ISSI");
}

/* ACCESS_REG。op_mod 0 = 書く、1 = 読む。data は 16 バイト。 */
static int access_reg(bfm_dev_t *d, uint16_t reg, int read, uint8_t data[16], const char *what)
{
    uint8_t in[32] = {0}, out[32];
    bfm_wr16be(&in[0], MLX5_CMD_ACCESS_REG);
    bfm_wr16be(&in[6], read ? 1u : 0u);
    bfm_wr16be(&in[10], reg);
    memcpy(&in[16], data, 16);
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), what);
    if (rc == BFM_OK) memcpy(data, &out[16], 16);
    return rc;
}

/* HOST_ENDIANNESS(Linux の set_hca_ctrl())。x86 はリトルエンディアンなので 0。 */
static int set_host_endianness(bfm_dev_t *d)
{
    uint8_t data[16] = {0};
    return access_reg(d, MLX5_REG_HOST_ENDIANNESS, 0, data, "ACCESS_REG(HOST_ENDIANNESS)");
}

static uint8_t s_cap_out[16 + MLX5_HCA_CAP_BYTES];
static uint8_t s_cap_in[16 + MLX5_HCA_CAP_BYTES];

static int query_cap(bfm_dev_t *d, uint16_t type, const char *what)
{
    uint8_t in[16] = {0};
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_HCA_CAP);
    bfm_wr16be(&in[6], (uint16_t)((type << 1) | 1u));   /* current */
    return mlx5_cmd(d, in, sizeof(in), s_cap_out, sizeof(s_cap_out), what);
}

/* s_cap_out の能力(書き換え済み)をそのまま SET_HCA_CAP で書く。 */
static int set_cap(bfm_dev_t *d, uint16_t type, const char *what)
{
    uint8_t out[16];
    memset(s_cap_in, 0, 16);
    bfm_wr16be(&s_cap_in[0], MLX5_CMD_SET_HCA_CAP);
    bfm_wr16be(&s_cap_in[6], (uint16_t)(type << 1));
    memcpy(&s_cap_in[16], &s_cap_out[16], MLX5_HCA_CAP_BYTES);
    return mlx5_cmd(d, s_cap_in, sizeof(s_cap_in), out, sizeof(out), what);
}

/*=================================================================
 * general の能力を読んで要るものを取り出し、今の値のまま書き戻す
 * (Linux も今の値を写してから一部を変えて書く。ここでは変えるものが無い)。
 * 位置は struct mlx5_ifc_cmd_hca_cap_bits のビット位置 / 8。
 * ===============================================================*/
static int setup_general_caps(bfm_dev_t *d)
{
    int rc = query_cap(d, MLX5_CAP_GENERAL, "QUERY_HCA_CAP(general)");
    if (rc != BFM_OK) return rc;
    const uint8_t *c = &s_cap_out[16];
    d->page_request_disable = (uint8_t)((c[0] & 0x02u) ? 1u : 0u);   /* bit 0x6 */
    d->log_max_ra_req_qp = c[41] & 0x3fu;                             /* bit 0x14a */
    d->log_max_ra_res_qp = c[43] & 0x3fu;                             /* bit 0x15a */
    const uint8_t port_type = c[54] & 0x03u;                          /* bit 0x1b6(1 = Ethernet)*/
    const uint8_t roce = (uint8_t)((c[67] & 0x04u) ? 1u : 0u);        /* bit 0x21d */
    d->bf_cap = (uint8_t)((c[76] & 0x80u) ? 1u : 0u);                 /* bit 0x260 */
    uint8_t log_bf = c[77] & 0x1fu;                                   /* bit 0x26b log_bf_reg_size */
    d->num_vhca_ports = c[194];                                       /* bit 0x610 */
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
    BFM_DBG("bfm: log_max_ra_req/res=%u/%u bf=%u bf_buf=%u num_vhca_ports=%u\n",
            d->log_max_ra_req_qp, d->log_max_ra_res_qp, d->bf_cap, d->bf_buf_size, d->num_vhca_ports);
    return set_cap(d, MLX5_CAP_GENERAL, "SET_HCA_CAP(general)");
}

/*=================================================================
 * atomic の能力。8 バイトの atomic をホストのバイト順で扱う設定にする(Linux の
 * handle_hca_cap_atomic() と同じ。できない装置は断る)。QP の atomic_mode もここで決める
 * (Linux の get_atomic_mode() と同じ規則)。
 *   byte 8  bit7-6 atomic_req_8B_endianness_mode(0 = BE / 1 = ホスト順)
 *   byte 8  bit1   supported_atomic_req_8B_endianness_mode_1
 *   byte 18-19     atomic_operations(bit0 CS / bit1 FA / bit2 拡張 CS / bit3 拡張 FA)
 *   byte 22-23     atomic_size_qp(bit n = 2^n バイト)
 * ===============================================================*/
static int setup_atomic_caps(bfm_dev_t *d)
{
    if (query_cap(d, MLX5_CAP_ATOMIC, "QUERY_HCA_CAP(atomic)") != BFM_OK) return BFM_E_HW;
    uint8_t *c = &s_cap_out[16];
    d->atomic_ops = bfm_rd16be(&c[18]);
    d->atomic_size_qp = bfm_rd16be(&c[22]);
    if ((c[8] >> 6) != 1u) {
        if (!((c[8] >> 1) & 1u)) {
            BFM_LOG("bfm: この装置は atomic をホストのバイト順にできない\n");
            return BFM_E_HW;
        }
        c[8] = (uint8_t)((c[8] & 0x3fu) | 0x40u);
        if (set_cap(d, MLX5_CAP_ATOMIC, "SET_HCA_CAP(atomic)") != BFM_OK) return BFM_E_HW;
    }

    /* atomic_mode: 拡張 atomic があれば対応する最大の大きさ(8B 未満なら 8B = 3)、
     * なければ標準の CS / FA で IB_COMP(1)。 */
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
    return BFM_OK;
}

/* RoCE の能力の qp_ts_format(byte 3 の下位 2bit)が 0 でなければ、QP は ts_format = 1(DEFAULT)。 */
static void setup_roce_caps(bfm_dev_t *d)
{
    d->qp_ts_format = 0;
    if (query_cap(d, MLX5_CAP_ROCE, "QUERY_HCA_CAP(roce)") != BFM_OK) return;
    if (s_cap_out[16 + 3] & 0x03u) d->qp_ts_format = 1u;
}

/* ---------------------------------------------------------------------
 * 資源
 * ------------------------------------------------------------------- */

int mlx5_alloc_uar(bfm_dev_t *d, uint32_t *uarn)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_ALLOC_UAR);
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "ALLOC_UAR");
    if (rc == BFM_OK) *uarn = bfm_rd24be(&out[9]);
    return rc;
}

static int alloc_pd(bfm_dev_t *d, uint32_t *pdn)
{
    uint8_t in[16] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_ALLOC_PD);
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "ALLOC_PD");
    if (rc == BFM_OK) *pdn = bfm_rd24be(&out[9]);
    return rc;
}

/*=================================================================
 * PA(物理アドレスをそのまま使う)モードの MKey を作る。
 *   access: mkc の byte 2(umr_en a rw rr lw lr | access_mode_1_0)
 *   len == 0 なら全アドレス(length64)。
 * 鍵の下位 8bit(mkey_7_0)は key8。
 * ===============================================================*/
static int create_mkey_pa(bfm_dev_t *d, uint8_t access, uint64_t start, uint64_t len, uint8_t key8,
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
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), what);
    if (rc == BFM_OK) *mkey = (bfm_rd24be(&out[9]) << 8) | key8;
    return rc;
}

/*=================================================================
 * CQ を作る。CQE は 64 バイト、2^log_n 個。CQE は全部「まだ書かれていない」
 * (opcode = 0xf、owner = 0)で始める。割り込みは使わない(arm しない)。
 * ===============================================================*/
int mlx5_cq_create(bfm_dev_t *d, uint32_t uarn, volatile uint8_t *buf, uint32_t log_n,
                   volatile uint32_t *dbr, uint32_t *cqn)
{
    if (log_n > 10u) return BFM_E_PARAM;           /* 1024 個 x 64 バイト = 16 ページまで(in[] の PAS の数)*/
    const uint32_t nent = 1u << log_n;
    const uint32_t npages = (nent * 64u + BFM_PAGE - 1u) / BFM_PAGE;
    for (uint32_t i = 0; i < nent; i++) buf[i * 64u + 63u] = (uint8_t)(MLX5_CQE_INVALID << 4);
    dbr[0] = 0;
    dbr[1] = 0;

    uint8_t in[272 + 16 * 8] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_CREATE_CQ);
    uint8_t *cqc = &in[16];
    cqc[12] = (uint8_t)log_n;                      /* log_cq_size */
    bfm_wr24be(&cqc[13], uarn);                    /* uar_page */
    bfm_wr32be(&cqc[20], d->eqn);                  /* c_eqn */
    cqc[24] = 0;                                   /* log_page_size = 4KiB */
    bfm_wr64be(&cqc[56], bfm_dma_addr(dbr));       /* dbr_addr */
    for (uint32_t p = 0; p < npages; p++) bfm_wr64be(&in[272 + p * 8u], bfm_dma_addr(buf + p * BFM_PAGE));
    const int rc = mlx5_cmd(d, in, 272u + npages * 8u, out, sizeof(out), "CREATE_CQ");
    if (rc == BFM_OK) *cqn = bfm_rd24be(&out[9]);
    return rc;
}

/* ---------------------------------------------------------------------
 * ポートと RoCE
 * ------------------------------------------------------------------- */

/* PMTU の admin_mtu / oper_mtu はフレーム長(IP の MTU + Ethernet のヘッダ 14 + VLAN 4 + FCS 4)。
 * Linux の mlx5e の MLX5E_SW2HW_MTU() と同じ換算。 */
#define PORT_MTU_OVERHEAD 22u

/* ポートの MTU を IP の MTU(Linux の ip link set mtu と同じ意味)で設定する。 */
static int port_set_mtu(bfm_dev_t *d, uint16_t ip_mtu)
{
    uint8_t r[16] = {0};
    uint32_t frame = (uint32_t)ip_mtu + PORT_MTU_OVERHEAD;
    r[1] = 1u;                                     /* local_port */
    if (access_reg(d, MLX5_REG_PMTU, 1, r, "ACCESS_REG(PMTU 読み)") != BFM_OK) return BFM_E_HW;
    uint32_t max = bfm_rd16be(&r[4]);
    if (max == 0) max = 0xffffu;
    if (frame > max) {
        BFM_LOG("bfm: g_bfm_port_mtu=%u がポートの上限(フレーム長 %u)を超えるので切り詰める\n", ip_mtu, max);
        frame = max;
    }
    memset(r, 0, sizeof(r));
    r[1] = 1u;
    bfm_wr16be(&r[8], (uint16_t)frame);            /* admin_mtu */
    return access_reg(d, MLX5_REG_PMTU, 0, r, "ACCESS_REG(PMTU 書き)");
}

/* フレーム長を IP の MTU に直す(オーバーヘッドより小さければ 0)。 */
static unsigned frame_to_ip_mtu(uint16_t frame)
{
    return (frame > PORT_MTU_OVERHEAD) ? (unsigned)(frame - PORT_MTU_OVERHEAD) : 0u;
}

/* 今のポートの MTU(IP の MTU に直したもの)を表示する。 */
static void port_log_mtu(bfm_dev_t *d)
{
    uint8_t r[16] = {0};
    r[1] = 1u;
    if (access_reg(d, MLX5_REG_PMTU, 1, r, "ACCESS_REG(PMTU 読み)") != BFM_OK) return;
    BFM_LOG("bfm: ポートの MTU: 設定 %u / 実際 %u(IP の MTU。上限 %u)\n",
            frame_to_ip_mtu(bfm_rd16be(&r[8])), frame_to_ip_mtu(bfm_rd16be(&r[12])),
            frame_to_ip_mtu(bfm_rd16be(&r[4])));
}

static int port_up(bfm_dev_t *d)
{
    uint8_t r[16] = {0};
    r[1] = 1u;                                     /* local_port */
    r[2] = 1u;                                     /* admin_status = UP */
    r[4] = 0x80u;                                  /* ase */
    if (access_reg(d, MLX5_REG_PAOS, 0, r, "ACCESS_REG(PAOS 書き)") != BFM_OK) return BFM_E_HW;
    const uint64_t t0 = bfm_port_time_ns();
    int read_ok = 0;
    for (;;) {
        memset(r, 0, sizeof(r));
        r[1] = 1u;
        read_ok = (access_reg(d, MLX5_REG_PAOS, 1, r, "ACCESS_REG(PAOS 読み)") == BFM_OK);
        if (read_ok && (r[3] & 0x0fu) == 1u) {
            d->link_up = 1;
            return BFM_OK;
        }
        if (bfm_port_time_ns() - t0 > (uint64_t)g_bfm_link_wait_ms * 1000000u) break;
        bfm_port_delay_us(10000u);
    }
    if (!read_ok) {
        BFM_LOG("bfm: ポートの状態(PAOS)が読めない\n");
        return BFM_E_HW;
    }
    BFM_LOG("bfm: 注意: リンクが %u ms で上がらない(このまま進む。上がれば EQ で分かる)\n",
            g_bfm_link_wait_ms);
    return BFM_OK;
}

/* vport の roce_en を立てる(立てないと INIT2RTR が断られる)。 */
static int vport_enable_roce(bfm_dev_t *d)
{
    static uint8_t in[516];
    uint8_t out[16];
    memset(in, 0, sizeof(in));
    bfm_wr16be(&in[0], MLX5_CMD_MODIFY_NIC_VPORT_CONTEXT);
    in[15] = 0x02u;                                /* field_select.roce_en */
    in[256 + 3] = 0x01u;                           /* nic_vport_context.roce_en */
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "MODIFY_NIC_VPORT_CONTEXT(roce_en)");
}

/* vport の permanent_address(nic_vport_context のビット 0x7b0 から 6 バイト)。 */
static int vport_query_mac(bfm_dev_t *d, uint8_t mac[6])
{
    uint8_t in[16] = {0};
    static uint8_t out[272];
    bfm_wr16be(&in[0], MLX5_CMD_QUERY_NIC_VPORT_CONTEXT);
    const int rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "QUERY_NIC_VPORT_CONTEXT");
    if (rc == BFM_OK) memcpy(mac, &out[16 + 246], 6);
    return rc;
}

/* GID の表の 0 番に、自分の IPv4 から作った RoCE v2 の GID と MAC を入れる。 */
static int set_roce_address(bfm_dev_t *d)
{
    uint8_t in[48] = {0}, out[16];
    bfm_wr16be(&in[0], MLX5_CMD_SET_ROCE_ADDRESS);
    bfm_wr16be(&in[8], 0u);                        /* roce_address_index */
    if (d->num_vhca_ports) in[11] = 1u;            /* vhca_port_num */
    memcpy(&in[16], d->gid, 16);                   /* source_l3_address */
    memcpy(&in[34], d->mac, 6);                    /* source_mac */
    in[42] = 0u;                                   /* roce_l3_type = IPv4 */
    in[43] = 2u;                                   /* roce_version = v2 */
    return mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "SET_ROCE_ADDRESS");
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

int mlx5_hca_init(bfm_dev_t *d)
{
    int rc;
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
    if ((rc = wait_fw_init(d, "pre-init")) != BFM_OK) return rc;
    if ((rc = mlx5_cmd_init(d)) != BFM_OK) return rc;
    if ((rc = wait_fw_init(d, "init")) != BFM_OK) return rc;
    if ((rc = simple_cmd(d, MLX5_CMD_ENABLE_HCA, 0, "ENABLE_HCA")) != BFM_OK) return rc;
    d->hca_enabled = 1;
    if ((rc = set_issi(d)) != BFM_OK) return rc;

    d->fw_pages = (uint8_t *)bfm_mem_alloc((uint64_t)g_bfm_fw_pages_max * BFM_PAGE, BFM_PAGE);
    if (d->fw_pages == NULL) return BFM_E_NOMEM;
    d->fw_pages_used = 0;
    if ((rc = satisfy_pages(d, 1)) != BFM_OK) return rc;
    if ((rc = set_host_endianness(d)) != BFM_OK) return rc;
    if ((rc = setup_general_caps(d)) != BFM_OK) return rc;
    if ((rc = setup_atomic_caps(d)) != BFM_OK) return rc;
    setup_roce_caps(d);
    if ((rc = satisfy_pages(d, 0)) != BFM_OK) return rc;
    {
        uint8_t in[32] = {0}, out[16];
        bfm_wr16be(&in[0], MLX5_CMD_INIT_HCA);
        if ((rc = mlx5_cmd(d, in, sizeof(in), out, sizeof(out), "INIT_HCA")) != BFM_OK) return rc;
    }

    if ((rc = mlx5_eq_create(d)) != BFM_OK) return rc;
    if ((rc = alloc_pd(d, &d->pdn)) != BFM_OK) return rc;
    const uint8_t key8 = (uint8_t)(bfm_port_time_ns() >> 10);
    if ((rc = create_mkey_pa(d, 0x0cu, 0, 0, 0, &d->lkey, "CREATE_MKEY(local)")) != BFM_OK) return rc;
    if ((rc = create_mkey_pa(d, 0x7cu, g_bfm_mr_base, g_bfm_mr_size, key8, &d->rkey,
                             "CREATE_MKEY(公開メモリ)")) != BFM_OK) return rc;

    if (g_bfm_port_mtu && (rc = port_set_mtu(d, g_bfm_port_mtu)) != BFM_OK) return rc;
    if ((rc = port_up(d)) != BFM_OK) return rc;
    port_log_mtu(d);
    if ((rc = vport_enable_roce(d)) != BFM_OK) return rc;
    {
        static const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
        if (memcmp(g_bfm_local_mac, zero, 6) != 0) memcpy(d->mac, g_bfm_local_mac, 6);
        else if ((rc = vport_query_mac(d, d->mac)) != BFM_OK) return rc;
    }
    d->ip = g_bfm_local_ip;
    mlx5_gid_from_ipv4(d->ip, d->gid);
    if ((rc = set_roce_address(d)) != BFM_OK) return rc;

    /* INIT_HCA の後に FW がページを追加で求めてくる。100ms 来なくなるまで答える(上限 2 秒)。 */
    {
        const uint64_t t0 = bfm_port_time_ns();
        uint64_t last = t0;
        while (bfm_port_time_ns() - t0 < 2000000000ull) {
            if (mlx5_eq_poll(d) > 0) last = bfm_port_time_ns();
            else if (bfm_port_time_ns() - last > 100000000ull) break;
            bfm_port_delay_us(1000u);
        }
    }
    BFM_LOG("bfm: HCA 準備完了: MAC %02x:%02x:%02x:%02x:%02x:%02x IP %u.%u.%u.%u リンク %s"
            " FW ページ %u 公開メモリ 0x%llx+0x%llx rkey 0x%08x\n",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
            d->ip >> 24, (d->ip >> 16) & 0xffu, (d->ip >> 8) & 0xffu, d->ip & 0xffu,
            d->link_up ? "UP" : "DOWN", d->fw_pages_used,
            (unsigned long long)g_bfm_mr_base, (unsigned long long)g_bfm_mr_size, d->rkey);
    return BFM_OK;
}

/*=================================================================
 * FW を止める(TEARDOWN_HCA。graceful が断られたら force)。この後は FW に
 * コマンドを出さない(資源を壊さずに止めた後の MANAGE_PAGES・DISABLE_HCA は
 * 返らないことがある。vfio_nvme で実機確認済み)。呼び出し側が Bus Master を落とす。
 * ===============================================================*/
void mlx5_hca_teardown(bfm_dev_t *d)
{
    if (!d->hca_enabled) return;
    int done = 0;
    for (uint16_t prof = 0; prof <= 1u && !done; prof++) {
        uint8_t in[16] = {0}, out[16];
        bfm_wr16be(&in[0], MLX5_CMD_TEARDOWN_HCA);
        bfm_wr16be(&in[10], prof);
        if (mlx5_cmd(d, in, sizeof(in), out, sizeof(out),
                     prof ? "TEARDOWN_HCA(force)" : "TEARDOWN_HCA(graceful)") == BFM_OK &&
            !(prof == 1u && (out[15] & 1u))) {
            done = 1;
        }
    }
    if (!done) BFM_LOG("bfm: 注意: TEARDOWN_HCA が graceful / force とも通らなかった(次の立ち上げの FLR で戻す)\n");
    d->hca_enabled = 0;
}
