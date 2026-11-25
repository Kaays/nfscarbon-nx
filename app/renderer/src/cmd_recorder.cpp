// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: Vulkan command recording on a thread of its own (see cmd_recorder.h).
#include "cmd_recorder.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>

#if defined(__SWITCH__)
#include <switch.h>
extern "C" uint64_t RexSwitchUseAllProcessCores(void);
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
extern "C" void RexSwitchRegisterCurrentThread(void);
extern "C" void RexSwitchPerfSetThreadName(u32 handle, const char* name);
#endif

namespace carbon::gpu {

enum class CmdRecorder::Op : uint16_t {
  kPad,
  kBindPipeline,
  kBindSet,
  kViewport,
  kScissor,
  kDepthBias,
  kBlend,
  kStencil,
  kIndexBuffer,
  kDraw,
  kDrawIndexed,
  kBeginRendering,
  kEndRendering,
  kBarrier,
  kPushConstants,
  kClearColorImage,
  kClearAttachment,
  kCopyImage,
  kCopyBufferToImage,
  kCopyImageToBuffer,
  kResetQueryPool,
  kWriteTimestamp,
};

namespace {

// Producer wakes a sleeping worker once this much is waiting (or at Drain): one wake per ~150
// draws instead of one per draw, a wake costing a kernel call on each side.
constexpr uint64_t kWakeBytes = 16u << 10;

struct PBindPipeline {
  VkCommandBuffer cb;
  VkPipeline pipeline;
  CmdRecorder::PrefetchFn prefetch;
};
struct PBindSet {
  VkCommandBuffer cb;
  VkPipelineLayout layout;
  VkDescriptorSet set;
  uint32_t index;
  uint32_t dynamic_count;
  uint32_t dynamic[4];
};
struct PViewport {
  VkCommandBuffer cb;
  VkViewport viewport;
};
struct PScissor {
  VkCommandBuffer cb;
  VkRect2D scissor;
};
struct PDepthBias {
  VkCommandBuffer cb;
  float constant, clamp, slope;
};
struct PBlend {
  VkCommandBuffer cb;
  float blend[4];
};
struct PStencil {
  VkCommandBuffer cb;
  uint32_t st[6];
};
struct PIndexBuffer {
  VkCommandBuffer cb;
  VkBuffer buffer;
  VkDeviceSize offset;
  VkIndexType type;
};
struct PDraw {
  VkCommandBuffer cb;
  uint32_t count, first;
};
struct PBeginRendering {
  VkCommandBuffer cb;
  VkRenderingInfo info;
  VkRenderingAttachmentInfo colors[4];
  VkRenderingAttachmentInfo depth;
  VkRenderingAttachmentInfo stencil;
  bool has_depth, has_stencil;
};
struct PCb {
  VkCommandBuffer cb;
};
struct PBarrier {
  VkCommandBuffer cb;
  VkImageMemoryBarrier barrier;
};
struct PPushConstants {
  VkCommandBuffer cb;
  VkPipelineLayout layout;
  VkShaderStageFlags stages;
  uint32_t offset, size;
  uint8_t data[128];
};
struct PClearColorImage {
  VkCommandBuffer cb;
  VkImage image;
  VkImageLayout layout;
  VkClearColorValue color;
  VkImageSubresourceRange range;
};
struct PClearAttachment {
  VkCommandBuffer cb;
  VkClearAttachment attachment;
  VkClearRect rect;
};
struct PCopyImage {
  VkCommandBuffer cb;
  VkImage src, dst;
  VkImageLayout src_layout, dst_layout;
  uint32_t count;
  VkImageCopy regions[1];  // count of them
};
struct PCopyBufferImage {
  VkCommandBuffer cb;
  VkBuffer buffer;
  VkImage image;
  VkImageLayout layout;
  uint32_t count;
  VkBufferImageCopy regions[1];  // count of them
};
struct PResetQueryPool {
  VkCommandBuffer cb;
  VkQueryPool pool;
  uint32_t first, count;
};
struct PWriteTimestamp {
  VkCommandBuffer cb;
  VkPipelineStageFlagBits stage;
  VkQueryPool pool;
  uint32_t query;
};

constexpr uint32_t Align8(uint32_t v) { return (v + 7u) & ~7u; }

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

template <typename T>
T* Payload(void* record) {
  return reinterpret_cast<T*>(static_cast<uint8_t*>(record));
}

}  // namespace

bool CmdRecorder::Init(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, bool threaded) {
#define LOAD(name)                                                                          \
  fn_.name = reinterpret_cast<PFN_vkCmd##name>(gdpa(device, "vkCmd" #name));                 \
  if (!fn_.name) return false;
  LOAD(BindPipeline)
  LOAD(BindDescriptorSets)
  LOAD(SetViewport)
  LOAD(SetScissor)
  LOAD(SetDepthBias)
  LOAD(SetBlendConstants)
  LOAD(SetStencilReference)
  LOAD(SetStencilCompareMask)
  LOAD(SetStencilWriteMask)
  LOAD(BindIndexBuffer)
  LOAD(Draw)
  LOAD(DrawIndexed)
  LOAD(BeginRendering)
  LOAD(EndRendering)
  LOAD(PipelineBarrier)
  LOAD(PushConstants)
  LOAD(ClearColorImage)
  LOAD(ClearAttachments)
  LOAD(CopyImage)
  LOAD(CopyBufferToImage)
  LOAD(CopyImageToBuffer)
  LOAD(ResetQueryPool)
  LOAD(WriteTimestamp)
#undef LOAD
  threaded_ = threaded;
  if (threaded_) {
    ring_.assign(kRingSize, 0);
    thread_ = std::thread([this]() { Worker(); });
  }
  return true;
}

void CmdRecorder::Shutdown() {
  if (!threaded_) {
    return;
  }
  Drain();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_.store(true);
  }
  work_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  threaded_ = false;
}

