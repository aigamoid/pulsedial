# PulseDial architecture

How a 466×466 round AMOLED on an ESP32-S3 shows Spotify at ~28 fps, with a beat-synced wave, on two cores.
All numbers below are from the source (`main/apps/app_pulsedial/`).

日本語の要約は [各図の下](#日本語の要約) にあります。

---

## 1. System overview

```mermaid
flowchart LR
    subgraph Home["Home Wi-Fi (2.4 GHz)"]
        SW["M5Stack StopWatch<br/>ESP32-S3 · octal PSRAM · 16 MB flash<br/>466×466 AMOLED · mic"]
        Phone["Phone / PC / speaker<br/>(plays the music)"]
    end
    API["api.spotify.com<br/>Web API"]
    ACC["accounts.spotify.com<br/>OAuth 2.0 + PKCE"]
    IMG["i.scdn.co<br/>album art JPEG"]
    RB["api.reccobeats.com<br/>tempo / energy"]
    GH["aigamoid.github.io/pulsedial<br/>installer + callback.html"]

    SW -- "poll /me/player (2 s)<br/>commands: play/next/like/volume" --> API
    API -- "Spotify Connect" --> Phone
    SW -- "refresh token → access token" --> ACC
    SW -- "keep-alive HTTPS, read ahead" --> IMG
    SW -- "track ID only" --> RB
    Phone -. "QR login (once)" .-> ACC
    ACC -. "redirect" .-> GH
    GH -. "one-time code → StopWatch :8080" .-> SW
```

The StopWatch never plays audio. It is a Spotify Connect **remote**: it reads the player state and sends
commands, and the music keeps playing on the phone, PC or speaker.

## 2. Tasks and cores

```mermaid
flowchart TB
    subgraph C0["Core 0 (app core)"]
        MAIN["main loop · prio 5<br/>buttons, touch, RTC (I²C)<br/>10 ms tick"]
        NET["spotify-net · prio 3<br/>poll, commands, token,<br/>QR login, setup httpd"]
        ART["spotify-art · prio 3<br/>album art, read-ahead,<br/>ReccoBeats, TrackMemory flush"]
        AUD["sp-audio · prio 4<br/>mic → FFT → beat"]
        HLP["sp-helper · prio 4<br/>draws half of each frame"]
    end
    subgraph C1["Core 1 (render core)"]
        RND["sp-render · prio 1<br/>compose frame, send dirty rects"]
    end

    MAIN -- "Cmd queue (8)" --> NET
    MAIN -- "Ui queue (8): flash effects" --> RND
    NET -- "Snapshot (mutex _lock)" --> RND
    NET -- "jobs: show / read ahead / download<br/>(mutex _jobLock)" --> ART
    ART -- "_shown / _spare RGB565 (mutex _artLock)<br/>artSeq, pixSeq" --> RND
    AUD -- "Frame: bands, energy, tempo, beat phase" --> RND
    RND -- "runParallel(): rows 0..232" --> HLP
    RND -- "learnTempo()" --> ART
```

- Network and JPEG work never block drawing: the render task only copies a `Snapshot` and swaps pointers.
- While a JPEG is decoded the render task stops using the helper core, so decoding gets the CPU (3 s → 0.5 s).
- The mic stays off until the real album art is shown (avoids competing with the decode and the TLS handshake).

## 3. Album art: from a song change to pixels

```mermaid
flowchart LR
    T["song changes<br/>(poll, or Next = shown at once<br/>from the read-ahead queue)"]
    PIX["TrackMemory<br/>16×16 · 16-color pixel art<br/>(shown instantly)"]
    C{"decoded already?<br/>_shown / _spare"}
    J{"JPEG cache<br/>4 files ≤ 480 KB<br/>PSRAM"}
    H["HTTPS GET i.scdn.co<br/>(keep-alive, ≤ 1 MB)"]
    D["TJpgDec in ROM<br/>0x40000858 / 0x40000864<br/>+ area-average scaler"]
    S["_spare 466×466 RGB565"]
    W["swap _shown ↔ _spare<br/>artSeq++"]
    UI["GlassUi.setArt<br/>220 px art + 466 px ambient<br/>(cut, no transition from pixel art)"]

    T --> PIX
    T --> C
    C -- yes --> W
    C -- no --> J
    J -- hit --> D
    J -- miss --> H --> D
    D --> S --> W --> UI
```

- **Read ahead**: `/me/player/queue` gives the next two songs. The next one is downloaded *and decoded* into
  `_spare`; the one after is only downloaded. Going back shows the previous art from `_spare` too.
- TJpgDec is called by address because a same-named LVGL copy would be linked instead.
- Progressive JPEGs are rejected (ROM TJpgDec can't do them); the real size is read from the SOF marker
  because Spotify's "640 px" images are sometimes 500 px.

## 4. Drawing a frame

```mermaid
flowchart LR
    B["base layer cache<br/>(art, blur, static text)<br/>rebuilt only on change"]
    Z["zones<br/>title, artist, time, ♥, BPM"]
    WV["wave ring<br/>(beat-driven, band rect only)"]
    FX["effects<br/>(play/pause rings, ♥, volume)"]
    FB["M5GFX AMOLED framebuffer<br/>466×466 RGB565"]
    DMA["display(x,y,w,h)<br/>dirty rects only (≤ 72)<br/>FlipBuffer patched: grow-only"]

    B --> FB
    Z --> FB
    WV --> FB
    FX --> FB
    FB --> DMA
```

- Frame rate: 30 fps after input, 20 fps playing untouched, 15 fps paused, 10 fps clock, 4 fps Wi-Fi setup.
- Typical cost while playing: draw ≈ 23 ms (split over 2 cores), send ≈ 6 ms, ~180 rows/frame.
- Burn-in care: the whole picture drifts on a 14 px circle every 1.5 s; dimmer when idle; clock after 3 min paused.
- `patches/M5GFX.patch`: sending many rects of different widths made M5GFX free and re-allocate its DMA buffer
  every time, fragmenting internal RAM until an allocation failed. The patch makes it grow-only.

## 5. Finding the beat

```mermaid
flowchart LR
    MIC["I²S mic<br/>44.1 kHz mono"]
    HOP["hop 512 (11.6 ms)"]
    FFT["FFT 1024 (esp-dsp, AES3)<br/>43 Hz/bin → 32 bands"]
    ON["onset strength<br/>(spectral flux, bands ≥ 6)"]
    AC["autocorrelation of 6 s of onsets<br/>every 0.5 s · 70–184 BPM"]
    PH["beat phase<br/>(−70 ms latency comp.)"]
    RB["ReccoBeats<br/>tempo + energy"]
    TM["TrackMemory<br/>BPM, beat offset, source"]
    WAVE["wave ring shakes on the beat"]

    MIC --> HOP --> FFT --> ON --> AC --> PH
    RB --> TM
    PH -- "same tempo heard 8 s →<br/>learn beat offset" --> TM
    TM --> WAVE
    PH --> WAVE
```

Priority of sources: remembered value → ReccoBeats → microphone. With a known BPM, beats are placed from the
playback position (`(position − offset) mod 60000/BPM`), and the mic only refines the offset. With earphones
(nothing heard for 30 s) the mic is switched off.

## 6. QR login (no PC needed)

```mermaid
sequenceDiagram
    participant U as Phone
    participant S as StopWatch (httpd :8080)
    participant A as accounts.spotify.com
    participant G as GitHub Pages callback.html

    S->>S: show QR http://IP:8080/?pin=NNNN
    U->>S: GET /?pin=NNNN
    S-->>U: page (paste Client ID)
    U->>S: GET /login?pin&client_id
    S->>S: PKCE verifier (stays here), state = IP~nonce
    S-->>U: 302 → authorize?code_challenge&state
    U->>A: log in, Agree
    A-->>U: 302 → callback.html?code&state
    U->>G: load (no referrer)
    G-->>U: location.replace(http://IP:8080/callback?code&state)
    U->>S: GET /callback?code&state
    S->>A: POST /api/token (code + verifier)  [net task]
    A-->>S: refresh token (stored in NVS)
    U->>S: GET /status → "ok"
```

- The code that passes through the browser is useless without the PKCE verifier, which never leaves the StopWatch.
- `callback.html` only forwards to private IPv4 addresses (10/8, 172.16/12, 192.168/16).
- The token exchange runs on the network task (8 KB stack); the httpd task's stack is too small for TLS.
- `CONFIG_HTTPD_MAX_URI_LEN=1536`: the login code doesn't fit the default 512-byte URI.
- Refresh tokens last 180 days; `invalid_grant` sends PulseDial back to the QR screen.

## 7. What it remembers (flash)

| File (`/spiflash/spotify/`) | Slots | Slot | Key |
|---|---|---|---|
| `tempo.bin` | 2048 songs | 24 B: BPM×10, beat offset, source, energy, stamp, FNV-1a32 sum | FNV-1a64(track ID) |
| `pix.bin` | 1024 albums | 176 B: 16-color RGB565 palette + 16×16 4-bit indices, stamp, sum | FNV-1a64(image ID) |

Fixed-size slots, rewritten one at a time (or the whole file if more than 32 changed); least-recently-used slot
is replaced when full; a slot with a wrong checksum (power cut while writing) is dropped on load. Song names
are not stored. NVS (`spotify` namespace) holds only the Client ID and the refresh token.

---

## 日本語の要約

1. **全体**：StopWatch は音を鳴らさない「リモコン」。Spotify の Web API を 2 秒ごとに見にいき、操作を送る。
   ジャケットは i.scdn.co、テンポは ReccoBeats（曲 ID だけ送る）から取る。
2. **タスクとコア**：コア 0 に通信・ジャケット・マイク・描画の手伝い、コア 1 に描画。描画は状態の写し（Snapshot）を
   受け取るだけなので、通信や JPEG の展開で止まらない。
3. **ジャケット**：曲が変わったら、まず覚えていたドット絵 → 展開ずみなら入れ替えるだけ → なければ JPEG のキャッシュか
   ネットから取り、チップの ROM にある TJpgDec で 466px に縮めながら展開する。次の曲は先に展開しておく。
4. **描画**：変わったところだけ画面バッファに描き、その四角だけ送る。2 つのコアで半分ずつ描く。
5. **拍**：覚えた値 → ReccoBeats → マイク（FFT 1024、立ち上がりを 6 秒ためて 0.5 秒ごとに自己相関、70〜184 BPM）の順。拍の位置は再生位置から計算し、
   マイクで 8 秒そろったら覚える。
6. **QR ログイン**：PKCE の合言葉は本体の中だけ。ブラウザを通るコードだけでは鍵に換えられない。
7. **記憶**：BPM 2048 曲・ドット絵 1024 アルバムを、決まった大きさの枠でフラッシュに。曲名は持たない。
