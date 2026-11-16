/*
 * Spotify Web API とのやりとり (実機だけ。自分のタスクで動く)
 * - ログインの鍵 (Client ID と refresh token) は NVS の "spotify" に入れる。ログには出さない
 *   - 鍵がないときは、Wi-Fi の中で小さな受け口 (port 8080) を開いて、Mac の tools/spotify_login.py から受け取る
 *     (画面に出す 4 けたの番号が合ったときだけ受け取る)
 * - 再生中は 2 秒ごと、止まっているときは 4〜5 秒ごとに GET /v1/me/player を見る
 * - 曲が変わったら、曲名はすぐ出す。ジャケットとテンポは裏の「ジャケットの係」(自分のタスク) が取りにいく
 *   (取っている間も操作はすぐ効く)。最近のジャケット 3 枚とテンポは覚えておき、次の曲 (queue) は前もって取っておく
 * - 「次へ」は、次の曲が分かっていれば Spotify の返事を待たずにすぐ出す
 * - 操作 (再生・停止・曲送り・♥・音量) は command() で頼む。終わったらすぐ見直す
 * 使っている API は 2026 年 2 月の Development Mode の変更後も使えるもの (♥ は /me/library)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "PixelArt.h"
#include "TrackMemory.h"

namespace spotify {

enum class NetState : uint8_t {
    Starting,
    WifiConnecting,  // Wi-Fi につないでいる
    WifiSetupAp,     // Wi-Fi の設定待ち (本体が設定用の Wi-Fi を出している)
    NeedLogin,       // Spotify の鍵がない (Mac から受け取る)
    Connecting,      // Spotify につないでいる
    Ready,
    AuthError,  // 鍵が使えない (もう一度ログインが要る)
    NetError,   // しばらくつながらない
};

enum class Cmd : uint8_t { Play, Pause, Next, Prev, Like, Unlike, VolumeUp, VolumeDown };

struct Snapshot {
    NetState state = NetState::Starting;
    char msg[3][96] = {};  // 案内の画面に出す 3 行

    bool hasPlayback = false;  // どこかで曲が選ばれている
    bool playing     = false;
    uint32_t progressMs = 0, durationMs = 0;
    int64_t fetchedUs   = 0;  // progressMs を受け取った時刻
    char title[160]     = {};
    char artist[160]    = {};
    bool isTrack        = false;  // ♥ を付けられる (ポッドキャストではない)
    bool liked          = false;
    char device[48]     = {};  // 再生している機器の名前 (iPhone など)
    float bpm           = 0;      // 曲のテンポ (ReccoBeats から。分からなければ 0)
    float energy        = 0.6f;   // 曲の激しさ 0..1 (同じく)
    float beatOffsetMs  = 0;      // 拍の位置 (曲の位置がこの値のとき拍。覚えていれば)
    uint8_t bpmSource   = 0;      // TempoSource
    char trackId[24]    = {};     // Spotify の曲 ID (曲のときだけ)
    uint32_t pixSeq     = 0;      // 覚えていたドット絵を出すたびに増える
    int volume          = -1;
    bool supportsVolume = false;
    uint32_t trackSeq   = 0;  // 曲が変わるたびに増える
    uint32_t artSeq     = 0;  // ジャケットが変わるたびに増える
    bool artPending     = false;  // 曲が変わって、本物のジャケットをまだ出していない (この間マイクは聞かない)
    uint32_t lastError  = 0;  // 確かめる用: 最後の HTTP の状態
};

class Client {
public:
    bool start(int core);
    void stop();
    bool command(Cmd c);  // 受け付けたら true (頼みの列がいっぱいなら false: 画面の印も出さない)
    void snapshot(Snapshot& out);
    // ジャケットを展開している最中 (このあいだ描く側は、手伝いのコアを使わない)
    bool decoding() const { return _decoding.load(); }
    // ジャケット (RGB565、ふつうのバイト順)。lockArt が true を返したら、使い終わってから unlockArt
    bool lockArt(const uint16_t** px, int* w, int* h);
    void unlockArt();
    bool lockPix(spviz::PixelArt* out);  // 覚えていたドット絵 (pixSeq が増えたら)
    // マイクで聞いたテンポと拍の位置を覚える (描く側から。書くのはジャケットの係)
    void learnTempo(const char* trackId, float bpm, float offsetMs, uint8_t source);

    // 中で使う受け口 (HTTP の応答と、Mac からの設定)
    void httpData(const char* data, int len);
    void httpHeader(const char* key, const char* value);
    bool acceptSetup(int pin, const char* clientId, const char* refresh);

private:
    static void taskEntry(void* arg);
    void run();

    // 鍵
    bool loadCredentials();
    void saveRefresh(const char* refresh);
    void clearCredentials();
    int refreshToken();  // 1 = 取れた、0 = 鍵が使えない、-1 = つながらない
    // Mac からの受け取り
    void startSetupServer();
    void stopSetupServer();
    // API
    int api(const char* method, const char* path, const char* query, char* body = nullptr, size_t bodyCap = 0,
            size_t* bodyLen = nullptr);
    void poll();
    void doCommand(Cmd c);
    bool fetchLiked();
    void fetchQueue();  // 次に流れる曲 (前もってジャケットとテンポを取っておく)
    void publishTrack(const char* uri, const char* title, const char* artist, bool isTrack, const char* artUrl);

    // ジャケットの係 (裏のタスク): ジャケットとテンポを取る。頼むのは requestArt (最新のものだけ残る)
    struct Sink {
        char* buf  = nullptr;
        size_t cap = 0, len = 0;
    };
    struct Job {
        char uri[80];
        char art[200];
    };
    static void artEntry(void* arg);
    void artRun();
    void requestArt(const char* uri, const char* artUrl, bool show);  // show = false は前もって取るだけ
    bool stillWanted(const char* uri);
    int getW(void** handle, const char* url, int timeoutMs, const char* accept);
    const uint8_t* loadJpeg(const char* url, size_t* len);  // 覚えている分か、取ってくる
    bool decodeArt(const uint8_t* jpg, size_t len, bool urgent, const char* cancelUnlessUrl);  // 予備の入れ物 (_spare) へ
    bool cachedJpeg(const char* url) const;
    void rememberPix(const char* artUrl);
    bool showPendingOther(const char* url);  // 別のジャケットを出す頼みが来ている
    bool tempoFor(const char* trackId, float* bpm, float* energy);  // ReccoBeats (覚えている分はそれ)
    void setState(NetState s, const char* m0, const char* m1, const char* m2);

    TaskHandle_t _task          = nullptr;
    SemaphoreHandle_t _done     = nullptr;
    SemaphoreHandle_t _lock     = nullptr;  // _snap
    SemaphoreHandle_t _artLock  = nullptr;
    QueueHandle_t _cmds         = nullptr;
    std::atomic<bool> _running{false};
    std::atomic<bool> _decoding{false};

    Snapshot _snap;
    void* _api      = nullptr;  // esp_http_client (api.spotify.com。つなぎっぱなしで使い回す)
    char* _buf      = nullptr;  // 応答の入れ物 (PSRAM)
    size_t _bufCap  = 0;
    size_t _bufLen  = 0;
    char _clientId[64]  = {};
    char _refresh[512]  = {};
    char _access[512]   = {};
    int64_t _accessUntilUs = 0;
    int64_t _nextPollUs    = 0;
    int64_t _retryAfterUs  = 0;
    bool _rateLimited      = false;  // _retryAfterUs は Spotify の Retry-After (混んでいる)
    int _failures          = 0;
    char _uri[80]          = {};  // 今の曲
    char _queueFor[80]     = {};  // 次の曲を聞いたときの曲
    struct Next {
        bool valid = false;
        char uri[80], title[160], artist[160], art[200];
        uint32_t durMs;
    } _next, _next2;  // 次の曲と、その次
    char _prevUri[80]       = {};  // 「次へ」を押す前の曲 (返事が遅れて前の曲が返ってきても、戻さない)
    bool _optimistic        = false;
    int64_t _expectUntilUs  = 0;  // 曲送りのあと: この時刻までは、こまめに見る

    // ジャケットの係
    TaskHandle_t _artTask         = nullptr;
    SemaphoreHandle_t _artDone    = nullptr;
    SemaphoreHandle_t _jobLock    = nullptr;
    Job _jobShow{}, _jobPre{}, _jobDl{};
    bool _hasShow = false, _hasPre = false, _hasDl = false;  // 出す・先に読んでおく・取っておくだけ (2 曲先)
    void requestDownload(const char* uri, const char* artUrl);
    char _wantUri[80]  = {};  // 画面に出す曲 (_jobLock)
    char _shownArt[200] = {};  // 係だけが使う
    std::atomic<bool> _pixOver{false};  // 覚えていたドット絵を出したまま、本物のジャケット (artSeq) をまだ出し直していない
    char _readyArt[2][200] = {};  // 展開ずみの絵 (出ている絵・予備)。通信の係が見るので _jobLock で守る
    char _pixUrl[200]      = {};  // いま出ているドット絵のジャケット (_lock)
    void markReady();             // 係: _shownArt と _spareUrl を _readyArt に写す
    bool showPixFor(const char* artUrl);  // 覚えていたドット絵があれば出す (どちらの係からでも)
    Sink _w;
    void* _imgHttp = nullptr;  // i.scdn.co (つなぎっぱなし)
    void* _rbHttp  = nullptr;  // api.reccobeats.com
    struct ArtCache {
        char url[200];
        uint8_t* jpg;
        size_t len;
        uint32_t stamp;
    } _artCache[4] = {};
    TrackMemory _mem;
    spviz::PixelArt _pixShow{};
    uint32_t _stamp = 0;
    // ジャケット (画面の大きさ 466px まで、RGB565)。_shown を画面が読み、_spare に次のものを読んでおく (入れかえるだけで出る)
    uint16_t* _shown = nullptr;
    uint16_t* _spare = nullptr;
    int _shownW = 0, _shownH = 0, _spareW = 0, _spareH = 0;
    char _spareUrl[200] = {};  // _spare に入っている絵 (係だけが使う)
    const char* _cancelUnless = nullptr;

    void* _server = nullptr;  // httpd_handle_t
    int _pin      = 0;
    int64_t _setupRetryUs = 0;  // 受け取りのサーバを作れなかった: この時刻まで作り直さない
    std::atomic<bool> _credsArrived{false};
    char _ip[20] = {};
};

}  // namespace spotify