void* CmdRecorder::Reserve(Op op, uint32_t payload) {
  const uint32_t size = Align8(uint32_t(sizeof(Header)) + payload);
  Header* h;
  if (!threaded_) {
    h = reinterpret_cast<Header*>(inline_);
  } else {
    // A record never straddles the end of the ring: pad to the end first.
    uint32_t pos = uint32_t(write_ % kRingSize);
    if (pos + size > kRingSize) {
      const uint32_t pad = kRingSize - pos;
      // Room for the pad and the record.
      while (write_ + pad + size - executed_.load(std::memory_order_acquire) > kRingSize) {
        Drain();
      }
      Header* p = reinterpret_cast<Header*>(ring_.data() + pos);
      p->op = Op::kPad;
      p->size = 0;  // pad: the rest of the ring
      write_ += pad;
      pos = 0;
    }
    while (write_ + size - executed_.load(std::memory_order_acquire) > kRingSize) {
      Drain();
    }
    h = reinterpret_cast<Header*>(ring_.data() + pos);
  }
  h->op = op;
  h->size = uint16_t(size);
  return reinterpret_cast<uint8_t*>(h) + sizeof(Header);
}

void CmdRecorder::Publish() {
  if (!threaded_) {
    Execute(reinterpret_cast<const Header*>(inline_));
    return;
  }
  const Header* h = reinterpret_cast<const Header*>(ring_.data() + write_ % kRingSize);
  write_ += h->size;
  published_.store(write_, std::memory_order_seq_cst);
  if (sleeping_.load(std::memory_order_seq_cst) &&
      write_ - executed_.load(std::memory_order_relaxed) >= kWakeBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    work_cv_.notify_one();
  }
}

void CmdRecorder::Drain() {
  if (!threaded_ || executed_.load(std::memory_order_acquire) == write_) {
    return;
  }
  const uint64_t t0 = NowNs();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    work_cv_.notify_one();
    done_cv_.wait(lock, [this] { return executed_.load(std::memory_order_acquire) == write_; });
  }
  drain_wait_s_ += double(NowNs() - t0) * 1e-9;
}

