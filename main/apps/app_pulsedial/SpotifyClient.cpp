/*
 * Spotify Web API とのやりとり。説明は SpotifyClient.h
 */
#include "SpotifyClient.h"

#include "ArtJpeg.h"

#include <hal/hal.h>
#include <hal/utils/settings/settings.h>
#include <mooncake_log.h>
#include <ArduinoJson.h>

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include <algorithm>
#include <string>

namespace spotify {

namespace {

constexpr const char* kTag       = "spotify";
constexpr const char* kNs        = "spotify";  // NVS
constexpr const char* kApi       = "https://api.spotify.com";
constexpr size_t kBufCap         = 360 * 1024;  // 応答 (ジャケットの JPEG も入る。640px で 100〜250KB)
constexpr int kArtSide           = 466;         // ジャケットを持つ大きさ (画面いっぱい)。読みながらここまで縮める
constexpr int kSetupPort         = 8080;
constexpr const char* kCallbackUrl = "https://aigamoid.github.io/pulsedial/callback.html";  // QR ログインの戻り先 (GitHub Pages)
constexpr const char* kScopes =
    "user-read-playback-state user-modify-playback-state user-read-currently-playing user-library-read user-library-modify";
constexpr int64_t kPollPlayingUs = 2000000;
constexpr int64_t kPollPausedUs  = 4000000;
constexpr int64_t kPollIdleUs    = 5000000;
constexpr int64_t kPollExpectUs  = 500000;   // 曲送りのあと、変わったのが見えるまで
constexpr size_t kArtBufCap      = 320 * 1024;  // ジャケットの係の入れ物 (JPEG)。大きい絵が来たら広げる
constexpr size_t kArtBufMax      = 1024 * 1024;

int64_t nowUs()
{
    return esp_timer_get_time();
}

void copyStr(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

// URL の中に入れるための書き換え (英数字と -_.~ 以外を %XX に)
std::string urlEncode(const char* s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (; *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// フォームの値を取り出す (a=1&b=2 の形)。%XX と + を戻す
bool formValue(const char* body, const char* key, char* out, size_t cap)
{
    const size_t klen = strlen(key);
    const char* p     = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t n = 0;
            while (*p && *p != '&' && n + 1 < cap) {
                if (*p == '%' && isxdigit(static_cast<unsigned char>(p[1])) && isxdigit(static_cast<unsigned char>(p[2]))) {
                    char hx[3] = {p[1], p[2], 0};
                    out[n++]   = static_cast<char>(strtol(hx, nullptr, 16));
                    p += 3;
                } else {
                    out[n++] = *p == '+' ? ' ' : *p;
                    ++p;
                }
            }
            out[n] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    return false;
}

// JPEG の本当の大きさ (SOF の印から読む)。Spotify の「640px」でも、中身は 500px などのことがある
bool jpegSize(const uint8_t* d, size_t n, int* w, int* h, bool* progressive)
{
    size_t i = 2;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    while (i + 9 < n) {
        if (d[i] != 0xFF) {
            ++i;
            continue;
        }
        const uint8_t m = d[i + 1];
        if (m == 0xFF) {
            ++i;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  // 長さのない印
            i += 2;
            continue;
        }
        const size_t len = (static_cast<size_t>(d[i + 2]) << 8) | d[i + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h           = (d[i + 5] << 8) | d[i + 6];
            *w           = (d[i + 7] << 8) | d[i + 8];
            *progressive = m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE;
            return *w > 0 && *h > 0;
        }
        i += 2 + len;
    }
    return false;
}

esp_err_t onHttpEvent(esp_http_client_event_t* e)
{
    auto* self = static_cast<Client*>(e->user_data);
    if (!self) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        self->httpData(static_cast<const char*>(e->data), e->data_len);
    } else if (e->event_id == HTTP_EVENT_ON_HEADER) {
        self->httpHeader(e->header_key, e->header_value);
    }
    return ESP_OK;
}

// ジャケットの係用: 応答をその係の入れ物へ
esp_err_t onSinkEvent(esp_http_client_event_t* e)
{
    struct SinkView {
        char* buf;
        size_t cap, len;
    };
    auto* sk = static_cast<SinkView*>(e->user_data);
    if (sk && e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 && sk->buf) {
        if (sk->len + e->data_len + 1 > sk->cap && sk->cap < kArtBufMax) {  // 入りきらない: 広げる
            const size_t want = std::min(kArtBufMax, std::max(sk->cap * 3 / 2, sk->len + e->data_len + 1));
            if (void* p = heap_caps_realloc(sk->buf, want, MALLOC_CAP_SPIRAM)) {
                sk->buf = static_cast<char*>(p);
                sk->cap = want;
            }
        }
        const size_t n = std::min<size_t>(e->data_len, sk->cap - 1 - sk->len);
        memcpy(sk->buf + sk->len, e->data, n);
        sk->len += n;
    }
    return ESP_OK;
}

// ジャケット: 640px にいちばん近いもの (画面いっぱいに出すので大きいもの)
std::string pickArt(JsonObject it)
{
    JsonArray imgs = it["album"]["images"].as<JsonArray>();
    if (imgs.isNull()) imgs = it["images"].as<JsonArray>();
    std::string url;
    int best = 1 << 30;
    for (JsonObject im : imgs) {
        const int w = im["width"] | 300;
        const int d = abs(w - 640) + (w < 200 ? 1000 : 0);
        if (d < best) {
            best = d;
            url  = im["url"] | "";
        }
    }
    return url;
}

std::string joinArtists(JsonObject it)
{
    std::string artist;
    JsonArray list = it["artists"].as<JsonArray>();
    for (JsonObject a : list) {
        if (!artist.empty()) artist += ", ";
        artist += a["name"] | "";
    }
    if (artist.empty()) artist = it["show"]["name"] | "";
    return artist;
}

esp_err_t onSetup(httpd_req_t* req)
{
    auto* self = static_cast<Client*>(req->user_ctx);
    char body[1400];
    const int total = std::min<int>(req->content_len, sizeof(body) - 1);
    int got         = 0;
    while (got < total) {
        const int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;
    char pin[8] = {}, id[64] = {}, refresh[512] = {};
    formValue(body, "pin", pin, sizeof(pin));
    formValue(body, "client_id", id, sizeof(id));
    formValue(body, "refresh_token", refresh, sizeof(refresh));
    const bool ok = self && self->acceptSetup(atoi(pin), id, refresh);
    httpd_resp_set_status(req, ok ? "200 OK" : "403 Forbidden");
    httpd_resp_sendstr(req, ok ? "ok\n" : "bad pin or data\n");
    return ESP_OK;
}

// URL の ? のあと (なければ空)
std::string queryOf(httpd_req_t* req)
{
    const size_t n = httpd_req_get_url_query_len(req);
    std::string q(n, '\0');
    if (n == 0 || httpd_req_get_url_query_str(req, q.data(), n + 1) != ESP_OK) return std::string();
    return q;
}

// 本体のページ: Client ID を入れて「Spotify にログイン」
esp_err_t onHello(httpd_req_t* req)
{
    auto* self = static_cast<Client*>(req->user_ctx);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const std::string page = self ? self->loginPage() : std::string("PulseDial");
    httpd_resp_send(req, page.data(), page.size());
    return ESP_OK;
}

esp_err_t onLogin(httpd_req_t* req)
{
    auto* self      = static_cast<Client*>(req->user_ctx);
    const auto q    = queryOf(req);
    char pin[8]     = {}, id[64] = {}, redirect[160] = {};
    formValue(q.c_str(), "pin", pin, sizeof(pin));
    formValue(q.c_str(), "client_id", id, sizeof(id));
    formValue(q.c_str(), "r", redirect, sizeof(redirect));
    std::string location;
    if (!self || !self->loginStart(atoi(pin), id, redirect, &location)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "Wrong PIN or Client ID. Scan the QR code on the StopWatch again.\n"
                                "PIN か Client ID がちがいます。StopWatch の QR をもう一度読んでください。\n");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

esp_err_t onCallback(httpd_req_t* req)
{
    auto* self   = static_cast<Client*>(req->user_ctx);
    const auto q = queryOf(req);
    std::string code(q.size() + 1, '\0');
    char state[48] = {}, error[64] = {};
    formValue(q.c_str(), "code", code.data(), code.size());
    formValue(q.c_str(), "state", state, sizeof(state));
    formValue(q.c_str(), "error", error, sizeof(error));
    const bool ok = self && self->loginCallback(code.c_str(), state, error);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static const char kPage[] =
        "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>PulseDial</title><style>body{font:17px/1.6 -apple-system,sans-serif;background:#0b0d12;color:#eef2f8;"
        "margin:0;padding:48px 20px;text-align:center}b{color:#2878ff}</style></head><body><h2>PulseDial</h2>"
        "<p id=m>Logging in&hellip; / ログインしています&hellip;</p><script>"
        "async function t(){try{const s=await (await fetch('/status',{cache:'no-store'})).text();"
        "if(s.startsWith('ok')){m.innerHTML='<b>Done!</b> Look at the StopWatch. You can close this page.<br>"
        "完了しました。StopWatch を見てください。このページは閉じて大丈夫です。';return}"
        "if(s.startsWith('error')){m.textContent=s;return}}catch(e){}setTimeout(t,1000)}t()</script></body></html>";
    static const char kBad[] =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font:17px/1.6 sans-serif;padding:48px 20px;text-align:center'>"
        "This login link is old or wrong. Scan the QR code on the StopWatch again.<br>"
        "このリンクは古いか、まちがっています。StopWatch の QR をもう一度読んでください。</body>";
    httpd_resp_sendstr(req, ok ? kPage : kBad);
    return ESP_OK;
}

esp_err_t onStatus(httpd_req_t* req)
{
    auto* self = static_cast<Client*>(req->user_ctx);
    char s[128] = "waiting";
    if (self) self->loginStatus(s, sizeof(s));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, s);
    return ESP_OK;
}

// PKCE: 合言葉の SHA-256 を base64url (= なし) に
std::string base64url(const uint8_t* d, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        if (i + 1 < n) out += t[(v >> 6) & 63];
        if (i + 2 < n) out += t[v & 63];
    }
    return out;
}

void randomChars(char* out, size_t n)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    for (size_t i = 0; i < n; ++i) out[i] = t[esp_random() % 66];
    out[n] = 0;
}

}  // namespace

// ============================================================================ 始める・止める

bool Client::start(int core)
{
    _lock    = xSemaphoreCreateMutex();
    _artLock = xSemaphoreCreateMutex();
    _done    = xSemaphoreCreateBinary();
    _cmds    = xQueueCreate(8, sizeof(Cmd));
    _buf     = static_cast<char*>(heap_caps_malloc(kBufCap, MALLOC_CAP_SPIRAM));
    _shown   = static_cast<uint16_t*>(heap_caps_malloc(kArtSide * kArtSide * 2, MALLOC_CAP_SPIRAM));
    _spare   = static_cast<uint16_t*>(heap_caps_malloc(kArtSide * kArtSide * 2, MALLOC_CAP_SPIRAM));
    _jobLock = xSemaphoreCreateMutex();
    _artDone = xSemaphoreCreateBinary();
    _w.buf   = static_cast<char*>(heap_caps_malloc(kArtBufCap, MALLOC_CAP_SPIRAM));
    _w.cap   = kArtBufCap;
    if (!_lock || !_artLock || !_done || !_cmds || !_buf || !_shown || !_spare || !_jobLock || !_artDone || !_w.buf) {
        stop();  // 作れた分を片付ける
        return false;
    }
    _bufCap = kBufCap;
    _snap   = Snapshot{};
    _running.store(true);
    // ジャケットの係: 通信の係と同じ優先度 (2 だと最初の暗号の接続作りがマイクの解析などに押されて 12 秒かかった)
    if (xTaskCreatePinnedToCore(artEntry, "spotify-art", 8192, this, 3, &_artTask, core) != pdPASS) {
        _artTask = nullptr;
        stop();
        return false;
    }
    if (xTaskCreatePinnedToCore(taskEntry, "spotify-net", 8192, this, 3, &_task, core) != pdPASS) {
        _task = nullptr;
        stop();  // 先に動き出したジャケットの係も止める
        return false;
    }
    return true;
}

void Client::stop()
{
    _running.store(false);
    if (_task) {
        xSemaphoreTake(_done, portMAX_DELAY);
        _task = nullptr;
    }
    if (_artTask) {
        xTaskNotifyGive(_artTask);
        xSemaphoreTake(_artDone, portMAX_DELAY);
        _artTask = nullptr;
    }
    if (_jobLock) vSemaphoreDelete(_jobLock);
    if (_artDone) vSemaphoreDelete(_artDone);
    _jobLock = _artDone = nullptr;
    heap_caps_free(_w.buf);
    _w = Sink{};
    for (auto& e : _artCache) {
        heap_caps_free(e.jpg);
        e = ArtCache{};
    }
    _hasShow = _hasPre = _hasDl = false;
    _next2.valid = false;
    _wantUri[0] = _shownArt[0] = _queueFor[0] = _uri[0] = 0;
    _pixOver = false;
    _readyArt[0][0] = _readyArt[1][0] = _pixUrl[0] = 0;
    _next.valid = false;
    if (_cmds) vQueueDelete(_cmds);
    if (_done) vSemaphoreDelete(_done);
    if (_lock) vSemaphoreDelete(_lock);
    if (_artLock) vSemaphoreDelete(_artLock);
    heap_caps_free(_buf);
    heap_caps_free(_shown);
    heap_caps_free(_spare);
    _cmds    = nullptr;
    _done    = nullptr;
    _lock    = nullptr;
    _artLock = nullptr;
    _buf     = nullptr;
    _shown = _spare = nullptr;
    _shownW = _shownH = _spareW = _spareH = 0;
    _spareUrl[0] = 0;
    // 鍵は消しておく (メモリに残さない)
    memset(_access, 0, sizeof(_access));
    memset(_refresh, 0, sizeof(_refresh));
}

void Client::taskEntry(void* arg)
{
    auto* self = static_cast<Client*>(arg);
    self->run();
    xSemaphoreGive(self->_done);
    vTaskDelete(nullptr);
}

bool Client::command(Cmd c)
{
    return _cmds && xQueueSend(_cmds, &c, 0) == pdTRUE;
}

void Client::snapshot(Snapshot& out)
{
    if (!_lock) {  // 始まっていない (始めるのに失敗した)
        out = Snapshot{};
        return;
    }
    xSemaphoreTake(_lock, portMAX_DELAY);
    out = _snap;
    xSemaphoreGive(_lock);
}

bool Client::lockArt(const uint16_t** px, int* w, int* h)
{
    if (!_artLock) {
        *px = nullptr;
        *w = *h = 0;
        return false;
    }
    xSemaphoreTake(_artLock, portMAX_DELAY);
    *px = _shownW > 0 ? _shown : nullptr;
    *w  = _shownW;
    *h  = _shownH;
    return true;
}

void Client::unlockArt()
{
    if (_artLock) xSemaphoreGive(_artLock);
}

void Client::setState(NetState s, const char* m0, const char* m1, const char* m2)
{
    xSemaphoreTake(_lock, portMAX_DELAY);
    _snap.state = s;
    copyStr(_snap.msg[0], sizeof(_snap.msg[0]), m0);
    copyStr(_snap.msg[1], sizeof(_snap.msg[1]), m1);
    copyStr(_snap.msg[2], sizeof(_snap.msg[2]), m2);
    xSemaphoreGive(_lock);
}

// ============================================================================ 鍵

bool Client::loadCredentials()
{
    Settings s(kNs, false);
    copyStr(_clientId, sizeof(_clientId), s.GetString("client_id").c_str());
    copyStr(_refresh, sizeof(_refresh), s.GetString("refresh").c_str());
    _accessUntilUs = 0;
    return _clientId[0] && _refresh[0];
}

void Client::saveRefresh(const char* refresh)
{
    copyStr(_refresh, sizeof(_refresh), refresh);
    Settings s(kNs, true);
    s.SetString("refresh", _refresh);
}

void Client::clearCredentials()
{
    Settings s(kNs, true);
    s.EraseKey("refresh");
    memset(_refresh, 0, sizeof(_refresh));
    memset(_access, 0, sizeof(_access));
    _accessUntilUs = 0;
}

bool Client::acceptSetup(int pin, const char* clientId, const char* refresh)
{
    if (pin != _pin || _pin == 0 || !clientId || !refresh) return false;
    const size_t idLen = strlen(clientId), rLen = strlen(refresh);
    if (idLen < 16 || idLen >= sizeof(_clientId) || rLen < 20 || rLen >= sizeof(_refresh)) return false;
    {
        Settings s(kNs, true);
        s.SetString("client_id", clientId);
        s.SetString("refresh", refresh);
    }
    mclog::tagInfo(kTag, "setup: credentials received");
    _credsArrived.store(true);
    return true;
}

// ---------------------------------------------------------------------------- QR ログイン

std::string Client::loginPage()
{
    char id[64];
    copyStr(id, sizeof(id), _clientId);  // 前に使った Client ID (あれば入れておく)
    for (char* p = id; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p))) *p = 0;
    }
    std::string page =
        "<!doctype html><html lang=en><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>PulseDial setup</title><style>"
        ":root{--bg:#0b0d12;--fg:#eef2f8;--mu:#9aa4b5;--card:#151922;--line:#252b38;--ac:#2878ff}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
        "font:16px/1.6 -apple-system,BlinkMacSystemFont,'Hiragino Sans',sans-serif}"
        "main{max-width:520px;margin:0 auto;padding:28px 16px 48px}h1{font-size:24px;margin:0 0 4px}"
        ".mu{color:var(--mu);font-size:14px}.card{background:var(--card);border:1px solid var(--line);"
        "border-radius:14px;padding:16px;margin-top:16px}label{font-weight:600;display:block;margin-bottom:6px}"
        "input{width:100%;font:16px monospace;padding:12px;border-radius:10px;border:1px solid var(--line);"
        "background:#0f1218;color:var(--fg)}button{width:100%;margin-top:14px;font:600 17px sans-serif;color:#fff;"
        "background:var(--ac);border:0;border-radius:999px;padding:14px}code{background:var(--line);"
        "border-radius:4px;padding:2px 5px;font-size:13px;word-break:break-all}ol{padding-left:20px;margin:6px 0}"
        "</style></head><body><main><h1>PulseDial</h1><div class=mu>Spotify login / Spotify ログイン</div>"
        "<div class=card><ol><li>At <a style=color:var(--ac) href='https://developer.spotify.com/dashboard'>"
        "developer.spotify.com/dashboard</a>, make an app (Web API) with this Redirect URI:<br><code>";
    page += kCallbackUrl;
    page +=
        "</code><br><span class=mu>開発者ページでアプリを作り、Redirect URI に上の URL を入れます。</span></li>"
        "<li>Copy its <b>Client ID</b> and paste it below.<br><span class=mu>アプリの Client ID を下に貼り付けます。</span>"
        "</li></ol></div><form class=card action=/login method=get><label for=c>Client ID</label>"
        "<input id=c name=client_id required minlength=16 maxlength=63 autocomplete=off autocapitalize=off "
        "spellcheck=false value='";
    page += id;
    page +=
        "'><input type=hidden name=pin id=p><input type=hidden name=r id=r>"
        "<button>Log in with Spotify / Spotify にログイン</button></form>"
        "<p class=mu>Spotify Premium is required. The key is stored only on the StopWatch.<br>"
        "Spotify Premium が必要です。鍵は StopWatch の中にだけ保存されます。</p></main><script>"
        "const q=new URLSearchParams(location.search);p.value=q.get('pin')||'';r.value=q.get('r')||'';"
        "</script></body></html>";
    return page;
}

