// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: Vulkan command recording on a thread of its own.
//
// At crowded race starts the GPU command thread spends ~60 ms per frame on ~5,700 draws, 20 ms of it
// inside NVK recording commands (vkCmd*). The recorder takes those calls off it: every vkCmd* of the
// renderer goes through here as a small record in a ring, and a worker thread replays them into the
// command buffers in the same order. The command thread keeps doing everything else (state,
// textures, uploads, descriptor writes); it waits for the worker only when it needs the command
// buffers itself (ending the frame, presenting).
//
// Rules that make this safe:
//  - Between Drain() and the next recorded command the worker is idle: the command thread may touch
//    command buffers directly (begin, end, submit, the presenter's callback).
//  - Records carry copies of everything they point to; handles stay valid because every
//    destruction in the renderer is deferred until the frame's fence, after its Drain().
//  - Only the worker records into a frame's command buffers while it runs, so the command pool is
//    used by one thread at a time.
// carbon_gpu_record_thread = false runs every call at once, on the calling thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/ui/vulkan/api.h>

namespace carbon::gpu {

class CmdRecorder {
 public:
  using PrefetchFn = void (*)(VkPipeline);

  // core >= 0 pins the worker to that core (Switch).
  bool Init(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, bool threaded, int core = -1);
  void Shutdown();
  bool threaded() const { return threaded_; }

  // Waits until every recorded command is in its command buffer.
  void Drain();

  void BindPipeline(VkCommandBuffer cb, VkPipeline pipeline, PrefetchFn prefetch);
  void BindDescriptorSet(VkCommandBuffer cb, VkPipelineLayout layout, uint32_t set,
                         VkDescriptorSet descriptor_set, uint32_t dynamic_count,
                         const uint32_t* dynamic_offsets);
  void SetViewport(VkCommandBuffer cb, const VkViewport& viewport);
  void SetScissor(VkCommandBuffer cb, const VkRect2D& scissor);
  void SetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope);
  void SetBlendConstants(VkCommandBuffer cb, const float blend[4]);
  // Reference, compare mask and write mask, front then back (the order of the renderer's cache).
  void SetStencil(VkCommandBuffer cb, const uint32_t st[6]);
  void BindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
  void Draw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t first_vertex = 0);
  void DrawIndexed(VkCommandBuffer cb, uint32_t index_count);
  void BeginRendering(VkCommandBuffer cb, const VkRenderingInfo& info);
  void EndRendering(VkCommandBuffer cb);
  void ImageBarrier(VkCommandBuffer cb, const VkImageMemoryBarrier& barrier);
  void PushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                     uint32_t offset, uint32_t size, const void* data);
  void ClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                       const VkClearColorValue& color, const VkImageSubresourceRange& range);
  void ClearAttachment(VkCommandBuffer cb, const VkClearAttachment& attachment,
                       const VkClearRect& rect);
  void CopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
                 VkImageLayout dst_layout, uint32_t count, const VkImageCopy* regions);
  void CopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                         uint32_t count, const VkBufferImageCopy* regions);
  void CopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst,
                         uint32_t count, const VkBufferImageCopy* regions);
  void ResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first, uint32_t count);
  void WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage, VkQueryPool pool,
                      uint32_t query);

  // Since the last call: worker busy time, time the command thread waited in Drain(), records.
  struct Stats {
    double busy_s = 0.0;
    double drain_wait_s = 0.0;
    uint64_t records = 0;
    uint64_t wakes = 0;
  };
  Stats TakeStats();

 private:
  enum class Op : uint16_t;
  struct Header {
    Op op;
    uint16_t size;  // whole record, header included, multiple of 8
  };

  void* Reserve(Op op, uint32_t payload);
  void Publish();
  void Execute(const Header* h);
  void Worker();

  struct Fn {
    PFN_vkCmdBindPipeline BindPipeline;
    PFN_vkCmdBindDescriptorSets BindDescriptorSets;
    PFN_vkCmdSetViewport SetViewport;
    PFN_vkCmdSetScissor SetScissor;
    PFN_vkCmdSetDepthBias SetDepthBias;
    PFN_vkCmdSetBlendConstants SetBlendConstants;
    PFN_vkCmdSetStencilReference SetStencilReference;
    PFN_vkCmdSetStencilCompareMask SetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask SetStencilWriteMask;
    PFN_vkCmdBindIndexBuffer BindIndexBuffer;
    PFN_vkCmdDraw Draw;
    PFN_vkCmdDrawIndexed DrawIndexed;
    PFN_vkCmdBeginRendering BeginRendering;
    PFN_vkCmdEndRendering EndRendering;
    PFN_vkCmdPipelineBarrier PipelineBarrier;
    PFN_vkCmdPushConstants PushConstants;
    PFN_vkCmdClearColorImage ClearColorImage;
    PFN_vkCmdClearAttachments ClearAttachments;
    PFN_vkCmdCopyImage CopyImage;
    PFN_vkCmdCopyBufferToImage CopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CopyImageToBuffer;
    PFN_vkCmdResetQueryPool ResetQueryPool;
    PFN_vkCmdWriteTimestamp WriteTimestamp;
  } fn_ = {};

  bool threaded_ = false;
  int core_ = -1;
  // Single producer (the command thread), single consumer (the worker). Offsets grow without
  // wrapping; the position in the ring is offset % kRingSize.
  static constexpr uint32_t kRingSize = 8u << 20;
  std::vector<uint8_t> ring_;
  uint64_t write_ = 0;                    // producer's private write position
  std::atomic<uint64_t> published_{0};    // what the worker may read
  std::atomic<uint64_t> executed_{0};     // what the worker has recorded
  uint64_t notified_at_ = 0;              // published_ when the worker was last woken
  alignas(64) std::atomic<bool> sleeping_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> drain_requested_{false};
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  std::thread thread_;
  // Inline mode (not threaded): one record at a time.
  alignas(16) uint8_t inline_[1024];

  std::atomic<uint64_t> busy_ticks_ns_{0};
  std::atomic<uint64_t> records_{0};
  std::atomic<uint64_t> wakes_{0};
  double drain_wait_s_ = 0.0;
};

}  // namespace carbon::gpu
