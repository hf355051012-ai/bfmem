/* bfm_peer -- bfmem の試験の相手。Linux のカーネルの mlx5 の上で librdmacm + libibverbs で動く
 * (bfmem の側は VFIO で NIC を直接動かすので、相手は普通の Linux でよい)。
 *
 *   bfm_peer [-a 待ち受けIP] [-p ポート] [-s 公開メモリのバイト数]       受け側(server)
 *   bfm_peer -c <相手のIP> [-p ポート] [-n 回数] [-x]                   掛け側(client)
 *
 * 受け側: 公開メモリを登録し、その先頭・rkey・大きさ・自分の MAC を **REP の private data** で
 *         返す(bfmem の bfm_cm.c と同じ 'BFM1' の形)。中身は bfm_test の server と同じ取り決め
 *         ([0, 64KiB) = (i*7+3)&0xff、[64KiB, +4KiB) = 0)。kill -QUIT か切断で hash を出す。
 * 掛け側: bfmem_test server へ繋ぎ、REQ の private data に自分の公開メモリと MAC を載せる
 *         (bfmem は ARP に答えないので、相手の MAC はこれで知る)。繋がったら相手のメモリへ
 *         read / write / atomic をして確かめる。
 *         **bfmem は ARP に答えないので、掛け側の前に相手の MAC を近隣表へ入れておくこと**
 *         (ip neigh replace <相手のIP> lladdr <相手のMAC> dev <IF> nud permanent)。
 *
 * ビルド: gcc -O2 -Wall -Wextra -o bfm_peer bfm_peer.c -lrdmacm -libverbs */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define DATA_BYTES  65536u
#define ATOM_OFF    65536u
#define ATOM_WORDS  512u
#define PRIV_MAGIC  0x42464d31u
#define PRIV_LEN    36u

static volatile sig_atomic_t g_dump, g_stop;
static uint8_t *g_mem;
static size_t g_mem_len = 1u << 20;
static uint8_t g_mac[6];