void CmdRecorder::Worker() {
#if defined(__SWITCH__)
  RexSwitchUseAllProcessCores();
  RexSwitchSetCurrentThreadPriorityOk(0x2C);  // the GPU command thread's priority
  RexSwitchRegisterCurrentThread();
  const u32 handle = threadGetCurHandle();
#endif
  uint64_t read = 0;
  uint64_t wakes = 0;
  for (;;) {
    uint64_t pub = published_.load(std::memory_order_acquire);
    if (read == pub) {
      std::unique_lock<std::mutex> lock(mutex_);
      sleeping_.store(true, std::memory_order_seq_cst);
      pub = published_.load(std::memory_order_seq_cst);
      if (read == pub) {
        done_cv_.notify_all();
        if (stop_.load()) {
          sleeping_.store(false);
          return;
        }
        work_cv_.wait(lock, [&] {
          return stop_.load() || published_.load(std::memory_order_acquire) != read;
        });
      }
      sleeping_.store(false, std::memory_order_seq_cst);
      ++wakes;
      wakes_.fetch_add(1, std::memory_order_relaxed);
#if defined(__SWITCH__)
      if ((wakes & 255) == 1) {
        RexSwitchPerfSetThreadName(handle, "Vulkan recorder");
      }
#endif
      continue;
    }
    const uint64_t t0 = NowNs();
    uint64_t n = 0;
    while (read != pub) {
      const uint32_t pos = uint32_t(read % kRingSize);
      const Header* h = reinterpret_cast<const Header*>(ring_.data() + pos);
      if (h->op == Op::kPad) {
        read += kRingSize - pos;
      } else {
        Execute(h);
        read += h->size;
        ++n;
      }
      executed_.store(read, std::memory_order_release);
    }
    busy_ticks_ns_.fetch_add(NowNs() - t0, std::memory_order_relaxed);
    records_.fetch_add(n, std::memory_order_relaxed);
  }
}

CmdRecorder::Stats CmdRecorder::TakeStats() {
  Stats s;
  s.busy_s = double(busy_ticks_ns_.exchange(0, std::memory_order_relaxed)) * 1e-9;
  s.records = records_.exchange(0, std::memory_order_relaxed);
  s.wakes = wakes_.exchange(0, std::memory_order_relaxed);
  s.drain_wait_s = drain_wait_s_;
  drain_wait_s_ = 0.0;
  return s;
}

