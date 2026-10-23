// SPDX-License-Identifier: GPL-3.0-or-later
// From zofo007's NFS-Carbon-NX (GPL-3.0), same USA default.xex; brought into nfscarbon-nx for the
// in-race picture-in-picture videos (boss and crew faces), which the game's own WMV3 decoder plays
// far below their frame rate on the Switch.
#include "video_wmv3.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace carbon::video {
namespace {

constexpr uint8_t kHeaderGuid[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                     0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kStreamGuid[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                     0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kVideoGuid[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint64_t Le64(const uint8_t* p) { return Le32(p) | (uint64_t(Le32(p + 4)) << 32); }

// Stream properties: type GUID(16) errcorr GUID(16) timeoffset(8) typeDataLen(4) errLen(4) flags(2) reserved(4) typeData.
// Video type data: width(4) height(4) reserved(1) formatDataSize(2) BITMAPINFOHEADER(40) extradata.
bool ParseVideoStream(const uint8_t* p, size_t n, MovieInfo& out) {
  if (n < 54 || std::memcmp(p, kVideoGuid, 16) != 0) return false;
  const uint32_t type_len = Le32(p + 40);
  if (type_len < 11 + 40 + 4 || 54 + size_t(type_len) > n) return false;
  const uint8_t* t = p + 54;
  const uint8_t* bih = t + 11;
  if (std::memcmp(bih + 16, "WMV3", 4) != 0) return false;
  const int w = int(Le32(bih + 4)), h = int(Le32(bih + 8));
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || Le32(bih) < 44) return false;
  out.width = w;
  out.height = h;
  std::memcpy(out.sequence, bih + 40, 4);
  return true;
}

}  // namespace

bool ParseAsfHeader(const uint8_t* d, size_t size, MovieInfo& out) {
  if (size < 30 || std::memcmp(d, kHeaderGuid, 16) != 0) return false;
  const uint64_t total = Le64(d + 16);
  const size_t end = total < size ? size_t(total) : size;
  size_t pos = 30;
  while (pos + 24 <= end) {
    const uint64_t len = Le64(d + pos + 16);
    if (len < 24 || pos + len > end) return false;
    if (std::memcmp(d + pos, kStreamGuid, 16) == 0 &&
        ParseVideoStream(d + pos + 24, size_t(len) - 24, out)) {
      return true;
    }
    pos += size_t(len);
  }
  return false;
}

Wmv3Decoder::~Wmv3Decoder() {
  av_packet_free(&packet_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
}

bool Wmv3Decoder::Open(const MovieInfo& info) {
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!codec) return false;
  ctx_ = avcodec_alloc_context3(codec);
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (!ctx_ || !frame_ || !packet_) return false;
  ctx_->width = info.width;
  ctx_->height = info.height;
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  ctx_->thread_count = 1;
  ctx_->extradata = static_cast<uint8_t*>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!ctx_->extradata) return false;
  std::memcpy(ctx_->extradata, info.sequence, 4);
  ctx_->extradata_size = 4;
  return avcodec_open2(ctx_, codec, nullptr) == 0;
}

bool Wmv3Decoder::Decode(const uint8_t* data, size_t size, const uint8_t* planes[3], int strides[3]) {
  if (!ctx_ || av_new_packet(packet_, int(size)) < 0) return false;
  std::memcpy(packet_->data, data, size);
  const int sent = avcodec_send_packet(ctx_, packet_);
  av_packet_unref(packet_);
  if (sent < 0 || avcodec_receive_frame(ctx_, frame_) < 0) return false;
  if (frame_->format != AV_PIX_FMT_YUV420P) return false;
  for (int i = 0; i < 3; ++i) {
    planes[i] = frame_->data[i];
    strides[i] = frame_->linesize[i];
  }
  return true;
}

void Wmv3Decoder::Flush() {
  if (ctx_) avcodec_flush_buffers(ctx_);
}

}  // namespace carbon::video