static void on_sig(int s)
{
    if (s == SIGQUIT) g_dump = 1;
    else g_stop = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void fill(void)
{
    for (uint32_t i = 0; i < DATA_BYTES; i++) g_mem[i] = (uint8_t)((i * 7u + 3u) & 0xffu);
    memset(g_mem + ATOM_OFF, 0, ATOM_WORDS * 8u);
}

static void dump(const char *why)
{
    printf("[peer] [%s] データ領域の hash = %016llx\n", why, (unsigned long long)fnv1a(g_mem, DATA_BYTES));
    for (unsigned w = 0; w < 3u; w++) {
        uint64_t v;
        memcpy(&v, g_mem + ATOM_OFF + w * 8u, 8);
        printf("[peer]   atomic 語 %u = %llu(ホスト順)\n", w, (unsigned long long)v);
    }
    fflush(stdout);
}

/* addr を持つ IF の MAC。 */
static int find_mac(const struct sockaddr *addr)
{
    struct ifaddrs *ifa0, *ifa;
    char name[IF_NAMESIZE] = "";
    if (getifaddrs(&ifa0) != 0) return -1;
    for (ifa = ifa0; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET &&
            ((const struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr ==
                ((const struct sockaddr_in *)addr)->sin_addr.s_addr) {
            snprintf(name, sizeof(name), "%s", ifa->ifa_name);
        }
    }
    int rc = -1;
    for (ifa = ifa0; ifa && name[0]; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_PACKET && strcmp(ifa->ifa_name, name) == 0) {
            memcpy(g_mac, ((const struct sockaddr_ll *)ifa->ifa_addr)->sll_addr, 6);
            rc = 0;
        }
    }
    freeifaddrs(ifa0);
    return rc;
}

static void put_priv(uint8_t *p, const struct ibv_mr *mr)
{
    uint32_t v32;
    uint64_t v64;
    memset(p, 0, PRIV_LEN);
    v32 = htobe32(PRIV_MAGIC); memcpy(p, &v32, 4);
    p[5] = 1;                                   /* 版 */
    p[7] = 1;                                   /* MAC あり */
    v64 = htobe64((uint64_t)(uintptr_t)mr->addr); memcpy(p + 8, &v64, 8);
    v32 = htobe32(mr->rkey); memcpy(p + 16, &v32, 4);
    v64 = htobe64(mr->length); memcpy(p + 20, &v64, 8);
    memcpy(p + 28, g_mac, 6);
}

typedef struct {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *mr;      /* 公開メモリ */
    struct ibv_mr *lmr;     /* 手元の作業域 */
    uint8_t *lbuf;
    uint64_t raddr, rlen;
    uint32_t rkey;
} peer_t;

/* 失敗したら途中まで作ったものは pr に残る(呼び出し側が teardown() で返す)。 */
static int setup_qp(struct rdma_cm_id *id, peer_t *pr)
{
    pr->pd = ibv_alloc_pd(id->verbs);
    pr->cq = ibv_create_cq(id->verbs, 256, NULL, NULL, 0);
    pr->lbuf = aligned_alloc(4096, 65536);
    if (pr->pd) {
        pr->mr = ibv_reg_mr(pr->pd, g_mem, g_mem_len, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                            IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);
        if (pr->lbuf) pr->lmr = ibv_reg_mr(pr->pd, pr->lbuf, 65536, IBV_ACCESS_LOCAL_WRITE);
    }
    if (!pr->pd || !pr->cq || !pr->lbuf || !pr->mr || !pr->lmr) {
        fprintf(stderr, "[peer] PD / CQ / MR を作れない (errno=%d)\n", errno);
        return -1;
    }
    struct ibv_qp_init_attr qa;
    memset(&qa, 0, sizeof(qa));
    qa.send_cq = qa.recv_cq = pr->cq;
    qa.qp_type = IBV_QPT_RC;
    qa.cap.max_send_wr = 128;
    qa.cap.max_recv_wr = 1;
    qa.cap.max_send_sge = qa.cap.max_recv_sge = 1;
    qa.cap.max_inline_data = 64;
    qa.sq_sig_all = 1;
    if (rdma_create_qp(id, pr->pd, &qa) != 0) {
        fprintf(stderr, "[peer] rdma_create_qp (errno=%d)\n", errno);
        return -1;
    }
    return 0;
}

static void teardown(struct rdma_cm_id *id, peer_t *pr)
{
    if (id->qp) rdma_destroy_qp(id);
    if (pr->lmr) ibv_dereg_mr(pr->lmr);
    if (pr->mr) ibv_dereg_mr(pr->mr);
    if (pr->cq) ibv_destroy_cq(pr->cq);
    if (pr->pd) ibv_dealloc_pd(pr->pd);
    free(pr->lbuf);
    memset(pr, 0, sizeof(*pr));
}

static int get_peer_priv(const void *data, uint8_t len, peer_t *pr)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t m;
    uint64_t v64;
    if (p == NULL || len < PRIV_LEN) return -1;
    memcpy(&m, p, 4);
    if (be32toh(m) != PRIV_MAGIC) return -1;
    memcpy(&v64, p + 8, 8); pr->raddr = be64toh(v64);
    memcpy(&m, p + 16, 4); pr->rkey = be32toh(m);
    memcpy(&v64, p + 20, 8); pr->rlen = be64toh(v64);
    return 0;
}

/* ---------------------------------------------------------------------
 * 受け側
 * ------------------------------------------------------------------- */

