#ifndef BFM_H
#define BFM_H

/* =====================================================================
 * bfmem -- ConnectX(mlx5)の BlueFlame で、ノード間のメモリへ直接
 * write / read / atomic(CAS・FAA)を打つためのライブラリ。
 *
 * 使い方(どちらのノードも同じ。設定は bfm_config.h の変数):
 *
 *   while (bfm_run() != BFM_OK) 別の仕事;     // 代表 API。どのコアから何本同時に呼んでもよい
 *   c = bfm_conn(port, i);                      // ポート port の接続 i(使えなければ NULL)
 *   bfm_write(c, off, src, len);                // 相手の公開メモリ + off へ書く
 *   bfm_read(c, off, dst, len);
 *   bfm_cas(c, off, expect, desired, &old);
 *   bfm_faa(c, off, add, &old);
 *   bfm_cas32(c, off, expect, desired, &old32); // 4 バイト(拡張 atomic)
 *   bfm_faa32(c, off, add, &old32);
 *   bfm_run();                                  // 使える後も、ときどき呼び続ける
 *   while (bfm_shutdown() == BFM_INPROGRESS) 別の仕事;
 *
 * **代表 API bfm_run()** は、呼ぶたびに次を待たずに少しずつ進める(待つところでは戻る)。
 * 呼び続けると、初期化(PCIe)→ HCA の立ち上げ(init)→ 接続(connect)→ 転送の準備 →
 * メモリの公開(相手からの write / read / atomic を受ける準備)まで済み、BFM_OK を返す。
 *   - 接続の表(ポート x g_bfm_port_conn[] 本。既定 2 ポート x 16 本 = 32 本)の行ごとに状態機械を持つ。
 *     bfm_run() は表を回り、**鍵を取れた行の状態を 1 つ進める**(取れなければ待たずに次の行へ)。
 *     最初に鍵を取ったコアが最初の状態を処理し、次の状態も、そのとき鍵を取ったコアが処理する。
 *     ポートの立ち上げ(PCIe・HCA・GSI)は、そのポートの行を進めたコアが行う。
 *   - 使えるようになった後も、ときどき呼び続けること(相手からの切断の知らせ・FW のページ要求に答える)。
 *
 * **双方向。** 両方のノードで同じものを動かす。ポートごとに、IPv4 の小さいほうのノードが掛け側、
 * 大きいほうが受け側になって、接続 (ポート p, 番号 i) どうしが対になる。1 本の接続(RC QP)で、
 * どちらのノードからも相手のメモリを触れる。触れるのは相手が g_bfm_mr_base / g_bfm_mr_size で
 * 公開した範囲だけで、off はその先頭からのオフセット。相手の CPU は何もしない(相手の NIC が応答する)。
 *
 * **複数のコアから同時に転送できる。** コアごとに別の接続を使うこと。データ経路
 * (write / read / atomic / T10-DIF)は鍵を使わず接続ごとの資源だけを触るので、別の接続どうしは
 * 互いを待たない。**1 つの接続を複数のコアから同時に使わないこと。**
 *   - bfm_run() / bfm_conn() / bfm_conn_state() / bfm_reconnect() は、どのコアから同時に呼んでもよい。
 *   - bfm_shutdown() は、ほかのコアが bfmem を使っていないときに 1 つのコアから呼ぶこと。
 *   - どの接続も相手の同じ公開メモリを触るので、別の接続から同じ場所へ書いたときの順は決まらない
 *     (atomic はどの接続から打っても不可分)。
 * ===================================================================== */

#include <stdint.h>
#include "bfm_config.h"

/* 戻り値(0 = 成功、負 = 失敗)。 */
#define BFM_OK            0
#define BFM_E_PARAM     (-1)   /* 引数が不正(範囲外・境界が合わない・長さ 0)*/
#define BFM_E_STATE     (-2)   /* 接続が使えない(未接続・エラー・切断済み)*/
#define BFM_E_TIMEOUT   (-3)   /* 完了・応答が来ない */
#define BFM_E_HW        (-4)   /* FW のコマンドが失敗した */
#define BFM_E_NOMEM     (-5)   /* DMA 領域・接続の枠が足りない */
#define BFM_E_REMOTE    (-6)   /* 相手が拒否した(rkey・範囲・権限)*/
#define BFM_E_RETRY     (-7)   /* 再送が尽きた(相手に届かない)*/
#define BFM_E_CQE       (-8)   /* そのほかのエラー完了 */
#define BFM_E_DIF       (-9)   /* T10-DIF の検査で誤りを見つけた(bfm_dif_err_t に詳しく)*/
#define BFM_E_NOTSUP    (-10)  /* 装置が対応していない(T10-DIF のオフロードなど)*/
#define BFM_E_BUSY      (-11)  /* その接続の前の T10-DIF の転送が、まだ終わっていない */