bool Client::loginStart(int pin, const char* clientId, const char* redirect, std::string* location)
{
    if (pin != _pin || _pin == 0 || !clientId) return false;
    const size_t idLen = strlen(clientId);
    if (idLen < 16 || idLen >= sizeof(_loginClientId)) return false;
    for (const char* p = clientId; *p; ++p) {
        if (!isalnum(static_cast<unsigned char>(*p))) return false;
    }
    // 戻り先: ふつうは GitHub Pages のページ。試すときだけ、手元 (127.0.0.1) か https の別のページを使える
    const char* r = (redirect && *redirect) ? redirect : kCallbackUrl;
    if (strncmp(r, "https://", 8) != 0 && strncmp(r, "http://127.0.0.1:", 17) != 0 && strncmp(r, "http://localhost:", 17) != 0) {
        return false;
    }
    if (strlen(r) >= sizeof(_loginRedirect)) return false;
    // PKCE の合言葉と、戻り先がどの本体へ返すかの印 (state)
    char verifier[72], nonce[17];
    randomChars(verifier, 64);
    randomChars(nonce, 16);
    for (char* p = nonce; *p; ++p) {  // state は URL にそのまま入るので英数字だけに
        if (!isalnum(static_cast<unsigned char>(*p))) *p = 'x';
    }
    uint8_t hash[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(verifier), 64, hash, 0);
    const std::string challenge = base64url(hash, sizeof(hash));
    char state[48];
    snprintf(state, sizeof(state), "%s~%s", _ip, nonce);
    xSemaphoreTake(_lock, portMAX_DELAY);
    copyStr(_verifier, sizeof(_verifier), verifier);
    copyStr(_state, sizeof(_state), state);
    copyStr(_loginClientId, sizeof(_loginClientId), clientId);
    copyStr(_loginRedirect, sizeof(_loginRedirect), r);
    _codePending = false;
    copyStr(_loginMsg, sizeof(_loginMsg), "waiting");
    xSemaphoreGive(_lock);
    *location = std::string("https://accounts.spotify.com/authorize?client_id=") + urlEncode(clientId) +
                "&response_type=code&redirect_uri=" + urlEncode(r) + "&scope=" + urlEncode(kScopes) +
                "&code_challenge_method=S256&code_challenge=" + challenge + "&state=" + urlEncode(state);
    mclog::tagInfo(kTag, "login: sent to Spotify");
    return true;
}