static int run_server(const char *bind_ip, uint16_t port)
{
    struct rdma_event_channel *ch = rdma_create_event_channel();
    struct rdma_cm_id *lid = NULL, *cid = NULL, *drop = NULL;
    struct sockaddr_in sa;
    peer_t pr;
    int ret = 1, any = 0;
    memset(&pr, 0, sizeof(pr));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (ch == NULL) {
        fprintf(stderr, "[peer] rdma_create_event_channel (errno=%d)\n", errno);
        return 1;
    }
    if (inet_pton(AF_INET, bind_ip, &sa.sin_addr) != 1) {
        fprintf(stderr, "[peer] %s は IPv4 のアドレスではない\n", bind_ip);
        goto out;
    }
    /* 0.0.0.0 で待つときは、MAC は接続要求が来た IF で調べる */
    any = (sa.sin_addr.s_addr == htonl(INADDR_ANY));
    if (!any && find_mac((struct sockaddr *)&sa) != 0) {
        fprintf(stderr, "[peer] %s を持つ IF が見つからない\n", bind_ip);
        goto out;
    }
    if (rdma_create_id(ch, &lid, NULL, RDMA_PS_TCP) || rdma_bind_addr(lid, (struct sockaddr *)&sa) ||
        rdma_listen(lid, 4)) {
        fprintf(stderr, "[peer] 待ち受けに失敗 (errno=%d)\n", errno);
        goto out;
    }
    fill();
    if (any)
        printf("[peer] 全部の IF の %u で待つ。kill -QUIT で hash\n", port);
    else
        printf("[peer] %s:%u で待つ(MAC %02x:%02x:%02x:%02x:%02x:%02x)。kill -QUIT で hash\n", bind_ip, port,
               g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    fflush(stdout);
    while (!g_stop) {
        struct rdma_cm_event *ev;
        if (g_dump) { g_dump = 0; dump("要求"); }
        if (rdma_get_cm_event(ch, &ev) != 0) {
            if (errno == EINTR) continue;
            break;
        }
        switch (ev->event) {
        case RDMA_CM_EVENT_CONNECT_REQUEST: {
            /* 断った要求の id は ack の後で壊す(drop) */
            if (cid) { rdma_reject(ev->id, NULL, 0); drop = ev->id; break; }
            if (any && find_mac(rdma_get_local_addr(ev->id)) != 0) {
                fprintf(stderr, "[peer] 接続要求が来た IF の MAC が分からない\n");
                rdma_reject(ev->id, NULL, 0);
                drop = ev->id;
                break;
            }
            cid = ev->id;
            peer_t tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (get_peer_priv(ev->param.conn.private_data, ev->param.conn.private_data_len, &tmp) == 0)
                printf("[peer] 接続要求: 相手の公開メモリ 0x%llx+%llu rkey 0x%08x\n",
                       (unsigned long long)tmp.raddr, (unsigned long long)tmp.rlen, tmp.rkey);
            fill();
            struct ibv_device_attr da;
            if (setup_qp(cid, &pr) != 0 || ibv_query_device(cid->verbs, &da) != 0) {
                rdma_reject(cid, NULL, 0);
                teardown(cid, &pr);
                drop = cid;
                cid = NULL;
                break;
            }
            uint8_t priv[PRIV_LEN];
            put_priv(priv, pr.mr);
            struct rdma_conn_param cp;
            memset(&cp, 0, sizeof(cp));
            cp.private_data = priv;
            cp.private_data_len = PRIV_LEN;
            cp.responder_resources = (uint8_t)da.max_qp_rd_atom;
            if (ev->param.conn.initiator_depth < cp.responder_resources)
                cp.responder_resources = ev->param.conn.initiator_depth;
            cp.initiator_depth = (uint8_t)da.max_qp_init_rd_atom;
            if (ev->param.conn.responder_resources < cp.initiator_depth)
                cp.initiator_depth = ev->param.conn.responder_resources;
            cp.rnr_retry_count = 7;
            cp.retry_count = 7;
            if (rdma_accept(cid, &cp) != 0) {
                fprintf(stderr, "[peer] rdma_accept (errno=%d)\n", errno);
                teardown(cid, &pr);
                drop = cid;
                cid = NULL;
            } else {
                printf("[peer] 受けた: 公開メモリ %p rkey 0x%08x len %zu\n", pr.mr->addr, pr.mr->rkey, g_mem_len);
            }
            break;
        }
        case RDMA_CM_EVENT_ESTABLISHED:
            printf("[peer] 確立\n");
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
            if (ev->id != cid) break;                /* 今の接続のものだけ */
            dump("切断");
            teardown(cid, &pr);
            drop = cid;
            cid = NULL;
            break;
        default:
            printf("[peer] CM のイベント %s\n", rdma_event_str(ev->event));
            break;
        }
        fflush(stdout);
        rdma_ack_cm_event(ev);
        /* **rdma_destroy_id() はその id のイベントを全部 ack してから。** 先に呼ぶと ack を待って
         * 永久に止まる(最初の版で踏んだ。次の接続要求が溜まるだけで何も起きなくなる)。 */
        if (drop) { rdma_destroy_id(drop); drop = NULL; }
    }
    ret = 0;
out:
    if (cid) {
        rdma_disconnect(cid);
        teardown(cid, &pr);
        rdma_destroy_id(cid);
    }
    if (lid) rdma_destroy_id(lid);
    rdma_destroy_event_channel(ch);
    return ret;
}

/* ---------------------------------------------------------------------
 * 掛け側
 * ------------------------------------------------------------------- */

static enum ibv_wc_status g_last_status;

#define POST_WAIT_NS 5000000000ull   /* 完了を待つ上限(5 秒。相手の再送が尽きるより長く)*/

static int post_wait(struct rdma_cm_id *id, peer_t *pr, struct ibv_send_wr *wr)
{
    struct ibv_send_wr *bad;
    if (ibv_post_send(id->qp, wr, &bad) != 0) {
        fprintf(stderr, "[cli] ibv_post_send (errno=%d)\n", errno);
        return -1;
    }
    struct ibv_wc wc;
    int n;
    uint32_t spins = 0;
    const uint64_t t0 = now_ns();
    while ((n = ibv_poll_cq(pr->cq, 1, &wc)) == 0) {
        if ((++spins & 0xfffu) == 0 && now_ns() - t0 > POST_WAIT_NS) {
            fprintf(stderr, "[cli] 完了が %llu 秒来ない\n", POST_WAIT_NS / 1000000000ull);
            g_last_status = IBV_WC_GENERAL_ERR;
            return -1;
        }
    }
    g_last_status = (n < 0) ? IBV_WC_GENERAL_ERR : wc.status;
    if (n < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "[cli] 完了がエラー: %s\n", n < 0 ? "poll" : ibv_wc_status_str(wc.status));
        return -1;
    }
    return 0;
}