void CmdRecorder::Execute(const Header* h) {
  const void* p = reinterpret_cast<const uint8_t*>(h) + sizeof(Header);
  switch (h->op) {
    case Op::kPad:
      break;
    case Op::kBindPipeline: {
      auto* a = static_cast<const PBindPipeline*>(p);
      if (a->prefetch) a->prefetch(a->pipeline);
      fn_.BindPipeline(a->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a->pipeline);
      break;
    }
    case Op::kBindSet: {
      auto* a = static_cast<const PBindSet*>(p);
      fn_.BindDescriptorSets(a->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, a->layout, a->index, 1, &a->set,
                             a->dynamic_count, a->dynamic_count ? a->dynamic : nullptr);
      break;
    }
    case Op::kViewport: {
      auto* a = static_cast<const PViewport*>(p);
      fn_.SetViewport(a->cb, 0, 1, &a->viewport);
      break;
    }
    case Op::kScissor: {
      auto* a = static_cast<const PScissor*>(p);
      fn_.SetScissor(a->cb, 0, 1, &a->scissor);
      break;
    }
    case Op::kDepthBias: {
      auto* a = static_cast<const PDepthBias*>(p);
      fn_.SetDepthBias(a->cb, a->constant, a->clamp, a->slope);
      break;
    }
    case Op::kBlend: {
      auto* a = static_cast<const PBlend*>(p);
      fn_.SetBlendConstants(a->cb, a->blend);
      break;
    }
    case Op::kStencil: {
      auto* a = static_cast<const PStencil*>(p);
      fn_.SetStencilReference(a->cb, VK_STENCIL_FACE_FRONT_BIT, a->st[0]);
      fn_.SetStencilReference(a->cb, VK_STENCIL_FACE_BACK_BIT, a->st[1]);
      fn_.SetStencilCompareMask(a->cb, VK_STENCIL_FACE_FRONT_BIT, a->st[2]);
      fn_.SetStencilCompareMask(a->cb, VK_STENCIL_FACE_BACK_BIT, a->st[3]);
      fn_.SetStencilWriteMask(a->cb, VK_STENCIL_FACE_FRONT_BIT, a->st[4]);
      fn_.SetStencilWriteMask(a->cb, VK_STENCIL_FACE_BACK_BIT, a->st[5]);
      break;
    }
    case Op::kIndexBuffer: {
      auto* a = static_cast<const PIndexBuffer*>(p);
      fn_.BindIndexBuffer(a->cb, a->buffer, a->offset, a->type);
      break;
    }
    case Op::kDraw: {
      auto* a = static_cast<const PDraw*>(p);
      fn_.Draw(a->cb, a->count, 1, a->first, 0);
      break;
    }
    case Op::kDrawIndexed: {
      auto* a = static_cast<const PDraw*>(p);
      fn_.DrawIndexed(a->cb, a->count, 1, 0, 0, 0);
      break;
    }
    case Op::kBeginRendering: {
      auto* a = static_cast<const PBeginRendering*>(p);
      VkRenderingInfo info = a->info;
      info.pColorAttachments = info.colorAttachmentCount ? a->colors : nullptr;
      info.pDepthAttachment = a->has_depth ? &a->depth : nullptr;
      info.pStencilAttachment = a->has_stencil ? &a->stencil : nullptr;
      fn_.BeginRendering(a->cb, &info);
      break;
    }
    case Op::kEndRendering:
      fn_.EndRendering(static_cast<const PCb*>(p)->cb);
      break;
    case Op::kBarrier: {
      auto* a = static_cast<const PBarrier*>(p);
      fn_.PipelineBarrier(a->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                          &a->barrier);
      break;
    }
    case Op::kPushConstants: {
      auto* a = static_cast<const PPushConstants*>(p);
      fn_.PushConstants(a->cb, a->layout, a->stages, a->offset, a->size, a->data);
      break;
    }
    case Op::kClearColorImage: {
      auto* a = static_cast<const PClearColorImage*>(p);
      fn_.ClearColorImage(a->cb, a->image, a->layout, &a->color, 1, &a->range);
      break;
    }
    case Op::kClearAttachment: {
      auto* a = static_cast<const PClearAttachment*>(p);
      fn_.ClearAttachments(a->cb, 1, &a->attachment, 1, &a->rect);
      break;
    }
    case Op::kCopyImage: {
      auto* a = static_cast<const PCopyImage*>(p);
      fn_.CopyImage(a->cb, a->src, a->src_layout, a->dst, a->dst_layout, a->count, a->regions);
      break;
    }
    case Op::kCopyBufferToImage: {
      auto* a = static_cast<const PCopyBufferImage*>(p);
      fn_.CopyBufferToImage(a->cb, a->buffer, a->image, a->layout, a->count, a->regions);
      break;
    }
    case Op::kCopyImageToBuffer: {
      auto* a = static_cast<const PCopyBufferImage*>(p);
      fn_.CopyImageToBuffer(a->cb, a->image, a->layout, a->buffer, a->count, a->regions);
      break;
    }
    case Op::kResetQueryPool: {
      auto* a = static_cast<const PResetQueryPool*>(p);
      fn_.ResetQueryPool(a->cb, a->pool, a->first, a->count);
      break;
    }
    case Op::kWriteTimestamp: {
      auto* a = static_cast<const PWriteTimestamp*>(p);
      fn_.WriteTimestamp(a->cb, a->stage, a->pool, a->query);
      break;
    }
  }
}

// ---- Recording ----------------------------------------------------------------------------------

void CmdRecorder::BindPipeline(VkCommandBuffer cb, VkPipeline pipeline, PrefetchFn prefetch) {
  auto* a = Payload<PBindPipeline>(Reserve(Op::kBindPipeline, sizeof(PBindPipeline)));
  a->cb = cb;
  a->pipeline = pipeline;
  a->prefetch = prefetch;
  Publish();
}

void CmdRecorder::BindDescriptorSet(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t set,
                                    VkDescriptorSet descriptor_set, uint32_t dynamic_count,
                                    const uint32_t* dynamic_offsets) {
  auto* a = Payload<PBindSet>(Reserve(Op::kBindSet, sizeof(PBindSet)));
  a->cb = cb;
  a->layout = layout;
  a->set = descriptor_set;
  a->index = set;
  a->dynamic_count = std::min<uint32_t>(dynamic_count, 4);
  for (uint32_t i = 0; i < a->dynamic_count; ++i) a->dynamic[i] = dynamic_offsets[i];
  Publish();
}