bool Client::loginCallback(const char* code, const char* state, const char* error)
{
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool known = _state[0] && state && strcmp(state, _state) == 0;
    if (known && error && *error) {
        snprintf(_loginMsg, sizeof(_loginMsg), "error: Spotify said \"%s\". Scan the QR code again.", error);
    } else if (known && code && *code && strlen(code) < sizeof(_loginCode)) {
        copyStr(_loginCode, sizeof(_loginCode), code);
        _codePending = true;
        _state[0]    = 0;  // 1 回だけ使える
        copyStr(_loginMsg, sizeof(_loginMsg), "working");
    }
    xSemaphoreGive(_lock);
    if (known) mclog::tagInfo(kTag, "login: {}", (error && *error) ? "refused in the browser" : "got the code");
    return known;
}

void Client::loginStatus(char* out, size_t cap)
{
    xSemaphoreTake(_lock, portMAX_DELAY);
    copyStr(out, cap, _loginMsg[0] ? _loginMsg : "waiting");
    xSemaphoreGive(_lock);
}

// 通信の係から: 許可のコードを鍵 (refresh token) に換えて保存する
int Client::exchangeCode()
{
    char code[768], verifier[72], id[64], redirect[160];
    xSemaphoreTake(_lock, portMAX_DELAY);
    copyStr(code, sizeof(code), _loginCode);
    copyStr(verifier, sizeof(verifier), _verifier);
    copyStr(id, sizeof(id), _loginClientId);
    copyStr(redirect, sizeof(redirect), _loginRedirect);
    xSemaphoreGive(_lock);
    const std::string body = "grant_type=authorization_code&code=" + urlEncode(code) + "&redirect_uri=" + urlEncode(redirect) +
                             "&client_id=" + urlEncode(id) + "&code_verifier=" + urlEncode(verifier);
    memset(code, 0, sizeof(code));
    esp_http_client_config_t cfg = {};
    cfg.url                      = "https://accounts.spotify.com/api/token";
    cfg.method                   = HTTP_METHOD_POST;
    cfg.crt_bundle_attach        = esp_crt_bundle_attach;
    cfg.timeout_ms               = 10000;
    cfg.event_handler            = onHttpEvent;
    cfg.user_data                = this;
    cfg.buffer_size              = 2048;
    cfg.buffer_size_tx           = 1536;
    esp_http_client_handle_t c   = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, body.c_str(), body.size());
    _bufLen             = 0;
    const esp_err_t err = esp_http_client_perform(c);
    const int status    = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    _buf[std::min(_bufLen, _bufCap - 1)] = 0;
    if (status < 0) {
        mclog::tagWarn(kTag, "login: network error {}", static_cast<int>(err));
        return -1;
    }
    JsonDocument doc;
    deserializeJson(doc, _buf, _bufLen);
    const char* refresh = doc["refresh_token"] | "";
    if (status != 200 || !refresh[0] || strlen(refresh) >= sizeof(_refresh)) {
        mclog::tagWarn(kTag, "login: http {} {}", status, doc["error"] | "");
        return 0;
    }
    {
        Settings s(kNs, true);
        s.SetString("client_id", id);
        s.SetString("refresh", refresh);
    }
    mclog::tagInfo(kTag, "login: key received");
    _stopServerAtUs = nowUs() + 4000000;  // スマホが「完了」を見られるように 4 秒だけ残す
    _credsArrived.store(true);
    return 1;
}