static int op(struct rdma_cm_id *id, peer_t *pr, enum ibv_wr_opcode opc, uint64_t off, uint32_t len,
              uint64_t a, uint64_t b, int inl)
{
    struct ibv_sge sge = { (uintptr_t)pr->lbuf, len, pr->lmr->lkey };
    struct ibv_send_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = opc;
    wr.send_flags = IBV_SEND_SIGNALED | (inl ? IBV_SEND_INLINE : 0);
    if (opc == IBV_WR_ATOMIC_CMP_AND_SWP || opc == IBV_WR_ATOMIC_FETCH_AND_ADD) {
        wr.wr.atomic.remote_addr = pr->raddr + off;
        wr.wr.atomic.rkey = pr->rkey;
        wr.wr.atomic.compare_add = a;
        wr.wr.atomic.swap = b;
    } else {
        wr.wr.rdma.remote_addr = pr->raddr + off;
        wr.wr.rdma.rkey = pr->rkey;
    }
    return post_wait(id, pr, &wr);
}

/* 次の CM のイベントを待ち、want なら ack して 0。違えば表示して ack し -1。
 * ESTABLISHED のときは ack の前に private data を読む(ack すると読めなくなる)。 */
static int wait_event(struct rdma_event_channel *ch, enum rdma_cm_event_type want, peer_t *pr)
{
    struct rdma_cm_event *ev;
    if (rdma_get_cm_event(ch, &ev) != 0) {
        fprintf(stderr, "[cli] rdma_get_cm_event (errno=%d)\n", errno);
        return -1;
    }
    int rc = 0;
    if (ev->event != want) {
        fprintf(stderr, "[cli] %s(%s を待っていた)\n", rdma_event_str(ev->event), rdma_event_str(want));
        rc = -1;
    } else if (want == RDMA_CM_EVENT_ESTABLISHED &&
               get_peer_priv(ev->param.conn.private_data, ev->param.conn.private_data_len, pr) != 0) {
        fprintf(stderr, "[cli] REP の private data に相手の公開メモリの情報が無い\n");
        rc = -1;
    }
    rdma_ack_cm_event(ev);
    return rc;
}