void CmdRecorder::SetViewport(VkCommandBuffer cb, const VkViewport& viewport) {
  auto* a = Payload<PViewport>(Reserve(Op::kViewport, sizeof(PViewport)));
  a->cb = cb;
  a->viewport = viewport;
  Publish();
}

void CmdRecorder::SetScissor(VkCommandBuffer cb, const VkRect2D& scissor) {
  auto* a = Payload<PScissor>(Reserve(Op::kScissor, sizeof(PScissor)));
  a->cb = cb;
  a->scissor = scissor;
  Publish();
}

void CmdRecorder::SetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
  auto* a = Payload<PDepthBias>(Reserve(Op::kDepthBias, sizeof(PDepthBias)));
  a->cb = cb;
  a->constant = constant;
  a->clamp = clamp;
  a->slope = slope;
  Publish();
}

void CmdRecorder::SetBlendConstants(VkCommandBuffer cb, const float blend[4]) {
  auto* a = Payload<PBlend>(Reserve(Op::kBlend, sizeof(PBlend)));
  a->cb = cb;
  std::memcpy(a->blend, blend, sizeof(a->blend));
  Publish();
}

void CmdRecorder::SetStencil(VkCommandBuffer cb, const uint32_t st[6]) {
  auto* a = Payload<PStencil>(Reserve(Op::kStencil, sizeof(PStencil)));
  a->cb = cb;
  std::memcpy(a->st, st, sizeof(a->st));
  Publish();
}

void CmdRecorder::BindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset,
                                  VkIndexType type) {
  auto* a = Payload<PIndexBuffer>(Reserve(Op::kIndexBuffer, sizeof(PIndexBuffer)));
  a->cb = cb;
  a->buffer = buffer;
  a->offset = offset;
  a->type = type;
  Publish();
}

void CmdRecorder::Draw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t first_vertex) {
  auto* a = Payload<PDraw>(Reserve(Op::kDraw, sizeof(PDraw)));
  a->cb = cb;
  a->count = vertex_count;
  a->first = first_vertex;
  Publish();
}

void CmdRecorder::DrawIndexed(VkCommandBuffer cb, uint32_t index_count) {
  auto* a = Payload<PDraw>(Reserve(Op::kDrawIndexed, sizeof(PDraw)));
  a->cb = cb;
  a->count = index_count;
  a->first = 0;
  Publish();
}

void CmdRecorder::BeginRendering(VkCommandBuffer cb, const VkRenderingInfo& info) {
  auto* a = Payload<PBeginRendering>(Reserve(Op::kBeginRendering, sizeof(PBeginRendering)));
  a->cb = cb;
  a->info = info;
  a->info.pNext = nullptr;
  a->info.colorAttachmentCount = std::min<uint32_t>(info.colorAttachmentCount, 4);
  for (uint32_t i = 0; i < a->info.colorAttachmentCount; ++i) {
    a->colors[i] = info.pColorAttachments[i];
  }
  a->has_depth = info.pDepthAttachment != nullptr;
  if (a->has_depth) a->depth = *info.pDepthAttachment;
  a->has_stencil = info.pStencilAttachment != nullptr;
  if (a->has_stencil) a->stencil = *info.pStencilAttachment;
  Publish();
}

void CmdRecorder::EndRendering(VkCommandBuffer cb) {
  Payload<PCb>(Reserve(Op::kEndRendering, sizeof(PCb)))->cb = cb;
  Publish();
}

void CmdRecorder::ImageBarrier(VkCommandBuffer cb, const VkImageMemoryBarrier& barrier) {
  auto* a = Payload<PBarrier>(Reserve(Op::kBarrier, sizeof(PBarrier)));
  a->cb = cb;
  a->barrier = barrier;
  a->barrier.pNext = nullptr;
  Publish();
}