// 1 = 取れた、0 = 鍵が使えない (ログインし直し)、-1 = つながらない
int Client::refreshToken()
{
    const std::string body =
        "grant_type=refresh_token&refresh_token=" + urlEncode(_refresh) + "&client_id=" + urlEncode(_clientId);
    esp_http_client_config_t cfg = {};
    cfg.url                      = "https://accounts.spotify.com/api/token";
    cfg.method                   = HTTP_METHOD_POST;
    cfg.crt_bundle_attach        = esp_crt_bundle_attach;
    cfg.timeout_ms               = 8000;
    cfg.event_handler            = onHttpEvent;
    cfg.user_data                = this;
    cfg.buffer_size              = 2048;
    cfg.buffer_size_tx           = 1024;
    esp_http_client_handle_t c   = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, body.c_str(), body.size());
    _bufLen             = 0;
    const esp_err_t err = esp_http_client_perform(c);
    const int status    = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    _buf[std::min(_bufLen, _bufCap - 1)] = 0;
    if (status < 0) {
        mclog::tagWarn(kTag, "token: network error {}", static_cast<int>(err));
        return -1;
    }
    JsonDocument doc;
    deserializeJson(doc, _buf, _bufLen);
    if (status != 200) {
        const char* e = doc["error"] | "";
        mclog::tagWarn(kTag, "token: http {} {}", status, e);
        return (status == 400 || status == 401) && (strcmp(e, "invalid_grant") == 0 || strcmp(e, "invalid_client") == 0)
                   ? 0
                   : -1;
    }
    const char* access = doc["access_token"] | "";
    const int expires  = doc["expires_in"] | 3600;
    const char* fresh  = doc["refresh_token"] | "";
    if (!access[0]) return -1;
    copyStr(_access, sizeof(_access), access);
    _accessUntilUs = nowUs() + static_cast<int64_t>(expires) * 1000000;
    if (fresh[0] && strcmp(fresh, _refresh) != 0) saveRefresh(fresh);  // 新しい鍵が来たら、それを使う
    mclog::tagInfo(kTag, "token ok ({}s)", expires);
    return 1;
}

// ============================================================================ Mac からの受け取り

void Client::startSetupServer()
{
    if (_server || nowUs() < _setupRetryUs) return;
    _setupRetryUs         = nowUs() + 5000000;  // 作れなかったら 5 秒おいて作り直す
    _pin                  = 1000 + static_cast<int>(esp_random() % 9000);
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = kSetupPort;
    cfg.ctrl_port         = 32790;  // 公式の設定用サーバーとぶつからないように
    cfg.stack_size        = 6144;
    cfg.max_uri_handlers  = 8;
    httpd_handle_t h      = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        mclog::tagError(kTag, "setup server failed");
        return;
    }
    httpd_uri_t post = {};
    post.uri         = "/setup";
    post.method      = HTTP_POST;
    post.handler     = onSetup;
    post.user_ctx    = this;
    httpd_uri_t get = {};
    get.uri         = "/";
    get.method      = HTTP_GET;
    get.handler     = onHello;
    get.user_ctx    = this;
    httpd_uri_t login = {}, cb = {}, st = {};
    login.uri = "/login", login.method = HTTP_GET, login.handler = onLogin, login.user_ctx = this;
    cb.uri = "/callback", cb.method = HTTP_GET, cb.handler = onCallback, cb.user_ctx = this;
    st.uri = "/status", st.method = HTTP_GET, st.handler = onStatus, st.user_ctx = this;
    if (httpd_register_uri_handler(h, &post) != ESP_OK || httpd_register_uri_handler(h, &get) != ESP_OK ||
        httpd_register_uri_handler(h, &login) != ESP_OK || httpd_register_uri_handler(h, &cb) != ESP_OK ||
        httpd_register_uri_handler(h, &st) != ESP_OK) {
        mclog::tagError(kTag, "setup server: register failed");
        httpd_stop(h);
        return;
    }
    _server = h;
    mclog::tagInfo(kTag, "setup: http://{}:{}/  pin {}", _ip, kSetupPort, _pin);
}

void Client::stopSetupServer()
{
    if (!_server) return;
    httpd_stop(static_cast<httpd_handle_t>(_server));
    _server = nullptr;
    _pin    = 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    _verifier[0] = _state[0] = _loginCode[0] = _loginMsg[0] = 0;
    _codePending = false;
    xSemaphoreGive(_lock);
}

// ============================================================================ API

void Client::httpData(const char* data, int len)
{
    if (!_buf || len <= 0) return;
    const size_t n = std::min<size_t>(len, _bufCap - 1 - _bufLen);
    memcpy(_buf + _bufLen, data, n);
    _bufLen += n;
}

void Client::httpHeader(const char* key, const char* value)
{
    if (key && value && strcasecmp(key, "Retry-After") == 0) {
        _retryAfterUs = nowUs() + static_cast<int64_t>(std::max(1, atoi(value))) * 1000000;
        _rateLimited  = true;
    }
}

// api.spotify.com へ。HTTP の状態を返す (つながらなければ -1)。応答は _buf (終わりに 0)
int Client::api(const char* method, const char* path, const char* query, char*, size_t, size_t*)
{
    if (!_api) {
        esp_http_client_config_t cfg = {};
        cfg.url                      = kApi;
        cfg.crt_bundle_attach        = esp_crt_bundle_attach;
        cfg.timeout_ms               = 8000;
        cfg.event_handler            = onHttpEvent;
        cfg.user_data                = this;
        cfg.buffer_size              = 4096;
        cfg.buffer_size_tx           = 1536;
        cfg.keep_alive_enable        = true;
        _api                         = esp_http_client_init(&cfg);
        if (!_api) return -1;
    }
    auto* c = static_cast<esp_http_client_handle_t>(_api);
    std::string url = std::string(kApi) + path;
    if (query && *query) url += std::string("?") + query;
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (!strcmp(method, "PUT")) m = HTTP_METHOD_PUT;
    if (!strcmp(method, "POST")) m = HTTP_METHOD_POST;
    if (!strcmp(method, "DELETE")) m = HTTP_METHOD_DELETE;
    const std::string auth = std::string("Bearer ") + _access;

    int status = -1;
    for (int attempt = 0; attempt < 2 && status < 0; ++attempt) {  // つなぎっぱなしが切れていたら、もう 1 回
        esp_http_client_set_url(c, url.c_str());
        esp_http_client_set_method(c, m);
        esp_http_client_set_header(c, "Authorization", auth.c_str());
        esp_http_client_set_post_field(c, nullptr, 0);
        _bufLen             = 0;
        const esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK) {
            status = esp_http_client_get_status_code(c);
        } else {
            mclog::tagWarn(kTag, "{} {}: {}", method, path, esp_err_to_name(err));
            esp_http_client_close(c);
        }
    }
    _buf[std::min(_bufLen, _bufCap - 1)] = 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    _snap.lastError = status;
    xSemaphoreGive(_lock);
    if (status == 401) _accessUntilUs = 0;  // 鍵の期限切れ: 次で取り直す
    return status;
}

