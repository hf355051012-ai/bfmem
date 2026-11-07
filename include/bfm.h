#ifndef BFM_H
#define BFM_H

/* =====================================================================
 * bfmem -- ConnectX(mlx5)の BlueFlame で、ノード間のメモリへ直接
 * write / read / atomic(CAS・FAA)を打つためのライブラリ。
 *
 * 使い方(どちらのノードも同じ):
 *
 *   bfm_init();                         // PCIe と HCA を立ち上げる
 *   c = bfm_accept(port, ms);           // 受け側: 相手からの接続を待つ
 *   c = bfm_connect(ip, mac, port, ms); // 掛け側: 相手へ接続する
 *   bfm_write(c, off, src, len);        // 相手の公開メモリ + off へ書く
 *   bfm_read(c, off, dst, len);
 *   bfm_cas(c, off, expect, desired, &old);
 *   bfm_faa(c, off, add, &old);
 *   bfm_cas32(c, off, expect, desired, &old32);   // 4 バイト(拡張 atomic)
 *   bfm_faa32(c, off, add, &old32);
 *   bfm_poll();                         // ときどき呼ぶ(相手からの切断と FW のページ要求に答える)
 *   bfm_disconnect(c);
 *   bfm_shutdown();
 *
 * 1 本の接続(RC QP)で、両方のノードが互いのメモリを触れる。触れるのは
 * 相手が g_bfm_mr_base / g_bfm_mr_size で公開した範囲だけで、off はその先頭からの
 * オフセット。相手の CPU は何もしない(相手の NIC が応答する)。
 *
 * **スレッド安全ではない。** 同じ接続を複数のタスクから同時に使わないこと。
 * 初期化・接続・切断・bfm_poll() も 1 つのタスクから呼ぶこと。
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

typedef struct bfm_conn bfm_conn_t;

/* ---- 立ち上げ ---- */

/* PCIe の設定(FLR・BAR0・Bus Master)と HCA の立ち上げ、RoCE の準備
 * (GID の登録・GSI QP)までを行う。 */
int  bfm_init(void);

/* 全部の接続を畳み、FW を止め(TEARDOWN_HCA)、Bus Master を落とす。 */
void bfm_shutdown(void);

/* 相手からの切断の知らせ(BYE)と FW の非同期イベント(ページ要求など)を処理する。
 * データの経路では呼ばれないので、アプリケーションがときどき呼ぶこと。
 * 戻り値: 処理したイベントの数。 */
int  bfm_poll(void);

/* ---- 接続 ---- */

/* 相手へ接続する。peer_ip が 0 なら g_bfm_peer_ip / g_bfm_peer_mac を、
 * port が 0 なら g_bfm_cm_port を使う。失敗なら NULL(理由は表示する)。 */
bfm_conn_t *bfm_connect(uint32_t peer_ip, const uint8_t peer_mac[6], uint16_t port,
                        uint32_t timeout_ms);

/* port 宛ての接続要求を待って受ける(timeout_ms = 0 なら待ち続ける)。 */
bfm_conn_t *bfm_accept(uint16_t port, uint32_t timeout_ms);

/* 相手へ切断の知らせ(BYE)を送って畳み、QP などを返す。c はこの後使えない。 */
int  bfm_disconnect(bfm_conn_t *c);

/* 1 = 使える。0 = エラーか相手から切断された(bfm_disconnect() で返すこと)。 */
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