/* 待ちの途中(正の値)。同じ関数を後でもう一度呼ぶと先へ進む。 */
#define BFM_INPROGRESS    1

typedef struct bfm_conn bfm_conn_t;

/* ---- 代表 API(立ち上げ・接続・公開をまとめて進める)---- */

/* 接続の状態(bfm_conn_state() の値)。 */
enum {
    BFM_ST_UNUSED  = 0,   /* 使わない(g_bfm_port_conn[] の外)*/
    BFM_ST_INIT    = 1,   /* ポートの立ち上げ中(PCIe・HCA・公開メモリの MKey・GSI)*/
    BFM_ST_CONNECT = 2,   /* 接続の確立中(相手を待っている間もここ)*/
    BFM_ST_UP      = 3,   /* 使える(転送の準備とメモリの公開まで済んだ)*/
    BFM_ST_DOWN    = 4,   /* エラーになった・相手が切った(bfm_reconnect() で張り直す)*/
    BFM_ST_FAILED  = 5    /* 失敗した(ポートの立ち上げの失敗など。bfm_shutdown() の後にやり直す)*/
};

/* 全部を待たずに少しずつ進める(上の説明)。どのコアから何本同時に呼んでもよい。
 * 戻り値: BFM_OK = 全部の接続が使える。BFM_INPROGRESS = 途中(呼び続ける)。
 *         負 = 失敗した接続がある(BFM_ST_FAILED。理由は表示する)。BFM_E_STATE = 止めている途中か、
 *         エラー・切断のままの接続がある(BFM_ST_DOWN)。どの値でも、呼び続けてよい(ほかの接続は進む)。 */
int  bfm_run(void);

/* ポート port の接続 i。使える(BFM_ST_UP)ときだけ返し、それ以外は NULL。 */
bfm_conn_t *bfm_conn(unsigned port, unsigned i);

/* ポート port の接続 i の状態(BFM_ST_*)。 */
int  bfm_conn_state(unsigned port, unsigned i);

/* ポート port の接続 i が、エラーになった・相手が切った(BFM_ST_DOWN)か使える(BFM_ST_UP)とき、張り直すよう
 * 頼む(相手へ BYE を送って畳み、bfm_run() が確立からやり直す)。その接続で転送しているコアが無いときに
 * 呼ぶこと。相手のノードでは、その接続が BFM_ST_DOWN になるので、相手も bfm_reconnect() を呼ぶこと。
 * 戻り値: BFM_OK = 頼んだ。BFM_E_STATE = 確立の途中・失敗した接続。BFM_E_PARAM = 使わない接続。 */
int  bfm_reconnect(unsigned port, unsigned i);

/* 全部の接続を畳み(相手へ BYE を送る)、FW を止め(TEARDOWN_HCA)、Bus Master を落とす。待つところで
 * BFM_INPROGRESS を返すので、BFM_OK が返るまで呼び続ける。立ち上げの途中でも呼べる(そこで止める)。
 * 止めた後に bfm_run() を呼ぶと、最初から立ち上げ直す。 */
int  bfm_shutdown(void);

/* 1 = 使える。0 = エラーか相手から切断された(bfm_reconnect() で張り直すこと)。 */
int  bfm_is_up(const bfm_conn_t *c);

/* 相手の公開メモリの大きさ(off の上限)。 */
uint64_t bfm_remote_size(const bfm_conn_t *c);

/* ---- メモリ操作(相手の公開メモリ + off)---- */

/* 書いて、相手の NIC が受け取った(ACK が返った)ところで戻る。
 * bfm_inline_max() バイトまではデータを WQE に埋め込み、BlueFlame で 1 回の
 * MMIO 書き込みにする。それより長いものは中継バッファから送る。 */
int bfm_write(bfm_conn_t *c, uint64_t off, const void *src, uint32_t len);

/* 書いて、完了を待たずに戻る(len <= bfm_inline_max())。後で bfm_flush() で待つ。
 * 送る順に相手のメモリへ届く(RC の順序保証)。 */
int bfm_write_post(bfm_conn_t *c, uint64_t off, const void *src, uint32_t len);

/* bfm_write_post() で出したものが全部終わるまで待つ。 */
int bfm_flush(bfm_conn_t *c);

/* 読む。32 バイトまでは応答が CQE に入るので、dst へ CPU で写す。 */
int bfm_read(bfm_conn_t *c, uint64_t off, void *dst, uint32_t len);