void Client::poll()
{
    const int st    = api("GET", "/v1/me/player", "market=from_token&additional_types=episode");
    const int64_t t = nowUs();
    if (st == 204 || (st == 200 && _bufLen == 0)) {  // どこでも再生していない
        xSemaphoreTake(_lock, portMAX_DELAY);
        _snap.hasPlayback = false;
        _snap.playing     = false;
        _snap.state       = NetState::Ready;
        xSemaphoreGive(_lock);
        _failures   = 0;
        _nextPollUs = t + kPollIdleUs;
        return;
    }
    if (st != 200) {
        if (st == 429) {
            mclog::tagWarn(kTag, "rate limited");
        } else if (st != 401) {
            ++_failures;
        }
        _nextPollUs = t + (st == 401 ? 200000 : std::min<int64_t>(30000000, 2000000LL << std::min(_failures, 4)));
        if (_failures >= 3) setState(NetState::NetError, "Spotify", "Can't connect", "Retrying…");
        return;
    }
    _failures = 0;

    JsonDocument filter;
    filter["is_playing"]                   = true;
    filter["progress_ms"]                  = true;
    filter["device"]["volume_percent"]     = true;
    filter["device"]["name"]               = true;
    filter["device"]["supports_volume"]    = true;
    JsonObject item                        = filter["item"].to<JsonObject>();
    item["name"]                           = true;
    item["uri"]                            = true;
    item["type"]                           = true;
    item["duration_ms"]                    = true;
    item["artists"][0]["name"]             = true;
    item["album"]["images"][0]["url"]      = true;
    item["album"]["images"][0]["width"]    = true;
    item["images"][0]["url"]               = true;
    item["images"][0]["width"]             = true;
    item["show"]["name"]                   = true;
    JsonDocument doc;
    if (deserializeJson(doc, _buf, _bufLen, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
        mclog::tagWarn(kTag, "player: bad json ({} bytes)", _bufLen);
        _nextPollUs = t + kPollPausedUs;
        return;
    }
    JsonObject it      = doc["item"];
    const bool playing = doc["is_playing"] | false;
    const char* uri    = it["uri"] | "";
    const bool isTrack = strncmp(uri, "spotify:track:", 14) == 0;
    if (_optimistic) {
        if (t < _expectUntilUs && strcmp(uri, _prevUri) == 0) {  // まだ前の曲: 先に出した次の曲のまま、少しあとで見直す
            _nextPollUs = t + kPollExpectUs;
            return;
        }
        _optimistic = false;
    }

    const std::string artist = joinArtists(it);
    const std::string artUrl = pickArt(it);
    const bool changed = strcmp(uri, _uri) != 0;
    xSemaphoreTake(_lock, portMAX_DELAY);
    _snap.state       = NetState::Ready;
    _snap.hasPlayback = !it.isNull();
    _snap.playing     = playing;
    _snap.progressMs  = doc["progress_ms"] | 0;
    _snap.durationMs  = it["duration_ms"] | 0;
    _snap.fetchedUs   = t;
    _snap.volume      = doc["device"]["volume_percent"].isNull() ? -1 : doc["device"]["volume_percent"].as<int>();
    _snap.supportsVolume = doc["device"]["supports_volume"] | false;
    copyStr(_snap.device, sizeof(_snap.device), doc["device"]["name"] | "");
    xSemaphoreGive(_lock);

    // 曲が変わったら: 曲名はすぐ出す。ジャケットとテンポは裏の係が取る (その間も操作は効く)
    if (changed) {
        // 先取りしていた「次の曲」を合わせる: 次へ進んだのなら 1 つ繰り上げ、前へ戻った・とんだのなら捨てる (順番を聞き直すまで先に出さない)
        if (_next.valid && strcmp(_next.uri, uri) == 0) {
            _next        = _next2;
            _next2.valid = false;
        } else {
            _next.valid = _next2.valid = false;
        }
        const std::string title = it["name"] | "";
        publishTrack(uri, title.c_str(), artist.c_str(), isTrack, artUrl.c_str());
        if (isTrack) {
            const bool liked = fetchLiked();
            xSemaphoreTake(_lock, portMAX_DELAY);
            _snap.liked = liked;
            xSemaphoreGive(_lock);
        }
    }
    // 次に流れる曲を聞いて、ジャケットとテンポを前もって取っておく (曲ごとに 1 回)
    if (_snap.hasPlayback && *_uri && strcmp(_queueFor, _uri) != 0) fetchQueue();

    // 次に見る時刻: 再生中は 2 秒ごと (曲の終わりが近ければ、終わる少しあと)
    int64_t next = playing ? kPollPlayingUs : (_snap.hasPlayback ? kPollPausedUs : kPollIdleUs);
    if (playing && _snap.durationMs > _snap.progressMs) {
        const int64_t left = static_cast<int64_t>(_snap.durationMs - _snap.progressMs) * 1000 + 400000;
        next               = std::min(next, std::max<int64_t>(left, 500000));
    }
    if (nowUs() < _expectUntilUs) next = std::min(next, kPollExpectUs);  // 曲送りのあとは、こまめに
    _nextPollUs = nowUs() + next;
}

bool Client::fetchLiked()
{
    const std::string q = "uris=" + urlEncode(_uri);
    const int st        = api("GET", "/v1/me/library/contains", q.c_str());
    if (st != 200) return false;
    const char* p = strchr(_buf, '[');
    return p && strncmp(p + 1, "true", 4) == 0;
}

void Client::publishTrack(const char* uri, const char* title, const char* artist, bool isTrack, const char* artUrl)
{
    copyStr(_uri, sizeof(_uri), uri);
    xSemaphoreTake(_lock, portMAX_DELAY);
    copyStr(_snap.title, sizeof(_snap.title), title);
    copyStr(_snap.artist, sizeof(_snap.artist), artist);
    _snap.isTrack = isTrack;
    _snap.liked   = false;
    copyStr(_snap.trackId, sizeof(_snap.trackId), isTrack ? uri + 14 : "");
    // テンポ: 覚えていればすぐ (拍の位置も)。なければ係が ReccoBeats に聞くか、描く側がマイクで聞く
    TempoInfo ti;
    if (isTrack && _mem.findTempo(uri + 14, &ti) && ti.bpm > 0) {
        _snap.bpm          = ti.bpm;
        _snap.energy       = ti.energy;
        _snap.beatOffsetMs = ti.offsetMs;
        _snap.bpmSource    = ti.source;
    } else {
        _snap.bpm          = 0;
        _snap.energy       = 0.6f;
        _snap.beatOffsetMs = 0;
        _snap.bpmSource    = kTempoNone;
    }
    ++_snap.trackSeq;
    _snap.artPending = true;  // 本物のジャケットが出るまでマイクは待つ
    xSemaphoreGive(_lock);
    // 覚えていたドット絵を、ジャケットの係を待たずにすぐ出す (係は先読みや ReccoBeats の途中のことがある)。
    // 本物がもう展開ずみ (出ている・予備にある) なら、すぐ入れかわるので出さない (ちらつかないように)
    bool ready = false;
    if (_jobLock && artUrl && *artUrl) {
        xSemaphoreTake(_jobLock, portMAX_DELAY);
        ready = strcmp(_readyArt[0], artUrl) == 0 || strcmp(_readyArt[1], artUrl) == 0;
        xSemaphoreGive(_jobLock);
    }
    if (!ready) showPixFor(artUrl);
    requestArt(uri, artUrl, true);
}

bool Client::showPixFor(const char* artUrl)
{
    if (!artUrl || !*artUrl) return false;
    spviz::PixelArt pa;
    if (!_mem.findPix(artUrl, &pa)) return false;
    xSemaphoreTake(_lock, portMAX_DELAY);
    const bool already = strcmp(_pixUrl, artUrl) == 0 && _pixOver.load();
    if (!already) {
        _pixShow = pa;
        ++_snap.pixSeq;
        copyStr(_pixUrl, sizeof(_pixUrl), artUrl);
    }
    xSemaphoreGive(_lock);
    _pixOver.store(true);
    return true;
}

void Client::markReady()
{
    xSemaphoreTake(_jobLock, portMAX_DELAY);
    copyStr(_readyArt[0], sizeof(_readyArt[0]), _shownArt);
    copyStr(_readyArt[1], sizeof(_readyArt[1]), _spareW > 0 ? _spareUrl : "");
    xSemaphoreGive(_jobLock);
}

void Client::fetchQueue()
{
    const int st = api("GET", "/v1/me/player/queue", nullptr);
    if (st != 200) {
        if (st > 0) copyStr(_queueFor, sizeof(_queueFor), _uri);  // 聞けない (順番がない など): この曲ではもう聞かない
        return;
    }
    JsonDocument filter;
    filter["currently_playing"]["uri"] = true;
    JsonObject q                       = filter["queue"].add<JsonObject>();
    q["uri"]                           = true;
    q["name"]                          = true;
    q["duration_ms"]                   = true;
    q["artists"][0]["name"]            = true;
    q["show"]["name"]                  = true;
    q["album"]["images"][0]["url"]     = true;
    q["album"]["images"][0]["width"]   = true;
    q["images"][0]["url"]              = true;
    q["images"][0]["width"]            = true;
    JsonDocument doc;
    if (deserializeJson(doc, _buf, _bufLen, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
        copyStr(_queueFor, sizeof(_queueFor), _uri);
        return;
    }
    if (strcmp(doc["currently_playing"]["uri"] | "", _uri) != 0) return;  // Spotify 側がまだ前の状態: 次の機会に
    copyStr(_queueFor, sizeof(_queueFor), _uri);
    auto fill = [&](Next& dst, JsonObject n) {
        dst.valid = false;
        if (n.isNull() || !*(n["uri"] | "")) return;
        copyStr(dst.uri, sizeof(dst.uri), n["uri"] | "");
        copyStr(dst.title, sizeof(dst.title), n["name"] | "");
        copyStr(dst.artist, sizeof(dst.artist), joinArtists(n).c_str());
        copyStr(dst.art, sizeof(dst.art), pickArt(n).c_str());
        dst.durMs = n["duration_ms"] | 0;
        dst.valid = true;
    };
    fill(_next, doc["queue"][0]);
    fill(_next2, doc["queue"][1]);
    if (_next.valid) requestArt(_next.uri, _next.art, false);        // 次の曲: 読んでおく
    if (_next2.valid) requestDownload(_next2.uri, _next2.art);        // その次: 取っておく
}

// ============================================================================ ジャケットの係

void Client::requestArt(const char* uri, const char* artUrl, bool show)
{
    xSemaphoreTake(_jobLock, portMAX_DELAY);
    Job& j = show ? _jobShow : _jobPre;
    copyStr(j.uri, sizeof(j.uri), uri);
    copyStr(j.art, sizeof(j.art), artUrl);
    if (show) {
        _hasShow = true;
        copyStr(_wantUri, sizeof(_wantUri), uri);
    } else {
        _hasPre = true;
    }
    xSemaphoreGive(_jobLock);
    if (_artTask) xTaskNotifyGive(_artTask);
}

void Client::requestDownload(const char* uri, const char* artUrl)
{
    xSemaphoreTake(_jobLock, portMAX_DELAY);
    copyStr(_jobDl.uri, sizeof(_jobDl.uri), uri);
    copyStr(_jobDl.art, sizeof(_jobDl.art), artUrl);
    _hasDl = true;
    xSemaphoreGive(_jobLock);
    if (_artTask) xTaskNotifyGive(_artTask);
}

bool Client::stillWanted(const char* uri)
{
    xSemaphoreTake(_jobLock, portMAX_DELAY);
    const bool r = strcmp(_wantUri, uri) == 0;
    xSemaphoreGive(_jobLock);
    return r;
}

void Client::artEntry(void* arg)
{
    auto* self = static_cast<Client*>(arg);
    self->artRun();
    xSemaphoreGive(self->_artDone);
    vTaskDelete(nullptr);
}

// つなぎっぱなしの GET (応答は _w)。切れていたら、もう 1 回
int Client::getW(void** handle, const char* url, int timeoutMs, const char* accept)
{
    int status = -1;
    for (int attempt = 0; attempt < 2 && status < 0; ++attempt) {
        if (!*handle) {
            esp_http_client_config_t cfg = {};
            cfg.url                      = url;
            cfg.crt_bundle_attach        = esp_crt_bundle_attach;
            cfg.timeout_ms               = timeoutMs;
            cfg.event_handler            = onSinkEvent;
            cfg.user_data                = &_w;
            cfg.buffer_size              = 4096;
            cfg.keep_alive_enable        = true;
            *handle                      = esp_http_client_init(&cfg);
            if (!*handle) return -1;
        }
        auto* c = static_cast<esp_http_client_handle_t>(*handle);
        esp_http_client_set_url(c, url);
        esp_http_client_set_method(c, HTTP_METHOD_GET);
        if (accept) esp_http_client_set_header(c, "Accept", accept);
        _w.len              = 0;
        const esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK) {
            status = esp_http_client_get_status_code(c);
        } else {
            mclog::tagWarn(kTag, "get: {}", esp_err_to_name(err));
            esp_http_client_cleanup(c);
            *handle = nullptr;
        }
    }
    _w.buf[std::min(_w.len, _w.cap - 1)] = 0;
    return status;
}

const uint8_t* Client::loadJpeg(const char* url, size_t* len)
{
    for (auto& e : _artCache) {
        if (e.jpg && strcmp(e.url, url) == 0) {
            e.stamp = ++_stamp;
            *len    = e.len;
            return e.jpg;
        }
    }
    const int64_t t0 = nowUs();
    const int st     = getW(&_imgHttp, url, 8000, nullptr);
    if (st != 200 || _w.len == 0 || _w.len >= _w.cap - 1) {
        mclog::tagWarn(kTag, "art: http {} ({} bytes, room {})", st, _w.len, _w.cap);
        return nullptr;
    }
    // 合計が 480KB を超えないように、古いものから捨てる (PSRAM の空きを守る)
    for (;;) {
        size_t total = _w.len;
        ArtCache* old = nullptr;
        for (auto& e : _artCache) {
            if (!e.jpg) continue;
            total += e.len;
            if (!old || e.stamp < old->stamp) old = &e;
        }
        if (total <= 480 * 1024 || !old) break;
        heap_caps_free(old->jpg);
        *old = ArtCache{};
    }
    // いちばん古いものと入れかえて覚えておく
    ArtCache* slot = &_artCache[0];
    for (auto& e : _artCache) {
        if (!e.jpg) {
            slot = &e;
            break;
        }
        if (e.stamp < slot->stamp) slot = &e;
    }
    heap_caps_free(slot->jpg);
    *slot     = ArtCache{};
    slot->jpg = static_cast<uint8_t*>(heap_caps_malloc(_w.len, MALLOC_CAP_SPIRAM));
    if (!slot->jpg) {
        *len = _w.len;
        return reinterpret_cast<const uint8_t*>(_w.buf);
    }
    memcpy(slot->jpg, _w.buf, _w.len);
    copyStr(slot->url, sizeof(slot->url), url);
    slot->len   = _w.len;
    slot->stamp = ++_stamp;
    *len        = slot->len;
    mclog::tagInfo(kTag, "art got {} bytes in {}ms", slot->len, (nowUs() - t0) / 1000);
    return slot->jpg;
}

bool Client::cachedJpeg(const char* url) const
{
    for (const auto& e : _artCache) {
        if (e.jpg && strcmp(e.url, url) == 0) return true;
    }
    return false;
}

bool Client::showPendingOther(const char* url)
{
    xSemaphoreTake(_jobLock, portMAX_DELAY);
    const bool r = _hasShow && strcmp(_jobShow.art, url) != 0;
    xSemaphoreGive(_jobLock);
    return r;
}

// 予備の入れ物へ読む (画面は読まないので、ロックは要らない)。urgent = 画面に出す絵: このあいだ優先度を上げる。
// cancelUnlessUrl があれば、別のジャケットを出す頼みが来たら途中でやめる (先取りのとき)
bool Client::decodeArt(const uint8_t* jpg, size_t len, bool urgent, const char* cancelUnlessUrl)
{
    int jw = 0, jh = 0;
    bool prog = false;
    _spareUrl[0] = 0;
    _spareW = _spareH = 0;
    markReady();  // 予備は書きかわる
    if (!jpegSize(jpg, len, &jw, &jh, &prog)) return false;
    if (prog) {
        mclog::tagWarn(kTag, "art: progressive jpeg {}x{} (not supported)", jw, jh);
        return false;
    }
    const int64_t t0 = nowUs();
    if (urgent) _decoding.store(true);
    // 優先度は係のまま (3): マイクの解析より先に。先に読むときも、曲名を見て押すまでに間に合わせる
    _cancelUnless = cancelUnlessUrl;
    int w = 0, hh = 0;
    // 16 行ごとに呼ばれる: 1ms 休んで、このコアの待機タスクにも回す (固まり見張りが鳴った)。先に読むときは、要らなくなったらやめる
    const CancelFn cancel = [](void* ctx) {
        vTaskDelay(1);
        auto* self = static_cast<Client*>(ctx);
        return self->_cancelUnless && self->showPendingOther(self->_cancelUnless);
    };
    const bool ok = decodeJpegRom(jpg, len, _spare, kArtSide, &w, &hh, cancel, this);
    _cancelUnless = nullptr;
    if (urgent) _decoding.store(false);
    if (ok) {
        _spareW = w;
        _spareH = hh;
    }
    mclog::tagInfo(kTag, "art jpeg {}x{} -> {}x{} ({} bytes) {} in {}ms{}", jw, jh, w, hh, len, ok ? "ok" : "stopped/failed",
                   (nowUs() - t0) / 1000, urgent ? "" : " (ahead)");
    return ok;
}

// 曲のテンポと激しさ: ReccoBeats (鍵なしの公開 API)。送るのは Spotify の曲 ID だけ。見つからない曲も覚える
bool Client::tempoFor(const char* trackId, float* bpm, float* energy)
{
    *bpm    = 0;
    *energy = 0.6f;
    if (!trackId || !*trackId) return false;
    TempoInfo m;
    const bool have = _mem.findTempo(trackId, &m);
    if (have && m.reccoChecked) {  // もう聞いた (なかった曲も)。マイクで覚えた値があればそれ
        *bpm    = m.bpm;
        *energy = m.energy;
        return *bpm > 0;
    }
    const std::string url = std::string("https://api.reccobeats.com/v1/audio-features?ids=") + urlEncode(trackId);
    const int st          = getW(&_rbHttp, url.c_str(), 6000, "application/json");
    if (st != 200) {
        mclog::tagWarn(kTag, "tempo: http {}", st);
        if (have) {
            *bpm    = m.bpm;
            *energy = m.energy;
        }
        return *bpm > 0;  // つながらなかった: 覚えない (次の機会にまた聞く)
    }
    float rb = 0, en = 0.6f;
    JsonDocument filter;
    filter["content"][0]["tempo"]  = true;
    filter["content"][0]["energy"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, _w.buf, _w.len, DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
        JsonObject f = doc["content"][0];
        if (!f.isNull()) {
            rb = f["tempo"] | 0.0f;
            en = f["energy"] | 0.6f;
        }
    }
    TempoInfo n     = have ? m : TempoInfo{};
    n.reccoChecked  = true;
    if (rb > 0) {
        mclog::tagInfo(kTag, "tempo {:.1f} bpm, energy {:.2f}", rb, en);
        if (!(have && m.bpm > 0 && fabsf(m.bpm - rb) < rb * 0.04f)) n.offsetMs = 0;  // マイクで覚えた拍の位置は、同じテンポなら残す
        n.bpm    = rb;
        n.energy = en;
        n.source = kTempoRecco;
    } else {
        mclog::tagInfo(kTag, "tempo: not found{}", (have && m.bpm > 0) ? " (using remembered)" : "");
    }
    _mem.putTempo(trackId, n);
    *bpm    = n.bpm;
    *energy = n.energy;
    return *bpm > 0;
}

void Client::learnTempo(const char* trackId, float bpm, float offsetMs, uint8_t source)
{
    if (!trackId || !*trackId || bpm <= 0) return;
    TempoInfo n;
    _mem.findTempo(trackId, &n);
    n.bpm      = bpm;
    n.offsetMs = offsetMs;
    n.source   = source;
    _mem.putTempo(trackId, n);
    mclog::tagInfo(kTag, "learned tempo {:.1f} bpm, beat at {:.0f}ms ({})", bpm, offsetMs, source == kTempoMic ? "mic" : "fix");
    xSemaphoreTake(_lock, portMAX_DELAY);
    if (strcmp(_snap.trackId, trackId) == 0) {
        _snap.bpm          = bpm;
        _snap.beatOffsetMs = offsetMs;
        _snap.bpmSource    = source;
    }
    xSemaphoreGive(_lock);
    if (_artTask) xTaskNotifyGive(_artTask);  // 係が書く
}

// 予備の入れ物に読んだ絵から、ドット絵を作って覚える (まだなら)
void Client::rememberPix(const char* artUrl)
{
    if (_spareW <= 0 || _mem.hasPix(artUrl)) return;
    spviz::PixelArt pa;
    spviz::makePixelArt(_spare, _spareW, _spareH, &pa);
    _mem.putPix(artUrl, pa);
}

bool Client::lockPix(spviz::PixelArt* out)
{
    xSemaphoreTake(_lock, portMAX_DELAY);
    *out = _pixShow;
    xSemaphoreGive(_lock);
    return true;
}

void Client::artRun()
{
    _mem.begin();  // フラッシュに覚えたテンポとドット絵を読む
    while (_running.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        _mem.flush();  // 覚えたものを書く (変わった枠だけ)
        Job j;
        bool show = false, pre = false, dl = false;
        xSemaphoreTake(_jobLock, portMAX_DELAY);
        if (_hasShow) {
            j        = _jobShow;
            _hasShow = false;
            show     = true;
        } else if (_hasPre) {
            j       = _jobPre;
            _hasPre = false;
            pre     = true;
        } else if (_hasDl) {
            j      = _jobDl;
            _hasDl = false;
            dl     = true;
        }
        xSemaphoreGive(_jobLock);
        if (!show && !pre && !dl) continue;
        const bool isTrack = strncmp(j.uri, "spotify:track:", 14) == 0;
        const bool isTrackJ = strncmp(j.uri, "spotify:track:", 14) == 0;
        if (dl) {  // 2 曲先: ジャケットとテンポを取っておくだけ (曲送りで次になったら、読むだけで出せる)
            size_t len = 0;
            if (*j.art) loadJpeg(j.art, &len);
            float b, e;
            if (isTrackJ) tempoFor(j.uri + 14, &b, &e);
            continue;
        }
        if (pre) {
            // 次の曲を先に取って読んでおく (曲名を見てから押すので、すぐ始める)。画面に出す頼みが来たら途中でやめる
            if (*j.art && strcmp(j.art, _shownArt) != 0 && strcmp(j.art, _spareUrl) != 0) {
                size_t len         = 0;
                const uint8_t* jpg = loadJpeg(j.art, &len);
                if (jpg && !showPendingOther(j.art)) {
                    if (decodeArt(jpg, len, false, j.art)) {
                        copyStr(_spareUrl, sizeof(_spareUrl), j.art);
                        markReady();
                        rememberPix(j.art);
                    }
                }
                // 先に読む間も画面のための頼みは先にする (やめたときは、次の機会に読み直す)
            }
            float b, e;
            if (isTrack && !showPendingOther(j.art)) tempoFor(j.uri + 14, &b, &e);
            continue;
        }
        // 画面に出す曲: ジャケット → テンポ。途中で曲が変わったら、出さずに次の頼みへ
        if (strcmp(j.art, _shownArt) != 0) {
            bool ok = false;
            if (*j.art) {
                if (strcmp(j.art, _spareUrl) == 0 && _spareW > 0) {  // 先に読んであった (次の曲・ひとつ前の曲)
                    ok = true;
                } else {
                    if (stillWanted(j.uri) && showPixFor(j.art)) {  // 覚えていたドット絵 (通信の係がもう出していれば、そのまま)
                        mclog::tagInfo(kTag, "art: showing remembered pixel art first");
                    }
                    size_t len         = 0;
                    const uint8_t* jpg = loadJpeg(j.art, &len);
                    if (!jpg && stillWanted(j.uri)) {  // 取れなかった: 1 秒おいて、もう 1 回
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        if (stillWanted(j.uri)) jpg = loadJpeg(j.art, &len);
                    }
                    if (!stillWanted(j.uri)) continue;
                    if (jpg && decodeArt(jpg, len, true, nullptr)) {
                        copyStr(_spareUrl, sizeof(_spareUrl), j.art);
                        markReady();
                        rememberPix(j.art);
                        ok = true;
                    }
                }
            }
            if (!stillWanted(j.uri)) continue;
            // 入れかえる: 今の絵は予備に残る (すぐ前の曲に戻ったとき、読み直さずに出せる)
            xSemaphoreTake(_artLock, portMAX_DELAY);
            if (ok) {
                std::swap(_shown, _spare);
                std::swap(_shownW, _spareW);
                std::swap(_shownH, _spareH);
            } else {
                _shownW = _shownH = 0;
            }
            xSemaphoreGive(_artLock);
            if (ok) {
                copyStr(_spareUrl, sizeof(_spareUrl), _shownArt);  // 予備には、さっきまで出ていた絵
                if (!_spareUrl[0]) _spareW = _spareH = 0;
            }
            copyStr(_shownArt, sizeof(_shownArt), ok ? j.art : "");
            markReady();
            _pixOver = false;
            const bool wanted = stillWanted(j.uri);
            xSemaphoreTake(_lock, portMAX_DELAY);
            ++_snap.artSeq;
            _pixUrl[0] = 0;
            if (wanted) _snap.artPending = false;
            xSemaphoreGive(_lock);
        } else {
            // 絵はもう出ている。その上にドット絵が出たまま (前へ → すぐ次へ で元の曲に戻った・通信の係が先に出した) なら出し直す
            const bool over = _pixOver.exchange(false);
            if (over) mclog::tagInfo(kTag, "art: back to the shown art, replacing pixel art");
            const bool wanted = stillWanted(j.uri);
            xSemaphoreTake(_lock, portMAX_DELAY);
            if (over) {
                ++_snap.artSeq;
                _pixUrl[0] = 0;
            }
            if (wanted) _snap.artPending = false;
            xSemaphoreGive(_lock);
        }
        if (isTrack && stillWanted(j.uri)) {
            float bpm = 0, energy = 0.6f;
            tempoFor(j.uri + 14, &bpm, &energy);
            TempoInfo ti;
            if (stillWanted(j.uri) && _mem.findTempo(j.uri + 14, &ti)) {
                xSemaphoreTake(_lock, portMAX_DELAY);
                _snap.bpm          = ti.bpm;
                _snap.energy       = ti.energy;
                _snap.beatOffsetMs = ti.offsetMs;
                _snap.bpmSource    = ti.source;
                xSemaphoreGive(_lock);
            }
        }
    }
    for (void** h : {&_imgHttp, &_rbHttp}) {
        if (*h) esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(*h));
        *h = nullptr;
    }
    _mem.end();
}