static int run_client(const char *dst_ip, uint16_t port, unsigned iters, int oob)
{
    struct rdma_event_channel *ch = rdma_create_event_channel();
    struct rdma_cm_id *id = NULL;
    struct addrinfo *ai = NULL;
    peer_t pr;
    int ng = 0, connected = 0;
    memset(&pr, 0, sizeof(pr));
    char ports[8];
    snprintf(ports, sizeof(ports), "%u", port);
    if (ch == NULL) {
        fprintf(stderr, "[cli] rdma_create_event_channel (errno=%d)\n", errno);
        return 1;
    }
    if (getaddrinfo(dst_ip, ports, NULL, &ai) != 0) {
        fprintf(stderr, "[cli] %s が引けない\n", dst_ip);
        ng = 1;
        goto out;
    }
    if (rdma_create_id(ch, &id, NULL, RDMA_PS_TCP) || rdma_resolve_addr(id, NULL, ai->ai_addr, 2000) ||
        wait_event(ch, RDMA_CM_EVENT_ADDR_RESOLVED, &pr) != 0 ||
        rdma_resolve_route(id, 2000) || wait_event(ch, RDMA_CM_EVENT_ROUTE_RESOLVED, &pr) != 0) {
        fprintf(stderr, "[cli] %s への経路が引けない (errno=%d)\n", dst_ip, errno);
        ng = 1;
        goto out;
    }
    if (find_mac(rdma_get_local_addr(id)) != 0) {
        fprintf(stderr, "[cli] 自分の MAC が分からない\n");
        ng = 1;
        goto out;
    }
    fill();
    if (setup_qp(id, &pr) != 0) {
        ng = 1;
        goto out;
    }
    uint8_t priv[PRIV_LEN];
    put_priv(priv, pr.mr);
    struct rdma_conn_param cp;
    memset(&cp, 0, sizeof(cp));
    cp.private_data = priv;
    cp.private_data_len = PRIV_LEN;
    cp.responder_resources = 16;
    cp.initiator_depth = 16;
    cp.retry_count = 7;
    cp.rnr_retry_count = 7;
    if (rdma_connect(id, &cp) != 0) {
        fprintf(stderr, "[cli] rdma_connect (errno=%d)\n", errno);
        ng = 1;
        goto out;
    }
    if (wait_event(ch, RDMA_CM_EVENT_ESTABLISHED, &pr) != 0) {
        fprintf(stderr, "[cli] 接続できない\n");
        ng = 1;
        goto out;
    }
    connected = 1;
    printf("[cli] 接続した: 相手の公開メモリ 0x%llx+%llu rkey 0x%08x\n", (unsigned long long)pr.raddr,
           (unsigned long long)pr.rlen, pr.rkey);
    if (pr.rlen < ATOM_OFF + ATOM_WORDS * 8u) {
        fprintf(stderr, "[cli] 相手の公開メモリが小さすぎる(%u バイト要る)\n", ATOM_OFF + ATOM_WORDS * 8u);
        ng = 1;
        goto out;
    }

    static uint8_t model[DATA_BYTES];
    for (uint32_t i = 0; i < DATA_BYTES; i++) model[i] = (uint8_t)((i * 7u + 3u) & 0xffu);
    /* read 全体 */
    for (uint32_t off = 0; off < DATA_BYTES && !ng; off += 65536u) {
        if (op(id, &pr, IBV_WR_RDMA_READ, off, 65536u, 0, 0, 0) || memcmp(pr.lbuf, model + off, 65536u)) ng++;
    }
    printf("[cli] [1] read 64KiB が初期の模様と一致 %s\n", ng ? "NG" : "OK");
    /* write(inline と通常)と読み戻し */
    uint32_t seed = 777u;
    for (unsigned k = 0; k < 300u && !ng; k++) {
        seed = seed * 1103515245u + 12345u;
        const uint32_t len = 1u + seed % ((k & 1u) ? 64u : 4000u);
        const uint32_t off = (seed >> 9) % (DATA_BYTES - len);
        for (uint32_t j = 0; j < len; j++) pr.lbuf[j] = (uint8_t)(seed >> (j & 15u)) ^ (uint8_t)j;
        memcpy(model + off, pr.lbuf, len);
        if (op(id, &pr, IBV_WR_RDMA_WRITE, off, len, 0, 0, len <= 64u) ||
            op(id, &pr, IBV_WR_RDMA_READ, off, len, 0, 0, 0) || memcmp(pr.lbuf, model + off, len)) ng++;
    }
    printf("[cli] [2] write 1〜4000B と読み戻し %s\n", ng ? "NG" : "OK");
    /* atomic。元の値がどちらのバイト順で返るかは HCA の設定で違う(ConnectX-4 Lx の
     * Linux ではビッグエンディアンで返った)ので、最初の 0 でない値で決めて、その後は揃っていることを見る。 */
    int res_be = -1;
    for (uint64_t i = 0; i < 1000u && !ng; i++) {
        uint64_t old;
        if (op(id, &pr, IBV_WR_ATOMIC_FETCH_AND_ADD, ATOM_OFF, 8, 1, 0, 0)) { ng++; break; }
        memcpy(&old, pr.lbuf, 8);
        if (res_be < 0 && old != 0) res_be = (old != i && be64toh(old) == i);
        if (res_be > 0) old = be64toh(old);
        if (old != i) { printf("[cli] FAA の元の値 %llu(%llu のはず)\n", (unsigned long long)old, (unsigned long long)i); ng++; }
    }
    for (uint64_t i = 0; i < 1000u && !ng; i++) {
        uint64_t old;
        if (op(id, &pr, IBV_WR_ATOMIC_CMP_AND_SWP, ATOM_OFF + 8u, 8, i, i + 1u, 0)) { ng++; break; }
        memcpy(&old, pr.lbuf, 8);
        if (res_be > 0) old = be64toh(old);
        if (old != i) ng++;
    }
    {
        struct ibv_device_attr da;
        const int cap = (ibv_query_device(id->verbs, &da) == 0) ? (int)da.atomic_cap : -1;
        printf("[cli] [3] atomic の元の値は %s で返る(atomic_cap=%d)\n",
               res_be > 0 ? "ビッグエンディアン" : "ホスト順", cap);
    }
    printf("[cli] [3] FAA x1000 / CAS 0->1000 %s\n", ng ? "NG" : "OK");
    printf("[cli] 模型の hash = %016llx(相手の bfmem_test server が出す値と同じはず)\n",
           (unsigned long long)fnv1a(model, DATA_BYTES));
    /* 遅延 */
    if (!ng) {
        const char *names[3] = { "write 8B", "read 8B", "FAA" };
        for (int t = 0; t < 3 && !ng && !g_stop; t++) {
            uint64_t sum = 0;
            unsigned m = 0;                          /* 測れた回数 */
            for (; m < iters && !g_stop; m++) {
                const uint64_t t0 = now_ns();
                /* write は atomic 語の区画の後半へ(データ領域の hash を変えないため)。 */
                int r = (t == 0) ? op(id, &pr, IBV_WR_RDMA_WRITE, ATOM_OFF + 2048u + (m & 31u) * 64u, 8, 0, 0, 1)
                      : (t == 1) ? op(id, &pr, IBV_WR_RDMA_READ, (m & 63u) * 64u, 8, 0, 0, 0)
                                 : op(id, &pr, IBV_WR_ATOMIC_FETCH_AND_ADD, ATOM_OFF + 800u, 8, 1, 0, 0);
                if (r) { ng++; break; }              /* エラーの後は測らない(QP はもう使えない)*/
                sum += now_ns() - t0;
            }
            if (m > 0)
                printf("[cli] 遅延 %-9s 平均 %llu ns(x%u)\n", names[t], (unsigned long long)(sum / m), m);
        }
    }
    /* -x: 相手の公開メモリのすぐ外を読み、相手の NIC が断ること(相手の MKey が範囲を持つこと)。
     * QP はエラーになるので最後にやる。 */
    if (oob && !ng) {
        const int r = op(id, &pr, IBV_WR_RDMA_READ, pr.rlen, 8, 0, 0, 0);
        const int ok = (r != 0 && g_last_status == IBV_WC_REM_ACCESS_ERR);
        printf("[cli] [4] 公開メモリの外を読むと相手が断る(REM_ACCESS_ERR)%s\n", ok ? "OK" : "NG");
        if (!ok) ng++;
    }
out:
    if (connected) {
        rdma_disconnect(id);
        (void)wait_event(ch, RDMA_CM_EVENT_DISCONNECTED, &pr);
    }
    if (id) {
        teardown(id, &pr);
        rdma_destroy_id(id);
    }
    if (ai) freeaddrinfo(ai);
    rdma_destroy_event_channel(ch);
    printf("[cli] 結果 %s\n", ng ? "NG" : "OK");
    return ng ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0", *dst = NULL;
    uint16_t port = 18515;
    unsigned iters = 10000;
    int oob = 0;
    int opt;
    while ((opt = getopt(argc, argv, "a:c:p:s:n:x")) != -1) {
        switch (opt) {
        case 'a': bind_ip = optarg; break;
        case 'c': dst = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 's': g_mem_len = strtoull(optarg, NULL, 0); break;
        case 'n': iters = (unsigned)atoi(optarg); break;
        case 'x': oob = 1; break;
        default:
            fprintf(stderr, "使い方: bfm_peer [-a 待ち受けIP] [-p ポート] [-s バイト数] | -c 相手のIP [-n 回数] [-x]\n");
            return 2;
        }
    }
    if (iters == 0) {
        fprintf(stderr, "-n は 1 以上\n");
        return 2;
    }
    if (g_mem_len < ATOM_OFF + ATOM_WORDS * 8u) g_mem_len = ATOM_OFF + ATOM_WORDS * 8u;
    g_mem = aligned_alloc(4096, (g_mem_len + 4095u) & ~(size_t)4095u);
    if (g_mem == NULL) {
        fprintf(stderr, "公開メモリ %zu バイトを取れない\n", g_mem_len);
        return 1;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;           /* SA_RESTART なし: rdma_get_cm_event() を EINTR で抜ける */
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    return dst ? run_client(dst, port, iters, oob) : run_server(bind_ip, port);
}
