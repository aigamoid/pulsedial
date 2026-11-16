/*
 * Spotify リモコン。説明は app_pulsedial.h
 * - 音のタスク (メインと同じコア): マイクを 11.6ms ずつ読んで Analyzer へ
 * - Spotify のタスク (メインと同じコア): SpotifyClient
 * - 描くタスク (もう一方のコア): 30fps。Spotify の状態を GlassUi に渡し、変わったところだけ画面バッファに描いて、
 *   その行だけパネルへ送る (公式の画面バッファの送り方。描き終わってから一気に送るので、描きかけは映らない)
 * - メインのタスク: ボタンとタッチ
 */
#include "app_pulsedial.h"
extern "C" {
#include "qrcodegen.h"
}

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lgfx/v1/panel/Panel_AMOLED.hpp>
#include <stdio.h>
#include <algorithm>
#include <new>

using namespace spviz;

namespace {

constexpr const char* kTag          = "spotify-app";
constexpr int64_t kIdleAfterPauseUs = 180LL * 1000000;  // 一時停止のまま 3 分たったら時計の画面に
constexpr const char* kIdleHint     = "Play something on Spotify";

void* allocFast(size_t bytes)
{
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

// PSRAM を先に (Wi-Fi と TLS のために内蔵メモリを空けておく)
void* allocPsram(size_t bytes)
{
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void freeMem(void* p)
{
    heap_caps_free(p);
}

template <typename T>
T* newIn(void* (*alloc)(size_t))
{
    void* p = alloc(sizeof(T));
    return p ? new (p) T() : nullptr;
}

template <typename T>
void deleteMem(T*& p)
{
    if (p) {
        p->~T();
        heap_caps_free(p);
        p = nullptr;
    }
}

// 公式の画面バッファの中身 (ここへ直接描いて、display() で送る)
struct FbPeek : public lgfx::Panel_AMOLED_Framebuffer {
    static uint8_t* buffer(lgfx::Panel_AMOLED_Framebuffer* fb) { return fb->*(&FbPeek::_frame_buffer); }
};

// 文字を白黒で描く (M5GFX)。曲名などは IPA ゴシック 40px (太字は 1px ずらして重ねる)、時計は DejaVu72。縮めるのは GlassUi
bool rasterText(void*, const char* s, int font, Mask* out, AllocFn alloc)
{
    LGFX_Sprite spr;
    spr.setPsram(true);
    spr.setColorDepth(8);
    if (font == kFontDigits72) {
        spr.setFont(&lgfx::fonts::DejaVu72);
    } else {
        spr.setFont(&lgfx::fonts::lgfxJapanGothicP_40);
    }
    spr.setTextSize(1);
    spr.setTextDatum(lgfx::textdatum_t::top_left);
    const int tw = spr.textWidth(s);
    const int h  = spr.fontHeight() + 3;
    if (tw <= 0 || h <= 3) return false;
    const int w = std::min(tw + 5, 2800);  // とても長い曲名は切る (流して見せる)
    if (!spr.createSprite(w, h)) return false;
    spr.fillSprite(TFT_BLACK);
    spr.setTextColor(TFT_WHITE);
    spr.drawString(s, 2, 1);
    if (font == kFontJp40Bold) {
        spr.drawString(s, 3, 1);
        spr.drawString(s, 2, 2);
    }
    out->w = w;
    out->h = h;
    out->a = static_cast<uint8_t*>(alloc(static_cast<size_t>(w) * h));
    if (out->a) {
        const uint8_t* src = static_cast<const uint8_t*>(spr.getBuffer());
        for (int i = 0; i < w * h; ++i) out->a[i] = src[i] ? 255 : 0;
    }
    spr.deleteSprite();
    return out->a != nullptr;
}

}  // namespace

AppPulseDial::AppPulseDial()
{
    setAppInfo().name = "PulseDial";  // PulseDial — Spotify remote for M5Stack StopWatch
    setAppInfo().icon = (void*)&pulsedial_icon;
}

void AppPulseDial::onCreate()
{
    mclog::tagInfo(kTag, "on create");
}

void AppPulseDial::onOpen()
{
    mclog::tagInfo(kTag, "on open  internal free {} KB (largest {} KB)",
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    _key_manager = std::make_unique<input::KeyManager>();
    // ボタンとタッチを読むこのタスクを、開いている間はいちばん上に (展開や通信の裏で、短い 2 回押しを読み逃さない)。
    // 1 回の仕事は軽く、毎回 10ms 休むので、ほかの邪魔はしない
    _last_input_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    _main_prio = static_cast<int>(uxTaskPriorityGet(nullptr));
    vTaskPrioritySet(nullptr, 5);

    // ほかの描くアプリと同じく、LVGL を止めてから画面を直接使う
    GetHAL().stopLvglUpdate();
    if (GetHAL().lvglLock()) {
        GetHAL().lvglUnlock();
    }
    auto& display = GetHAL().getDisplay();
    display.startWrite();
    display.fillScreen(TFT_BLACK);
    display.endWrite();
    _fb_w = display.width();
    _fb_h = display.height();
    _fb   = reinterpret_cast<uint16_t*>(FbPeek::buffer(static_cast<lgfx::Panel_AMOLED_Framebuffer*>(display.getPanel())));

    // Wi-Fi: 切っていたら、開いている間だけ入れる (設定は書き換えない)
    _wifi_was_enabled = GetHAL().isWifiEnabled();
    if (!_wifi_was_enabled) GetHAL().setWifiEnabled(true, false);
    _user_bright = GetHAL().getBackLightBrightness();
    _bright_now  = _user_bright;

    _analyzer = newIn<Analyzer>(allocPsram);
    _ui_view  = newIn<GlassUi>(allocPsram);
    _ready    = _fb && _analyzer && _ui_view;
    if (_ready) {
        _analyzer->begin();
        _ready = _ui_view->begin(allocPsram, freeMem, rasterText, nullptr, allocFast);
    }
    if (_ready) {
        _ui_view->setClockFn([]() -> int64_t { return esp_timer_get_time(); });
        _ui_view->setViz(Viz::Wave);  // 静かな画面は、拍で震える白い波形
        _ui_view->setSkipStyle(1);  // 曲送りはエフェクトなし (光の筋は本体で 8fps まで落ちて重く見えた)
        _ui_view->showNotice(GlassUi::Notice::Logo, "Starting…", "Spotify", "");
    }
    GetHAL().audioSetMicGain(30.0f);

    const int64_t now = esp_timer_get_time();
    _last_frame_us    = now;
    _paused_since_us  = 0;
    _track_seq = _art_seq = 0;
    _next_log_us          = now + 10000000;
    _frames = _rows_sent = _send_us_sum = 0;
    _touch_down  = false;
    _next_time_ms = 0;
    _mic_errors.store(0);

    const BaseType_t app_core    = xPortGetCoreID();
    const BaseType_t render_core = app_core == 0 ? 1 : 0;
    _client_started = _ready && _client.start(app_core);

    _ui             = xQueueCreate(8, sizeof(Ui));
    _audio_stopped  = xSemaphoreCreateBinary();
    _render_stopped = xSemaphoreCreateBinary();
    _running.store(_ready && _ui);
    _audio_task_created = _render_task_created = false;
    if (_running.load() && _audio_stopped && _render_stopped) {
        _audio_task_created =
            // マイクは優先度 4 (描く手伝いと同じ)。2・3 だと待たされて I2S の入れ物 (約 33ms) があふれ、音を取りこぼしてテンポが数 % 速く出た。
            // 開いて 6 秒は聞かない (暗号の接続作りと重なると、CPU0 が埋まって固まり見張りが鳴った)
            xTaskCreatePinnedToCore(audioTaskEntry, "sp-audio", 6144, this, 4, &_audio_task, app_core) == pdPASS;
        // 描く手伝いは、描くタスクと反対のコアに (優先度は通信より上: 2 秒ごとの問い合わせでコマが止まらないように)
        _helper_done    = xSemaphoreCreateBinary();
        _helper_stopped = xSemaphoreCreateBinary();
        _helper_quit.store(false);
        if (_helper_done && _helper_stopped &&
            xTaskCreatePinnedToCore(helperTaskEntry, "sp-helper", 4096, this, 4, &_helper_task, app_core) == pdPASS) {
            _ui_view->setParallel(runParallel, this);
        } else {
            _helper_task = nullptr;
        }
        _render_task_created =
            xTaskCreatePinnedToCore(renderTaskEntry, "sp-render", 8192, this, 1, &_render_task, render_core) == pdPASS;
    }
    mclog::tagInfo(kTag, "ready {} fb {}x{} client {} audio {} render {}  internal free {} KB  psram free {} KB", _ready, _fb_w,
                   _fb_h, _client_started, _audio_task_created, _render_task_created,
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void AppPulseDial::post(Ui u)
{
    _last_input_ms.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
    if (_ui) xQueueSend(_ui, &u, 0);
}

void AppPulseDial::togglePlay()
{
    spotify::Snapshot s;
    _client.snapshot(s);
    // 頼みを受け付けたときだけ印を出す (列がいっぱいで捨てたのに、通ったように見せない)
    if (s.hasPlayback && s.playing) {
        if (_client.command(spotify::Cmd::Pause)) post(Ui::Pause);
    } else {
        if (_client.command(spotify::Cmd::Play)) post(Ui::Play);
    }
}

void AppPulseDial::toggleLike()
{
    spotify::Snapshot s;
    _client.snapshot(s);
    if (!s.hasPlayback || !s.isTrack) return;
    if (_client.command(s.liked ? spotify::Cmd::Unlike : spotify::Cmd::Like)) post(s.liked ? Ui::Unlike : Ui::Like);
}

void AppPulseDial::onRunning()
{
    GetHAL().updateButtonStates();
    const auto event = _key_manager ? _key_manager->update(false) : input::KeyEvent::None;
    if (event == input::KeyEvent::GoHome) {
        close();
        return;
    }
    if (!_running.load()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // ボタン: A = 再生/停止、B 1 回 = 次の曲、B 2 回 = 前の曲、B 長押し = ♥ (A と B の同時長押しはランチャーへ)
    auto& btnA = GetHAL().btnA;
    auto& btnB = GetHAL().btnB;
    if (btnA.wasClicked() && !btnB.isPressed()) togglePlay();
    // A を 5 秒長押し: Spotify からログアウト (別のアカウントや Client ID でログインし直すとき)
    if (btnA.pressedFor(5000) && !btnB.isPressed() && !_a_logout) {
        _a_logout = true;
        _client.command(spotify::Cmd::Logout);
    }
    if (btnA.isReleased()) _a_logout = false;
    if (btnB.wasHold() && !btnA.isPressed()) {
        toggleLike();
        _b_held = true;  // このあとに決まるクリックの回数は使わない
    }
    if (btnB.wasDecideClickCount()) {
        const int n = btnB.getClickCount();
        if (!_b_held && !btnA.isPressed()) {
            if (n == 1) {
                if (_client.command(spotify::Cmd::Next)) post(Ui::Next);
            } else if (n == 2) {
                if (_client.command(spotify::Cmd::Prev)) post(Ui::Prev);
            }
        }
        _b_held = false;
    }
    if (btnB.wasReleasedAfterHold()) _b_held = btnB.getClickCount() > 0;  // 長押しの前にクリックがあれば、その回数も捨てる

    // タッチ: ♥ の丸はお気に入り、ほかのタップは詳細を 7 秒出す。左右にはらう = 曲送り、上下にはらう = 音量
    const auto tp   = GetHAL().getTouchPoint();
    const bool down = tp.num > 0 && tp.x >= 0 && tp.y >= 0;
    if (down) {
        if (!_touch_down) {
            _down_ms = now;
            _down_x = _last_x = tp.x;
            _down_y = _last_y = tp.y;
        }
        _last_x = tp.x;
        _last_y = tp.y;
    } else if (_touch_down) {
        const int dx = _last_x - _down_x, dy = _last_y - _down_y;
        if (abs(dx) > 70 && abs(dx) > abs(dy)) {
            if (_client.command(dx < 0 ? spotify::Cmd::Next : spotify::Cmd::Prev)) post(dx < 0 ? Ui::Next : Ui::Prev);
        } else if (abs(dy) > 70) {
            spotify::Snapshot s;
            _client.snapshot(s);
            if (s.supportsVolume && s.volume >= 0) {
                const bool up = dy < 0;
                if (_client.command(up ? spotify::Cmd::VolumeUp : spotify::Cmd::VolumeDown)) {
                    _vol_shown.store(std::clamp(s.volume + (up ? 10 : -10), 0, 100));
                    _vol_changed_ms = now;
                    post(up ? Ui::VolUp : Ui::VolDown);
                }
            } else {
                post(Ui::Wake);  // 音量を変えられない機器 (iPhone など): 詳細だけ
            }
        } else if (now - _down_ms < 450) {
            const int ox = (_fb_w - kSize) / 2, oy = (_fb_h - kSize) / 2;
            const int b  = _ui_view ? _ui_view->hit(_down_x - ox, _down_y - oy) : 0;
            if (b == 4) {
                toggleLike();
            } else {
                post(Ui::Wake);  // 詳細を 7 秒
            }
        }
    }
    _touch_down = down;

    // 時計 (RTC は I2C なので、メインのタスクで読む)
    if (now >= _next_time_ms) {
        _next_time_ms = now + 1000;
        const auto t  = GetHAL().getTimeHms();
        _hour.store(t.isValid() ? t.hour : -1);
        _minute.store(t.isValid() ? t.minute : -1);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
}

void AppPulseDial::onClose()
{
    mclog::tagInfo(kTag, "on close");
    if (_main_prio >= 0) vTaskPrioritySet(nullptr, _main_prio);  // ボタンを読むタスクの優先度を元に
    _main_prio = -1;
    _running.store(false);
    if (_audio_task_created) xSemaphoreTake(_audio_stopped, portMAX_DELAY);
    if (_render_task_created) xSemaphoreTake(_render_stopped, portMAX_DELAY);
    _audio_task_created = _render_task_created = false;
    _audio_task = _render_task = nullptr;
    if (_helper_task) {  // 描くタスクが止まってから、手伝いを止める
        _helper_quit.store(true);
        xTaskNotifyGive(_helper_task);
        xSemaphoreTake(_helper_stopped, portMAX_DELAY);
        _helper_task = nullptr;
    }
    if (_ui_view) _ui_view->setParallel(nullptr, nullptr);
    if (_helper_done) vSemaphoreDelete(_helper_done);
    if (_helper_stopped) vSemaphoreDelete(_helper_stopped);
    _helper_done = _helper_stopped = nullptr;
    _client.stop();  // 始めるのに失敗していても、作れた分をここで片付ける (2 回呼んでもよい)
    _client_started = false;
    if (_audio_stopped) vSemaphoreDelete(_audio_stopped);
    if (_render_stopped) vSemaphoreDelete(_render_stopped);
    if (_ui) vQueueDelete(_ui);
    _audio_stopped = _render_stopped = nullptr;
    _ui                              = nullptr;

    if (_ui_view) _ui_view->end();
    deleteMem(_ui_view);
    deleteMem(_analyzer);
    _fb    = nullptr;
    _ready = false;
    _key_manager.reset();

    if (!_wifi_was_enabled) GetHAL().setWifiEnabled(false, false);
    GetHAL().setBackLightBrightness(_user_bright, false);

    auto& display = GetHAL().getDisplay();
    display.startWrite();
    display.fillScreen(TFT_BLACK);
    display.endWrite();
    if (GetHAL().lvglLock()) {
        lv_obj_invalidate(lv_screen_active());
        GetHAL().lvglUnlock();
    }
    GetHAL().startLvglUpdate();
}

void AppPulseDial::audioTaskEntry(void* arg)
{
    auto* self = static_cast<AppPulseDial*>(arg);
    self->audioLoop();
    xSemaphoreGive(self->_audio_stopped);
    vTaskDelete(nullptr);
}

void AppPulseDial::renderTaskEntry(void* arg)
{
    auto* self = static_cast<AppPulseDial*>(arg);
    self->renderLoop();
    xSemaphoreGive(self->_render_stopped);
    vTaskDelete(nullptr);
}

void AppPulseDial::helperTaskEntry(void* arg)
{
    auto* self = static_cast<AppPulseDial*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (self->_helper_quit.load()) break;
        self->_helper_job(self->_helper_arg, 0);
        xSemaphoreGive(self->_helper_done);
    }
    xSemaphoreGive(self->_helper_stopped);
    vTaskDelete(nullptr);
}

// 半分 (part 0) を手伝いのコアで、残り (part 1) をここで同時に走らせ、両方終わるまで待つ
void AppPulseDial::runParallel(void* ctx, spviz::GlassUi::ParJob job, void* arg)
{
    auto* self = static_cast<AppPulseDial*>(ctx);
    if (!self->_helper_task) {
        job(arg, 0);
        job(arg, 1);
        return;
    }
    self->_helper_job = job;
    self->_helper_arg = arg;
    xTaskNotifyGive(self->_helper_task);
    job(arg, 1);
    xSemaphoreTake(self->_helper_done, portMAX_DELAY);
}

void AppPulseDial::audioLoop()
{
    int16_t hop[kHop];
    // 開いて 6 秒は聞かない
    const int64_t startAt = esp_timer_get_time() + 6000000;
    while (_running.load() && esp_timer_get_time() < startAt) vTaskDelay(pdMS_TO_TICKS(100));
    // 取りこぼしの見張り: 2 秒ごとに、受け取った音の量が時間に合っているか (足りなければ、テンポは聞き直す)
    int64_t winStart = esp_timer_get_time();
    uint32_t winSamples = 0, winProcUs = 0, winHops = 0;
    bool fresh = true;  // 止めていたあとは、入れ物にたまった分で量がずれるので 1 回見送る
    while (_running.load()) {
        if (_light.load() || _mic_off.load()) {  // Wi-Fi の設定中と、拍で動かしている (BPM が分かる) ときは聞かない
            vTaskDelay(pdMS_TO_TICKS(50));
            fresh = true;
            continue;
        }
        if (!GetHAL().audioReadMic(hop, kHop)) {
            _mic_errors.fetch_add(1);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int64_t p0 = esp_timer_get_time();
        _analyzer->process(hop);
        const int64_t now = esp_timer_get_time();
        winProcUs += static_cast<uint32_t>(now - p0);
        ++winHops;
        winSamples += kHop;
        if (now - winStart >= 2000000) {
            const float rate = winSamples * 1.0e6f / static_cast<float>(now - winStart);
            if (!fresh && rate < kSampleRate * 0.985f) {  // ログはここでは書かない (スタックがあふれた)。数だけ数えて、描く側のログに出す
                _analyzer->resetTempo();
                _mic_drops.fetch_add(1);
            }
            if (!fresh) {
                _mic_rate.store(static_cast<int>(rate));
                _mic_proc_us.store(static_cast<int>(winProcUs / std::max<uint32_t>(1, winHops)));
            }
            fresh      = false;
            winStart   = now;
            winSamples = winProcUs = winHops = 0;
        }
    }
}

void AppPulseDial::renderLoop()
{
    int64_t next = esp_timer_get_time();
    while (_running.load()) {
        const int64_t now = esp_timer_get_time();
        if (now < next) {
            const uint32_t ms = static_cast<uint32_t>((next - now) / 1000);
            vTaskDelay(ms > 1 ? pdMS_TO_TICKS(ms - 1) : 1);
            continue;
        }
        next = now + _frame_us.load();  // 遅れたコマは取り戻さない
        renderFrame();
        vTaskDelay(1);
    }
}

// Spotify の状態を画面へ
void AppPulseDial::applySnapshot(const spotify::Snapshot& s, int64_t now)
{
    if (s.state != spotify::NetState::Ready) {
        showNoticeFor(s);
    } else {
        if (s.playing) {
            _paused_since_us = 0;
        } else if (_paused_since_us == 0) {
            _paused_since_us = now;
        }
        const bool player = s.hasPlayback && (s.playing || now - _paused_since_us < kIdleAfterPauseUs);
        if (player) {
            _ui_view->showPlayer();
        } else {
            _ui_view->showIdle(kIdleHint);
        }
    }
    if (s.trackSeq != _track_seq) {
        _track_seq = s.trackSeq;
        if (_analyzer) _analyzer->resetTempo();
        _ui_view->setTrack(s.title, s.artist);
    }
    _ui_view->setDevice(s.device);
    // 曲送りのエフェクト中は、ジャケットを替えない (全体の描き直しでコマが落ち、エフェクトが重く見えた)。終わったらすぐ替える
    const bool holdArt = _ui_view->skipFxActive();
    if (!holdArt && s.pixSeq != _pix_seq) {  // 覚えていたドット絵 (本物のジャケットより先に)
        _pix_seq = s.pixSeq;
        spviz::PixelArt pa;
        _client.lockPix(&pa);
        _ui_view->setArtPixel(pa);
    }
    if (!holdArt && s.artSeq != _art_seq) {
        _art_seq           = s.artSeq;
        const uint16_t* px = nullptr;
        int w = 0, h = 0;
        _client.lockArt(&px, &w, &h);
        _ui_view->setArt(px, w, h);
        _client.unlockArt();
    }
    uint32_t ms = s.progressMs;
    if (s.playing) ms += static_cast<uint32_t>((now - s.fetchedUs) / 1000);
    if (s.durationMs > 0) ms = std::min(ms, s.durationMs);
    _ui_view->setPlayback(s.playing, ms, s.durationMs, s.liked, s.isTrack);
    _pos_ms = static_cast<float>(ms);
    // テンポが分かっていても、拍の位置を覚えるまではマイクを聞く (覚えたか、30 秒何も聞こえなければ止める)
    // 曲が変わったら、本物のジャケットが出るまでは聞かない (覚えていたドット絵 → ジャケット → マイクの順。展開と重ならないように)
    _mic_off.store(s.artPending || (s.bpm > 0 && (_learned || _no_hear_s > 30)));
    // 音量: 変えた直後は、返事を待たずに変えたあとの値 (ゲージ用)
    if (static_cast<uint32_t>(now / 1000) - _vol_changed_ms > 2000) _ui_view->setVolume(s.volume);
    _ui_view->setClock(_hour.load(), _minute.load());
}

// QR を作って案内の画面に渡す (中身が前と同じなら何もしない)
void AppPulseDial::setQr(const std::string& text)
{
    if (text == _qr_text) return;
    strncpy(_qr_text, text.c_str(), sizeof(_qr_text) - 1);
    _qr_text[sizeof(_qr_text) - 1] = 0;
    // 作業用は PSRAM に (内蔵メモリは Wi-Fi の設定ページに残す)
    constexpr size_t kQrBuf = qrcodegen_BUFFER_LEN_FOR_VERSION(10);
    auto* work = static_cast<uint8_t*>(heap_caps_malloc(kQrBuf * 2 + 57 * 57, MALLOC_CAP_SPIRAM));
    if (!work) return;
    uint8_t *qr = work, *tmp = work + kQrBuf, *mods = work + kQrBuf * 2;
    if (qrcodegen_encodeText(text.c_str(), tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) {
        const int n = qrcodegen_getSize(qr);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) mods[y * n + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
        _ui_view->setNoticeQr(mods, n);
    }
    heap_caps_free(work);
}

// 案内の画面: 状態ごとに絵柄と文言 (ドット絵 + 白い文字)。Wi-Fi 設定は本体の Wi-Fi に入る QR、ログインは本体のページの QR
void AppPulseDial::showNoticeFor(const spotify::Snapshot& s)
{
    using N = GlassUi::Notice;
    switch (s.state) {
        case spotify::NetState::Starting: _ui_view->showNotice(N::Logo, "Starting…", "Spotify", ""); break;
        case spotify::NetState::WifiConnecting: _ui_view->showNotice(N::Wifi, "Connecting to Wi-Fi", s.msg[2], ""); break;
        case spotify::NetState::WifiSetupAp: {
            // QR の中身: WIFI:T:nopass;S:<名前>;; (本体の Wi-Fi は鍵なし)。名前の \ ; , : " は \ を付ける
            std::string txt = "WIFI:T:nopass;S:";
            for (const char* p = s.msg[1]; *p; ++p) {
                if (strchr("\\;,:\"", *p)) txt += '\\';
                txt += *p;
            }
            txt += ";;";
            setQr(txt);
            const char* url = s.msg[2];
            if (strncmp(url, "http://", 7) == 0) url += 7;
            const std::string open = std::string("then open ") + url;
            _ui_view->showNotice(N::Qr, "Scan to join Wi-Fi", s.msg[1], open.c_str());
            break;
        }
        case spotify::NetState::NeedLogin: {
            // msg[1] = "IP 192.168.1.23"、msg[2] = "PIN 1234"。受け取りのサーバがまだなら msg[1] は空
            if (strncmp(s.msg[1], "IP ", 3) != 0 || strncmp(s.msg[2], "PIN ", 4) != 0) {
                _ui_view->showNotice(N::Key, s.msg[2], "Spotify setup", "", true);
                break;
            }
            const char* ip  = s.msg[1] + 3;
            const char* pin = s.msg[2] + 4;
            setQr(std::string("http://") + ip + ":8080/?pin=" + pin);  // スマホで読むと本体のページが開く
            const std::string open = std::string("or open ") + ip + ":8080";
            _ui_view->showNotice(N::Qr, "Scan to log in to Spotify", s.msg[2], open.c_str());
            break;
        }
        case spotify::NetState::Connecting:  // 2 行目はログイン中だけ ("Logging in…")
            _ui_view->showNotice(N::Logo, "Connecting to Spotify", strcmp(s.msg[1], "Connecting…") == 0 ? "" : s.msg[1], "");
            break;
        default: _ui_view->showNotice(N::Logo, s.msg[1], s.msg[2], ""); break;  // つながらない・鍵が使えない
    }
}

void AppPulseDial::tempoStep(const spotify::Snapshot& s, const spviz::Frame& in, float dt)
{
    if (s.trackSeq != _tempo_track) {  // 曲が変わった
        _tempo_track = s.trackSeq;
        _off_ok = _learned = false;
        _agree_s = _stable_s = _stable_bpm = _disagree_s = _no_hear_s = 0;
    }
    // マイクに音楽が入っている (イヤホンなら入らない)
    const bool hears = s.playing && in.tempo > 0 && !in.quiet && s.trackId[0];
    _no_hear_s       = hears ? 0 : _no_hear_s + dt;
    auto micOffset   = [&](float bpm) {  // マイクで聞いた拍の位置を、曲の位置で表す
        const float per = 60000.0f / bpm;
        float o         = fmodf(_pos_ms - in.beatAgo * 1000.0f, per);
        return o < 0 ? o + per : o;
    };
    if (s.bpm > 0) {  // 覚えていた値か ReccoBeats: 曲の位置で拍を刻む
        float off = _off_ok ? _off : s.beatOffsetMs;
        if (hears && fabsf(in.tempo - s.bpm) < s.bpm * 0.04f) {  // 同じテンポが聞こえる: 拍の位置を少しずつ合わせる
            const float per = 60000.0f / s.bpm, target = micOffset(s.bpm);
            if (!_off_ok) {
                _off    = target;
                _off_ok = true;
            } else {
                float d = target - _off;
                d -= per * roundf(d / per);
                _off += d * std::min(1.0f, dt * 0.8f);
                _off = fmodf(_off + per, per);
            }
            off = _off;
            _agree_s += dt;
            if (_agree_s > 8 && !_learned) {  // 8 秒そろったら、拍の位置を覚える
                _client.learnTempo(s.trackId, s.bpm, _off, s.bpmSource == spotify::kTempoRecco ? spotify::kTempoRecco : spotify::kTempoMic);
                _learned = true;
            }
        } else if (hears && s.bpmSource == spotify::kTempoMic) {  // マイクで覚えた値と、ちがうテンポが 20 秒続く: 覚え直す
            if (fabsf(in.tempo - _stable_bpm) < in.tempo * 0.02f) {
                _disagree_s += dt;
            } else {
                _stable_bpm = in.tempo;
                _disagree_s = 0;
            }
            if (_disagree_s > 20 && !_learned) {
                _client.learnTempo(s.trackId, in.tempo, micOffset(in.tempo), spotify::kTempoMic);
                _learned = true;
                _off_ok  = false;
            }
        }
        _ui_view->setBeat(s.bpm, s.energy, off);
        _ui_view->setMicBeat(0, 0, 0);
        return;
    }
    // テンポが分からない: マイクで聞いたテンポで動かし、10 秒安定したら覚える (静かなら、ゆっくりゆらぐだけ)
    _ui_view->setBeat(0, s.energy, 0);
    _ui_view->setMicBeat(in.tempo, in.beatAgo, in.energy);
    if (hears) {
        if (fabsf(in.tempo - _stable_bpm) < in.tempo * 0.02f) {
            _stable_s += dt;
        } else {
            _stable_bpm = in.tempo;
            _stable_s   = 0;
        }
        if (_stable_s > 10 && !_learned) {
            _client.learnTempo(s.trackId, in.tempo, micOffset(in.tempo), spotify::kTempoMic);
            _learned = true;
        }
    }
}

void AppPulseDial::renderFrame()
{
    auto& display         = GetHAL().getDisplay();
    const int64_t started = esp_timer_get_time();
    const float dt        = std::min(0.2f, static_cast<float>(started - _last_frame_us) * 1.0e-6f);
    _last_frame_us        = started;

    // 操作の印
    Ui u;
    while (_ui && xQueueReceive(_ui, &u, 0) == pdTRUE) {
        switch (u) {
            case Ui::Play: _ui_view->flash(Flash::Play); break;
            case Ui::Pause: _ui_view->flash(Flash::Pause); break;
            case Ui::Next: _ui_view->flash(Flash::Next); break;
            case Ui::Prev: _ui_view->flash(Flash::Prev); break;
            case Ui::Like: _ui_view->flash(Flash::Like); break;
            case Ui::Unlike: _ui_view->flash(Flash::Unlike); break;
            case Ui::Wake: _ui_view->wake(); break;
            case Ui::Ambient: _ui_view->enterAmbient(); break;
            case Ui::VolUp:
            case Ui::VolDown:
                _ui_view->setVolume(_vol_shown.load());
                _ui_view->flash(u == Ui::VolUp ? Flash::VolUp : Flash::VolDown);
                break;
        }
    }
    spotify::Snapshot snap;
    if (_client_started) {
        _client.snapshot(snap);
        applySnapshot(snap, started);
    }
    // 速さ: 30fps。Wi-Fi の設定中は 4fps で音も聞かない
    const bool light = snap.state == spotify::NetState::WifiSetupAp;
    _light.store(light);
    // コマ数: 操作して 1 分は 30、再生中で触っていなければ 20、止めていれば 15、時計の画面は 10 (熱と電池のため。動きは時間で計算するので速さは同じ)
    int hz = kRenderHz;
    if (_ui_view->mode() != Mode::Player) {
        hz = 10;
    } else if (!snap.playing) {
        hz = 15;
    } else if (static_cast<uint32_t>(started / 1000) - _last_input_ms.load() > 60000) {
        hz = 20;
    }
    _frame_us.store(light ? 250000 : 1000000 / hz);

    Frame in;
    _analyzer->take(in);
    tempoStep(snap, in, dt);

    // 変わったところだけ画面バッファに描いて、その四角だけ送る (偶数へのそろえは M5GFX がする)
    const int ox = (_fb_w - kSize) / 2, oy = (_fb_h - kSize) / 2;
    GlassUi::Dirty dirty[GlassUi::kMaxDirty];
    int nd = 0;
    _ui_view->frame(dt, in, _fb + oy * _fb_w, _fb_w, ox, dirty, &nd);
    const int64_t drawn = esp_timer_get_time();
    if (nd > 0) {
        display.startWrite();
        for (int i = 0; i < nd; ++i) {
            const auto& d = dirty[i];
            display.display(ox + d.x, oy + d.y, d.w, d.h);
            _rows_sent += d.w * d.h / kSize;  // 1 行 = 466 画素に直した数
        }
        display.endWrite();
    }
    const int64_t done = esp_timer_get_time();
    _send_us_sum += static_cast<uint32_t>(done - drawn);

    // 明るさ: 再生中は設定どおり。止まっている・時計の画面は 6 割。
    // (夜中に暗くするのはやめた: 本体の時計は再起動で 2024 年に戻ったり、タイムゾーンが GMT0 だったりして、昼に 4 割まで暗くなった)
    {
        int target = _user_bright;
        if (_ui_view->mode() != Mode::Player || !snap.playing) target = target * 6 / 10;
        if (_ui_view->ambient()) target = target * 85 / 100;  // 静かな画面は少し暗く (焼き付き対策)
        // 静かな画面ではキックで一瞬明るく (画面全体がドンと光る)
        target = std::min(100, static_cast<int>(target * (1.0f + 0.45f * _ui_view->pulse())));
        target = std::max(5, target);
        if (target != _bright_now) {
            _bright_now = target;
            GetHAL().setBackLightBrightness(target, false);
        }
    }

    ++_frames;
    if (done >= _next_log_us) {
        const float secs = (done - _next_log_us + 10000000) * 1e-6f;
        int rebuilds     = 0;
        uint32_t rebuildUs = 0, drawUs = 0;
        _ui_view->takeStats(&rebuilds, &rebuildUs, &drawUs);
        mclog::tagInfo(kTag,
                       "{:.1f}fps draw {}ms send {}ms rows {}/frame rebuilds {} ({}ms) | net {} http {} | energy {:.2f} | "
                       "internal {}KB psram {}KB mic err {} drops {} ({} Hz, {}us/hop) | bpm {:.1f} (src {}) mic {:.1f}{}{}",
                       _frames / secs, drawUs / _frames / 1000, _send_us_sum / _frames / 1000, _rows_sent / _frames, rebuilds,
                       rebuilds ? rebuildUs / rebuilds / 1000 : 0, static_cast<int>(snap.state), snap.lastError, in.energy,
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), _mic_errors.load(), _mic_drops.load(), _mic_rate.load(), _mic_proc_us.load(), snap.bpm,
                       static_cast<int>(snap.bpmSource), in.tempo, in.quiet ? " quiet" : "", _mic_off.load() ? " off" : "");
        uint32_t parts[4];
        _ui_view->takeParts(parts);
        mclog::tagInfo(kTag, "  per frame (0.1ms): calc {} wave {} redraw {} fx {}", parts[0] / _frames / 100, parts[1] / _frames / 100,
                       parts[2] / _frames / 100, parts[3] / _frames / 100);
        _next_log_us = done + 10000000;
        _frames = _rows_sent = _send_us_sum = 0;
    }
}