void Client::doCommand(Cmd c)
{
    static const char* const kNames[] = {"play", "pause", "next", "prev", "like", "unlike", "vol+", "vol-", "logout"};
    mclog::tagInfo(kTag, "command {}", kNames[static_cast<int>(c)]);
    int st          = -1;
    int64_t pollIn  = 300000;
    switch (c) {
        case Cmd::Play: st = api("PUT", "/v1/me/player/play", nullptr); break;
        case Cmd::Pause: st = api("PUT", "/v1/me/player/pause", nullptr); break;
        case Cmd::Next:
            st     = api("POST", "/v1/me/player/next", nullptr);
            pollIn = kPollExpectUs;
            if (st >= 200 && st < 300) {
                copyStr(_prevUri, sizeof(_prevUri), _uri);
                _expectUntilUs = nowUs() + 5000000;
                if (_next.valid && strcmp(_next.uri, _uri) != 0) {  // 次の曲が分かっている: Spotify の返事を待たずに出す
                    _optimistic          = true;
                    _next.valid          = false;
                    const uint32_t durMs = _next.durMs;  // 下で _next を繰り上げる前に
                    publishTrack(_next.uri, _next.title, _next.artist, strncmp(_next.uri, "spotify:track:", 14) == 0, _next.art);
                    if (_next2.valid) {  // その次が次になる: すぐ読み始める (曲名を見てから押すまでに間に合うように)
                        _next        = _next2;
                        _next2.valid = false;
                        requestArt(_next.uri, _next.art, false);
                    }
                    xSemaphoreTake(_lock, portMAX_DELAY);
                    _snap.progressMs = 0;
                    _snap.durationMs = durMs;
                    _snap.fetchedUs  = nowUs();
                    _snap.playing    = true;
                    xSemaphoreGive(_lock);
                    if (strncmp(_uri, "spotify:track:", 14) == 0) {
                        const bool liked = fetchLiked();
                        xSemaphoreTake(_lock, portMAX_DELAY);
                        _snap.liked = liked;
                        xSemaphoreGive(_lock);
                    }
                }
            }
            break;
        case Cmd::Prev:
            st     = api("POST", "/v1/me/player/previous", nullptr);
            pollIn = 400000;
            if (st >= 200 && st < 300) {
                _optimistic    = false;
                _expectUntilUs = nowUs() + 4000000;
            }
            break;
        case Cmd::Like:
        case Cmd::Unlike: {
            if (strncmp(_uri, "spotify:track:", 14) != 0) return;
            const std::string q = "uris=" + urlEncode(_uri);
            st                  = api(c == Cmd::Like ? "PUT" : "DELETE", "/v1/me/library", q.c_str());
            if (st >= 200 && st < 300) {
                xSemaphoreTake(_lock, portMAX_DELAY);
                _snap.liked = c == Cmd::Like;
                xSemaphoreGive(_lock);
            }
            return;
        }
        case Cmd::Logout:  // 鍵を消して、ログインの画面へ (Client ID は次のログインのために残す)
            clearCredentials();
            _logoutAsked = true;
            xSemaphoreTake(_lock, portMAX_DELAY);
            _snap.hasPlayback = false;
            _snap.playing     = false;
            xSemaphoreGive(_lock);
            return;
        case Cmd::VolumeUp:
        case Cmd::VolumeDown: {
            if (!_snap.supportsVolume || _snap.volume < 0) return;
            const int v         = std::clamp(_snap.volume + (c == Cmd::VolumeUp ? 10 : -10), 0, 100);
            const std::string q = "volume_percent=" + std::to_string(v);
            st                  = api("PUT", "/v1/me/player/volume", q.c_str());
            if (st >= 200 && st < 300) {
                xSemaphoreTake(_lock, portMAX_DELAY);
                _snap.volume = v;
                xSemaphoreGive(_lock);
            }
            return;
        }
    }
    if (st >= 200 && st < 300 && (c == Cmd::Play || c == Cmd::Pause)) {
        // 返事を待たずに、見た目を先に合わせる
        const int64_t t = nowUs();
        xSemaphoreTake(_lock, portMAX_DELAY);
        if (_snap.playing && c == Cmd::Pause) {
            _snap.progressMs += static_cast<uint32_t>((t - _snap.fetchedUs) / 1000);
        }
        _snap.playing   = c == Cmd::Play;
        _snap.fetchedUs = t;
        xSemaphoreGive(_lock);
    }
    if (st != 200 && st != 202 && st != 204) mclog::tagWarn(kTag, "command {}: http {}", static_cast<int>(c), st);
    _nextPollUs = nowUs() + pollIn;
}

