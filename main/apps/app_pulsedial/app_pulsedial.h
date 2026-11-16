/*
 * Spotify リモコン: いま Spotify で流れている曲を、Spotify 公式風 + リキッドグラス風の画面で見せて、操作もできるアプリ。
 * 常時表示を前提に、焼き付きにくくしてある (全体をゆっくり動かす・長く止めたら時計)。
 * - 画面は GlassUi、音の解析は SpotifyViz の Analyzer (どちらも Mac のシミュレータと共通)。
 *   再生中バーとふちの光は、本体のマイクで部屋の音を聞いて動かす
 * - Spotify とのやりとりは SpotifyClient (自分のタスク)。最初の 1 回だけ、Mac の tools/spotify_login.py でログインする
 * - 開いている間だけ Wi-Fi を使う (設定で Wi-Fi を切っていたら、閉じたときに切り直す)
 *
 * 操作: ボタン A = 再生/停止、B 1 回 = 次の曲、B 2 回 = 前の曲、B 長押し = ♥ (お気に入り)
 *       A と B を同時に長押し = ランチャーへ戻る (公式アプリ共通)
 *       画面はいつもジャケットいっぱい + ふちの白い波形 (拍で震える)。タップ = 詳細 (機器名・時間・BPM) を 7 秒、
 *       ♥ の丸 = お気に入り、左右にはらう = 曲送り、上下にはらう = 音量 (変えられる機器だけ)。操作ごとにエフェクト
 */
#pragma once

#include "GlassUi.h"
#include "SpotifyClient.h"
#include "SpotifyViz.h"
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <atomic>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class AppPulseDial : public mooncake::AppAbility {
public:
    AppPulseDial();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    static constexpr int kRenderHz = 30;  // 2 つのコアで描くので 30fps まで

    enum class Ui : uint8_t { Play, Pause, Next, Prev, Like, Unlike, Wake, Ambient, VolUp, VolDown };

    static void audioTaskEntry(void* arg);
    static void renderTaskEntry(void* arg);
    static void helperTaskEntry(void* arg);
    static void runParallel(void* ctx, spviz::GlassUi::ParJob job, void* arg);
    void audioLoop();
    void renderLoop();
    void renderFrame();
    void applySnapshot(const spotify::Snapshot& s, int64_t now);
    void post(Ui u);
    void togglePlay();
    void toggleLike();

    std::unique_ptr<input::KeyManager> _key_manager;
    spviz::Analyzer* _analyzer = nullptr;
    spviz::GlassUi* _ui_view   = nullptr;
    uint16_t* _fb              = nullptr;  // 画面バッファ (公式の Panel_AMOLED_Framebuffer の中身。ここへ直接描く)
    int _fb_w = 468, _fb_h = 466;
    spotify::Client _client;
    bool _ready          = false;
    bool _client_started = false;

    TaskHandle_t _audio_task          = nullptr;
    TaskHandle_t _render_task         = nullptr;
    SemaphoreHandle_t _audio_stopped  = nullptr;
    SemaphoreHandle_t _render_stopped = nullptr;
    QueueHandle_t _ui                 = nullptr;
    std::atomic<bool> _running{false};
    bool _audio_task_created  = false;
    bool _render_task_created = false;

    // 描く手伝い (もう片方のコア): 波形などの半分を同時に描く
    TaskHandle_t _helper_task          = nullptr;
    SemaphoreHandle_t _helper_done     = nullptr;
    SemaphoreHandle_t _helper_stopped  = nullptr;
    spviz::GlassUi::ParJob _helper_job = nullptr;
    void* _helper_arg                  = nullptr;
    std::atomic<bool> _helper_quit{false};

    // 描くタスクの中だけで使う
    int64_t _last_frame_us   = 0;
    int64_t _paused_since_us = 0;
    uint32_t _track_seq = 0, _art_seq = 0, _pix_seq = 0;
    // テンポ: 覚えた値・ReccoBeats・マイク。マイクに音楽が入っているときだけ、拍の位置を合わせ直して覚える (イヤホンなら何もしない)
    void tempoStep(const spotify::Snapshot& s, const spviz::Frame& in, float dt);
    void showNoticeFor(const spotify::Snapshot& s);
    char _qr_text[128] = {};  // いま出している QR の中身 (同じなら作り直さない)
    void setQr(const std::string& text);
    uint32_t _tempo_track = 0;
    float _pos_ms         = 0;  // 曲の位置 (このコマ)
    float _off            = 0;  // マイクで合わせた拍の位置
    bool _off_ok          = false;
    bool _learned         = false;
    float _agree_s = 0, _stable_s = 0, _stable_bpm = 0, _disagree_s = 0, _no_hear_s = 0;
    int _bright_now      = -1;
    int64_t _next_log_us = 0;
    uint32_t _frames      = 0;
    uint32_t _rows_sent   = 0;
    uint32_t _send_us_sum = 0;

    // メインのタスク (ボタン・タッチ)
    bool _touch_down       = false;
    uint32_t _down_ms      = 0;
    int _down_x = 0, _down_y = 0, _last_x = 0, _last_y = 0;
    bool _b_held           = false;
    bool _a_logout         = false;  // A を 10 秒長押しでログアウトした (離すまで 1 回だけ)
    int _main_prio         = -1;  // ボタンを読むタスクの元の優先度 (開いている間は上げる)  // B を長押しした (そのあとのクリックの回数は使わない)
    uint32_t _next_time_ms = 0;

    // 両方のタスクから
    std::atomic<int> _hour{-1};
    std::atomic<int> _minute{-1};
    std::atomic<std::uint32_t> _mic_errors{0};
    std::atomic<std::uint32_t> _mic_drops{0};  // マイクの音を取りこぼした回数 (2 秒ごとに見る)
    std::atomic<int> _mic_rate{0};             // 実際に受け取れている音の速さ (Hz)
    std::atomic<int> _mic_proc_us{0};          // 解析 1 回 (11.6ms 分) にかかった時間
    std::atomic<bool> _mic_off{false};  // 拍で動かしているときは、マイクを聞かない
    std::atomic<bool> _light{false};  // Wi-Fi の設定中など: 音を解析せず、ゆっくり描く (設定ページに CPU を空ける)
    std::atomic<int> _frame_us{33333};
    std::atomic<uint32_t> _last_input_ms{0};  // 最後に操作した時刻 (コマ数を決める)
    std::atomic<int> _vol_shown{-1};       // 音量を変えたときに見せる値
    volatile uint32_t _vol_changed_ms = 0;  // 音量を変えた時刻 (ms)
    int _user_bright       = 80;
    bool _wifi_was_enabled = false;
};