void CmdRecorder::PushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
                                VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                const void* data) {
  auto* a = Payload<PPushConstants>(Reserve(Op::kPushConstants, sizeof(PPushConstants)));
  a->cb = cb;
  a->layout = layout;
  a->stages = stages;
  a->offset = offset;
  a->size = std::min<uint32_t>(size, sizeof(a->data));
  std::memcpy(a->data, data, a->size);
  Publish();
}

void CmdRecorder::ClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                                  const VkClearColorValue& color,
                                  const VkImageSubresourceRange& range) {
  auto* a = Payload<PClearColorImage>(Reserve(Op::kClearColorImage, sizeof(PClearColorImage)));
  a->cb = cb;
  a->image = image;
  a->layout = layout;
  a->color = color;
  a->range = range;
  Publish();
}

void CmdRecorder::ClearAttachment(VkCommandBuffer cb, const VkClearAttachment& attachment,
                                  const VkClearRect& rect) {
  auto* a = Payload<PClearAttachment>(Reserve(Op::kClearAttachment, sizeof(PClearAttachment)));
  a->cb = cb;
  a->attachment = attachment;
  a->rect = rect;
  Publish();
}

void CmdRecorder::CopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                            VkImageLayout dst_layout, uint32_t count, const VkImageCopy* regions) {
  constexpr uint32_t kMax = 10;
  for (uint32_t done = 0; done < count;) {
    const uint32_t n = std::min(count - done, kMax);
    const uint32_t bytes = uint32_t(offsetof(PCopyImage, regions) + n * sizeof(VkImageCopy));
    auto* a = Payload<PCopyImage>(Reserve(Op::kCopyImage, bytes));
    a->cb = cb;
    a->src = src;
    a->dst = dst;
    a->src_layout = src_layout;
    a->dst_layout = dst_layout;
    a->count = n;
    std::memcpy(a->regions, regions + done, n * sizeof(VkImageCopy));
    Publish();
    done += n;
  }
}

void CmdRecorder::CopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                    VkImageLayout layout, uint32_t count,
                                    const VkBufferImageCopy* regions) {
  constexpr uint32_t kMax = 14;
  for (uint32_t done = 0; done < count;) {
    const uint32_t n = std::min(count - done, kMax);
    const uint32_t bytes =
        uint32_t(offsetof(PCopyBufferImage, regions) + n * sizeof(VkBufferImageCopy));
    auto* a = Payload<PCopyBufferImage>(Reserve(Op::kCopyBufferToImage, bytes));
    a->cb = cb;
    a->buffer = src;
    a->image = dst;
    a->layout = layout;
    a->count = n;
    std::memcpy(a->regions, regions + done, n * sizeof(VkBufferImageCopy));
    Publish();
    done += n;
  }
}

void CmdRecorder::CopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout,
                                    VkBuffer dst, uint32_t count,
                                    const VkBufferImageCopy* regions) {
  constexpr uint32_t kMax = 14;
  for (uint32_t done = 0; done < count;) {
    const uint32_t n = std::min(count - done, kMax);
    const uint32_t bytes =
        uint32_t(offsetof(PCopyBufferImage, regions) + n * sizeof(VkBufferImageCopy));
    auto* a = Payload<PCopyBufferImage>(Reserve(Op::kCopyImageToBuffer, bytes));
    a->cb = cb;
    a->buffer = dst;
    a->image = src;
    a->layout = layout;
    a->count = n;
    std::memcpy(a->regions, regions + done, n * sizeof(VkBufferImageCopy));
    Publish();
    done += n;
  }
}

void CmdRecorder::ResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first,
                                 uint32_t count) {
  auto* a = Payload<PResetQueryPool>(Reserve(Op::kResetQueryPool, sizeof(PResetQueryPool)));
  a->cb = cb;
  a->pool = pool;
  a->first = first;
  a->count = count;
  Publish();
}

void CmdRecorder::WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage,
                                 VkQueryPool pool, uint32_t query) {
  auto* a = Payload<PWriteTimestamp>(Reserve(Op::kWriteTimestamp, sizeof(PWriteTimestamp)));
  a->cb = cb;
  a->stage = stage;
  a->pool = pool;
  a->query = query;
  Publish();
}

}  // namespace carbon::gpu