/* 8 バイトの compare-and-swap。off は 8 の倍数。*old に元の値が入る
 * (*old == expect なら置き換わった)。 */
int bfm_cas(bfm_conn_t *c, uint64_t off, uint64_t expect, uint64_t desired, uint64_t *old);

/* 8 バイトの fetch-and-add。off は 8 の倍数。*old に足す前の値が入る。 */
int bfm_faa(bfm_conn_t *c, uint64_t off, uint64_t add, uint64_t *old);

/* 4 バイトの compare-and-swap。off は 4 の倍数。*old に元の値が入る。 */
int bfm_cas32(bfm_conn_t *c, uint64_t off, uint32_t expect, uint32_t desired, uint32_t *old);

/* 4 バイトの fetch-and-add。off は 4 の倍数。*old に足す前の値が入る。 */
int bfm_faa32(bfm_conn_t *c, uint64_t off, uint32_t add, uint32_t *old);

/* ---- T10-DIF(512 + 8 バイトのブロック)----
 *
 * 1 ブロック = データ 512 バイト + 保護情報(PI)8 バイト。PI はデータの直後に置く(520 バイトおき)。
 *   PI[0..1] guard   = データ 512 バイトの CRC16(T10-DIF、多項式 0x8BB7、初期値 0)、ビッグエンディアン
 *   PI[2..3] app tag = ビッグエンディアン
 *   PI[4..7] ref tag = ビッグエンディアン。ブロックごとに 1 増える(Type 1)
 * PI の無い形は 512 バイトおき。
 *
 * 操作(入る側 = write では自分のメモリ、read では相手のメモリ。出る側はその反対):
 *   BFM_DIF_CHECK         入る側の PI を検査し、そのまま渡す(520 -> 520)
 *   BFM_DIF_CHECK_SWAP    入る側の PI を検査し、app tag / ref tag を new_* に置き換える(520 -> 520。guard はそのまま)
 *   BFM_DIF_INSERT        PI を作って付ける(512 -> 520)
 *   BFM_DIF_CHECK_DELETE  入る側の PI を検査し、取り除く(520 -> 512)
 * mode:
 *   BFM_DIF_SW  CPU が CRC を計算する(どの装置でも使える。ConnectX-4 もこれ)
 *   BFM_DIF_HW  NIC の署名オフロード(署名つき MKey)。bfm_dif_hw_supported() が 1 のときだけ
 * 検査で誤りを見つけたら BFM_E_DIF を返し、*err に場所と中身を入れる。read では誤りの前の
 * ブロックまで dst に書いてある。write では、SW は誤りのある範囲を送らず、HW は送ってしまう
 * ことがある(誤りは送った後に分かる)。app tag の 0xffff を「検査しない」とみなす扱い(escape)はしない。
 *
 * 起動と完了確認を分けて使える(1 つの接続で同時に動かせる DIF の転送は 1 本):
 *   bfm_dif_write_start() / bfm_dif_read_start()  起動する(最初の分を出して、すぐ戻る)
 *   bfm_dif_poll()  待たずに進める(次の分を出す・届いた分を検査する)。終わったら結果を返す
 *   bfm_dif_wait()  終わるまで進める
 * 転送は DIF 用の中継バッファ(g_bfm_dif_buf_size)を 2 面に分けて交互に使い、片方を NIC が
 * 運ぶ間に、もう片方を CPU が用意・検査する。進むのは bfm_dif_poll() / bfm_dif_wait() の中だけ
 * なので、終わるまでどちらかを呼び続けること。終わるまで src / dst を変えたり手放したりしない。
 * 途中でも、同じ接続で普通の write / read / atomic を出してよい(中継バッファは別)。
 * bfm_dif_write() / bfm_dif_read() は、起動して bfm_dif_wait() で待つのと同じ。 */
#define BFM_DIF_BLOCK 512u
#define BFM_DIF_PI    8u

enum { BFM_DIF_CHECK = 0, BFM_DIF_CHECK_SWAP = 1, BFM_DIF_INSERT = 2, BFM_DIF_CHECK_DELETE = 3 };
enum { BFM_DIF_SW = 0, BFM_DIF_HW = 1 };

#define BFM_DIF_CHK_GUARD 0x1u
#define BFM_DIF_CHK_APP   0x2u
#define BFM_DIF_CHK_REF   0x4u
#define BFM_DIF_CHK_ALL   0x7u