// ============================================================================ 本体

void Client::run()
{
    bool haveCreds = loadCredentials();
    setState(NetState::Starting, "Spotify", "Starting…", "");
    while (_running.load()) {
        const auto wifi = GetHAL().getWifiStatus();
        if (!wifi.connected) {
            stopSetupServer();
            if (wifi.mode == Hal::WifiMode::ConfigAp) {
                setState(NetState::WifiSetupAp, "Wi-Fi setup", wifi.apSsid.c_str(), wifi.apUrl.c_str());
            } else {
                setState(NetState::WifiConnecting, "Wi-Fi", "Connecting…", wifi.ssid.c_str());
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        copyStr(_ip, sizeof(_ip), wifi.ipAddress.c_str());

        if (!haveCreds) {
            if (_credsArrived.exchange(false)) {
                haveCreds = loadCredentials();
                if (haveCreds) {
                    if (nowUs() >= _stopServerAtUs) stopSetupServer();  // QR ログインなら、少しあとで止める (下)
                    continue;
                }
            }
            bool pending = false;
            xSemaphoreTake(_lock, portMAX_DELAY);
            pending = _codePending;
            xSemaphoreGive(_lock);
            if (pending) {  // スマホで許可された: 鍵に換える
                setState(NetState::Connecting, "Spotify", "Logging in…", "");
                const int r = exchangeCode();
                xSemaphoreTake(_lock, portMAX_DELAY);
                _codePending = false;
                _loginCode[0] = 0;
                if (r > 0) {
                    copyStr(_loginMsg, sizeof(_loginMsg), "ok");
                } else {
                    copyStr(_loginMsg, sizeof(_loginMsg),
                            r == 0 ? "error: Spotify refused the login. Check the Client ID and the Redirect URI, then scan again."
                                   : "error: Could not reach Spotify. Check the Wi-Fi and scan again.");
                }
                xSemaphoreGive(_lock);
                continue;
            }
            startSetupServer();
            if (_server) {
                char ip[40], pin[24];
                snprintf(ip, sizeof(ip), "IP %s", _ip);
                snprintf(pin, sizeof(pin), "PIN %d", _pin);
                setState(NetState::NeedLogin, "Spotify setup", ip, pin);
            } else {  // 受け取りのサーバがまだ (作れなかった): 使えない PIN は出さない
                setState(NetState::NeedLogin, "Spotify setup", "", "Starting setup…");
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (_server && nowUs() >= _stopServerAtUs) stopSetupServer();  // QR ログインのあと 4 秒たった
        int64_t t = nowUs();
        if (t < _retryAfterUs) {
            // Spotify が混んでいて長く待つときは、待っていることを出す (何も出さないと止まって見える)
            const int64_t leftS = (_retryAfterUs - t) / 1000000;
            if (_rateLimited && leftS >= 5) {
                char left[48];
                if (leftS >= 60) {
                    snprintf(left, sizeof(left), "Retrying in %d min", static_cast<int>((leftS + 59) / 60));
                } else {
                    snprintf(left, sizeof(left), "Retrying in %d s", static_cast<int>(leftS));
                }
                setState(NetState::NetError, "Spotify", "Too many requests", left);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        _rateLimited = false;
        if (_accessUntilUs - t < 60000000) {
            if (_snap.state != NetState::Ready) setState(NetState::Connecting, "Spotify", "Connecting…", "");
            const int r = refreshToken();
            if (r == 0) {  // 鍵が使えない: もう一度 Mac からログイン
                clearCredentials();
                haveCreds = false;
                continue;
            }
            if (r < 0) {
                ++_failures;
                if (_failures >= 3) setState(NetState::NetError, "Spotify", "Can't connect", "Retrying…");
                _retryAfterUs = nowUs() + std::min<int64_t>(30000000, 2000000LL << std::min(_failures, 4));
                continue;
            }
            _failures   = 0;
            _nextPollUs = 0;
        }

        // 頼まれた操作を先に。なければ次に見る時刻まで待つ
        t                      = nowUs();
        const int64_t waitUs   = std::clamp<int64_t>(_nextPollUs - t, 0, 200000);
        Cmd c;
        if (xQueueReceive(_cmds, &c, pdMS_TO_TICKS(waitUs / 1000)) == pdTRUE) {
            doCommand(c);
            if (_logoutAsked) {
                _logoutAsked = false;
                haveCreds    = false;
            }
            continue;
        }
        if (nowUs() >= _nextPollUs) poll();
    }
    stopSetupServer();
    if (_api) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(_api));
        _api = nullptr;
    }
}

}  // namespace spotify
