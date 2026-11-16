/*
 * ジャケットの JPEG を読む: ESP32-S3 のチップに入っている TJpgDec (ROM) で読みながら、画面の大きさ (最大 maxSide) に
 * 面積の平均で縮めて、RGB565 (ふつうのバイト順) の入れ物へ書く。640px の絵をそのまま置く大きな入れ物は要らない
 * (M5GFX の drawJpg を通すと 640px で 3〜4 秒かかったため、チップのものを使う)
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace spotify {

using CancelFn = bool (*)(void* ctx);  // true を返すと、読むのをやめる (途中で要らなくなったとき)

// jpg を out (maxSide x maxSide まで) に読む。できた大きさ (*w, *h) を返す
bool decodeJpegRom(const uint8_t* jpg, size_t len, uint16_t* out, int maxSide, int* w, int* h, CancelFn cancel = nullptr,
                   void* cancelCtx = nullptr);

}  // namespace spotify