typedef struct {
    uint8_t  op;            /* BFM_DIF_CHECK など */
    uint8_t  mode;          /* BFM_DIF_SW / BFM_DIF_HW */
    uint8_t  check;         /* 検査するもの(BFM_DIF_CHK_*)。INSERT では使わない */
    uint16_t app_tag;       /* 入る側の app tag(INSERT では付ける値)*/
    uint32_t ref_tag;       /* 最初のブロックの ref tag(INSERT では付ける値)*/
    uint16_t new_app_tag;   /* CHECK_SWAP で置き換える app tag */
    uint32_t new_ref_tag;   /* CHECK_SWAP で置き換える、最初のブロックの ref tag */
} bfm_dif_t;

typedef struct {
    uint32_t block;         /* 誤りのあったブロック(先頭からの番号)*/
    uint8_t  what;          /* BFM_DIF_CHK_GUARD / APP / REF */
    uint32_t expected;      /* あるべき値 */
    uint32_t actual;        /* 実際の値 */
} bfm_dif_err_t;

/* bfm_dif_poll() の戻り値: まだ終わっていない。 */
#define BFM_DIF_INPROGRESS BFM_INPROGRESS

/* src の nblocks ブロックを、形を変えて相手の公開メモリ + off へ書く(終わるまで待つ)。
 * src の形は入る側(CHECK・SWAP・DELETE は 520 バイトおき、INSERT は 512 バイトおき)。
 * err は NULL でもよい。 */
int bfm_dif_write(bfm_conn_t *c, uint64_t off, const void *src, uint32_t nblocks,
                  const bfm_dif_t *dif, bfm_dif_err_t *err);

/* 相手の公開メモリ + off から nblocks ブロックを読み、形を変えて dst に置く(終わるまで待つ)。
 * dst の形は出る側(CHECK・SWAP・INSERT は 520 バイトおき、DELETE は 512 バイトおき)。 */
int bfm_dif_read(bfm_conn_t *c, uint64_t off, void *dst, uint32_t nblocks,
                 const bfm_dif_t *dif, bfm_dif_err_t *err);

/* bfm_dif_write() を起動だけして戻る。結果は bfm_dif_poll() / bfm_dif_wait() で受け取る。
 * 戻り値は起動できたかどうかだけ(BFM_OK・BFM_E_PARAM・BFM_E_STATE・BFM_E_NOTSUP、
 * 前の転送が終わっていなければ BFM_E_BUSY)。dif は戻った後に変えてよい。 */
int bfm_dif_write_start(bfm_conn_t *c, uint64_t off, const void *src, uint32_t nblocks,
                        const bfm_dif_t *dif);

/* bfm_dif_read() を起動だけして戻る。戻り値は bfm_dif_write_start() と同じ。 */
int bfm_dif_read_start(bfm_conn_t *c, uint64_t off, void *dst, uint32_t nblocks,
                       const bfm_dif_t *dif);

/* 起動した転送を、待たずに進める。
 * 戻り値: BFM_DIF_INPROGRESS = まだ途中。BFM_OK = 全部済んだ。負 = 失敗(BFM_E_DIF なら *err に
 * 場所と中身)。結果を返すのは 1 回だけで、その後は次の転送を起動できる。
 * 動いている転送が無ければ BFM_OK。err は NULL でもよい。 */
int bfm_dif_poll(bfm_conn_t *c, bfm_dif_err_t *err);

/* 起動した転送が終わるまで進め、結果を返す(戻り値は bfm_dif_poll() の BFM_DIF_INPROGRESS 以外)。
 * g_bfm_op_timeout_ms の間まったく進まなければ BFM_E_TIMEOUT(接続はエラーにする)。 */
int bfm_dif_wait(bfm_conn_t *c, bfm_dif_err_t *err);

/* 1 = BFM_DIF_HW が使える(使う全部のポートに署名オフロード(sho)があり、資源を用意できた)。
 * ポートの立ち上げが済む前は 0。 */
int bfm_dif_hw_supported(void);

/* データ 512 バイトの PI(8 バイト)を作る(アプリケーションが PI つきのデータを用意するため)。 */
void bfm_dif_make_pi(const void *data, uint16_t app_tag, uint32_t ref_tag, uint8_t pi[8]);

/* ---- 情報 ---- */

/* WQE に埋め込める write の最大バイト数。 */
uint32_t bfm_inline_max(void);

/* 1 = BlueFlame で WQE を送っている(WC の窓が取れた)。 */
int bfm_bf_active(const bfm_conn_t *c);

/* 直前のエラー完了の syndrome(0 = なし)。 */
uint8_t bfm_last_syndrome(const bfm_conn_t *c);

/* 戻り値の説明。 */
const char *bfm_strerror(int rc);

#endif /* BFM_H */
