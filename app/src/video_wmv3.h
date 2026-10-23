// SPDX-License-Identifier: GPL-3.0-or-later
// From zofo007's NFS-Carbon-NX (GPL-3.0), same USA default.xex; brought into nfscarbon-nx for the
// in-race picture-in-picture videos (boss and crew faces), which the game's own WMV3 decoder plays
// far below their frame rate on the Switch.
#pragma once
// FFmpeg-backed WMV3 (simple/main profile, I/P frames) decoder used to replace the game's slow
// recompiled XDK decoder for cutscenes. ASF parsing here is pure so it can be host-tested.
#include <cstddef>
#include <cstdint>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace carbon::video {

struct MovieInfo {
  int width = 0;
  int height = 0;
  uint8_t sequence[4] = {};  // WMV3 sequence header (BITMAPINFOHEADER extradata)
};

// Finds the first WMV3 video stream in an ASF header object. False when absent or malformed.
bool ParseAsfHeader(const uint8_t* data, size_t size, MovieInfo& out);

// WMV3 MAXBFRAMES: B frames need the game's decoder.
inline int MaxBFrames(const MovieInfo& m) { return (m.sequence[3] >> 4) & 7; }

class Wmv3Decoder {
 public:
  ~Wmv3Decoder();
  bool Open(const MovieInfo& info);
  // Decodes one complete compressed frame. On success planes/strides point at YUV420P data owned
  // by the decoder (valid until the next call).
  bool Decode(const uint8_t* data, size_t size, const uint8_t* planes[3], int strides[3]);
  void Flush();

 private:
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
};

}  // namespace carbon::video
