# Third-party notices

PulseDial is released under the MIT License (see [LICENSE](LICENSE)). The firmware it builds — and the
binaries on the web installer page — also contain the following software. Their full license texts are in
each project's repository; `fetch_repos.py` and `idf.py build` download them into `components/` and
`managed_components/`.

| Software | License | Source |
|---|---|---|
| M5StopWatch-UserDemo (the base of this repository) | MIT | https://github.com/m5stack/M5StopWatch-UserDemo |
| ESP-IDF | Apache-2.0 | https://github.com/espressif/esp-idf |
| M5GFX (patched by `patches/M5GFX.patch`) | MIT | https://github.com/m5stack/M5GFX |
| IPA fonts (Japanese text, inside M5GFX) | IPA Font License v1.0 | https://moji.or.jp/ipafont/ |
| LVGL | MIT | https://github.com/lvgl/lvgl |
| mooncake, smooth_ui_toolkit | MIT | https://github.com/Forairaaaaa |
| mooncake_log | MIT | https://github.com/Forairaaaaa/mooncake_log |
| ArduinoJson | MIT | https://github.com/bblanchon/ArduinoJson |
| M5IOE1, M5PM1 | MIT | https://github.com/m5stack |
| BMI270_BMM150_Sensor | MIT | https://github.com/lbuque |
| esp-wifi-connect | MIT | https://components.espressif.com/components/78/esp-wifi-connect |
| esp-dsp, esp_codec_dev, i2c_bus, cmake_utilities | Apache-2.0 | https://components.espressif.com/ |
| QR Code generator (in `main/apps/app_pulsedial/qrcodegen.*`) | MIT | https://www.nayuki.io/page/qr-code-generator-library |
| ESP Web Tools (loaded by `docs/index.html`) | Apache-2.0 | https://github.com/esphome/esp-web-tools |

Spotify is a trademark of Spotify AB. M5Stack is a trademark of M5Stack Technology Co., Ltd.
PulseDial is not affiliated with either.
