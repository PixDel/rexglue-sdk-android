// Frame generation (Android): Lossless Scaling Frame Generation through lsfg-vk's framegen library
// (GameNative/lsfg-vk-android, MIT) on the presenter's own device and queue.
//
// For every new guest frame, the frame between the previous real frame and this one is generated
// from the guest output (1280x720, cheap: before the upscale to the screen) and painted first; the
// real frame follows on the next refresh of the display (FIFO present). The shaders come from the
// user's own Lossless.dll (frame_generation_dll), read and translated at runtime.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/presenter.h>

REXCVAR_DEFINE_INT32(frame_generation, 0, "UI/Presenter",
                     "Frame generation (Android, Lossless Scaling): 0 off, 2 = one generated frame "
                     "before every real one. Needs Lossless.dll (frame_generation_dll)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(frame_generation_performance, true, "UI/Presenter",
                    "Frame generation: Lossless Scaling's performance mode (lighter, a bit softer)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(frame_generation_flow_scale, 1.0, "UI/Presenter",
                      "Frame generation: motion estimation resolution, 0.25 to 1 (lower is faster)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(frame_generation_dll, "", "UI/Presenter",
                      "Path of Lossless.dll for frame generation; empty: Lossless.dll in the app's "
                      "files folder (Android/data/<app>/files)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

#if defined(REX_HAS_LSFG) && REX_HAS_LSFG
#include <lsfg_3_1.hpp>
#include <lsfg_3_1p.hpp>

#include "config/config.hpp"
#include "extract/extract.hpp"
#include "extract/mipmaps_replacement_spv.h"
#include "extract/trans.hpp"

#if REX_PLATFORM_ANDROID
#include <SDL3/SDL_system.h>
#endif
#endif

namespace rex {
namespace ui {
namespace vulkan {

#if defined(REX_HAS_LSFG) && REX_HAS_LSFG

struct VulkanPresenter::FrameGeneration {
  const VulkanDevice* device = nullptr;
  bool performance = true;
  int32_t context = -1;
  VkExtent2D extent{};
  std::unique_ptr<GuestOutputImage> inputs[2];
  std::shared_ptr<GuestOutputImage> output;
  bool output_general = false;
  uint64_t frames = 0;       // real frames given to the current context
  uint64_t last_serial = 0;  // refresh_serial of the last real frame taken
  std::shared_ptr<GuestOutputImage> pending_real;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer pre = VK_NULL_HANDLE, post = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool fence_pending = false;
  bool library_initialized = false;

  void WaitFence() {
    if (!fence_pending) return;
    const VulkanDevice::Functions& dfn = device->functions();
    dfn.vkWaitForFences(device->device(), 1, &fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(device->device(), 1, &fence);
    fence_pending = false;
  }

  void DeleteContext() {
    if (context < 0) return;
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    try {
      if (performance) {
        LSFG_3_1P::deleteContext(context);
      } else {
        LSFG_3_1::deleteContext(context);
      }
    } catch (const std::exception& e) {
      REXLOG_WARN("Frame generation: deleting the context failed: {}", e.what());
    }
    context = -1;
    frames = 0;
  }

  ~FrameGeneration() {
    WaitFence();
    DeleteContext();
    if (library_initialized) {
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          device->AcquireQueue(device->queue_family_graphics_compute(), 0);
      try {
        if (performance) {
          LSFG_3_1P::finalize();
        } else {
          LSFG_3_1::finalize();
        }
      } catch (...) {
      }
    }
    const VulkanDevice::Functions& dfn = device->functions();
    if (fence != VK_NULL_HANDLE) dfn.vkDestroyFence(device->device(), fence, nullptr);
    if (pool != VK_NULL_HANDLE) dfn.vkDestroyCommandPool(device->device(), pool, nullptr);
  }
};

namespace {

std::string FrameGenerationDllPath() {
  std::string path = REXCVAR_GET(frame_generation_dll);
#if REX_PLATFORM_ANDROID
  if (path.empty()) {
    if (const char* external = SDL_GetAndroidExternalStoragePath()) {
      path = std::string(external) + "/Lossless.dll";
    }
  }
#endif
  return path;
}

void ImageBarrier(const VulkanDevice::Functions& dfn, VkCommandBuffer cb, VkImage image,
                  VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                  VkImageLayout old_layout, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access, VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  dfn.vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool g_frame_generation_failed = false;  // couldn't start: don't retry every frame

}  // namespace

std::shared_ptr<VulkanPresenter::GuestOutputImage> VulkanPresenter::FrameGenerationSubstitute(
    const std::shared_ptr<GuestOutputImage>& real, uint64_t serial) {
  const int32_t multiplier = REXCVAR_GET(frame_generation);
  if (multiplier < 2) {
    if (frame_generation_) {
      frame_generation_.reset();
      REXLOG_INFO("Frame generation: off");
    }
    frame_generation_paint_again_ = false;
    return real;
  }
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  const uint32_t family = vulkan_device_->queue_family_graphics_compute();

  if (!frame_generation_) {
    if (g_frame_generation_failed) return real;
    std::unique_ptr<FrameGeneration, void (*)(FrameGeneration*)> fg(
        new FrameGeneration, [](FrameGeneration* p) { delete p; });
    fg->device = vulkan_device_;
    fg->performance = REXCVAR_GET(frame_generation_performance);
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = family;
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn.vkCreateCommandPool(device, &pool_info, nullptr, &fg->pool) != VK_SUCCESS ||
        dfn.vkCreateFence(device, &fence_info, nullptr, &fg->fence) != VK_SUCCESS) {
      g_frame_generation_failed = true;
      return real;
    }
    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = fg->pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    dfn.vkAllocateCommandBuffers(device, &alloc_info, &fg->pre);
    dfn.vkAllocateCommandBuffers(device, &alloc_info, &fg->post);

    // The library on this device and queue, its shaders from Lossless.dll.
    const std::string dll = FrameGenerationDllPath();
    Config::activeConf.dll = dll;
    try {
      Extract::extractShaders();
      const auto loader = [](const std::string& name) -> std::vector<uint8_t> {
        if (name == "mipmaps" || name == "p_mipmaps") {
          // Mesa miscompiles the translated mipmaps shader on Adreno; lsfg-vk-android has a
          // replacement.
          return std::vector<uint8_t>(mipmaps_replacement_spv,
                                      mipmaps_replacement_spv + mipmaps_replacement_spv_len);
        }
        return Extract::translateShader(Extract::getShader(name));
      };
      const VulkanInstance* instance = vulkan_device_->vulkan_instance();
      const VkQueue queue = vulkan_device_->queue_families()[family].queues[0]->queue;
      const float flow_scale =
          std::clamp(float(REXCVAR_GET(frame_generation_flow_scale)), 0.25f, 1.0f);
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device_->AcquireQueue(family, 0);
      if (fg->performance) {
        LSFG_3_1P::initializeExternal(instance->functions().vkGetInstanceProcAddr,
                                      instance->instance(), vulkan_device_->physical_device(),
                                      device, family, queue, false, 1.0f / flow_scale,
                                      uint64_t(multiplier - 1), loader);
      } else {
        LSFG_3_1::initializeExternal(instance->functions().vkGetInstanceProcAddr,
                                     instance->instance(), vulkan_device_->physical_device(),
                                     device, family, queue, false, 1.0f / flow_scale,
                                     uint64_t(multiplier - 1), loader);
      }
      fg->library_initialized = true;
    } catch (const std::exception& e) {
      REXLOG_ERROR("Frame generation: can't start ({}): {}", dll, e.what());
      g_frame_generation_failed = true;
      return real;
    }
    REXLOG_INFO("Frame generation: on, {}x, {} mode, shaders from {}", multiplier,
                fg->performance ? "performance" : "quality", dll);
    frame_generation_ = std::move(fg);
  }
  FrameGeneration& fg = *frame_generation_;

  // The second paint of a frame: the real one, after the generated one.
  if (fg.pending_real) {
    std::shared_ptr<GuestOutputImage> pending = std::move(fg.pending_real);
    fg.pending_real.reset();
    frame_generation_paint_again_ = false;
    return pending;
  }
  // A repaint without a new guest frame (a UI change): as it is.
  if (!serial || serial == fg.last_serial) return real;
  fg.last_serial = serial;
  {
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count());
    if (frame_generation_last_real_ns_) {
      const uint64_t interval = now - frame_generation_last_real_ns_;
      if (interval < 200000000ull) {
        frame_generation_interval_ns_ =
            frame_generation_interval_ns_
                ? (frame_generation_interval_ns_ * 7 + interval) / 8
                : interval;
      }
    }
    frame_generation_last_real_ns_ = now;
  }

  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;

  const VkExtent2D extent = real->extent();
  constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;  // what lsfg-vk uses on Android
  if (fg.context < 0 || extent.width != fg.extent.width || extent.height != fg.extent.height) {
    fg.WaitFence();
    fg.DeleteContext();
    fg.extent = extent;
    fg.inputs[0] = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, kFormat);
    fg.inputs[1] = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, kFormat);
    fg.output = GuestOutputImage::Create(vulkan_device_, extent.width, extent.height, kFormat);
    if (!fg.inputs[0] || !fg.inputs[1] || !fg.output) {
      REXLOG_ERROR("Frame generation: can't create its {}x{} images", extent.width, extent.height);
      g_frame_generation_failed = true;
      frame_generation_.reset();
      return real;
    }
    // lsfg wants its images in GENERAL from the start.
    dfn.vkResetCommandPool(device, fg.pool, 0);
    dfn.vkBeginCommandBuffer(fg.pre, &begin);
    for (VkImage image : {fg.inputs[0]->image(), fg.inputs[1]->image(), fg.output->image()}) {
      ImageBarrier(dfn, fg.pre, image, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                   VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_GENERAL);
    }
    dfn.vkEndCommandBuffer(fg.pre);
    submit.pCommandBuffers = &fg.pre;
    {
      const VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device_->AcquireQueue(family, 0);
      dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit, fg.fence);
      fg.fence_pending = true;
      try {
        const std::vector<VkImage> outputs = {fg.output->image()};
        fg.context = fg.performance
                         ? LSFG_3_1P::createContextFromImages(fg.inputs[0]->image(),
                                                              fg.inputs[1]->image(), outputs,
                                                              extent, kFormat)
                         : LSFG_3_1::createContextFromImages(fg.inputs[0]->image(),
                                                             fg.inputs[1]->image(), outputs,
                                                             extent, kFormat);
      } catch (const std::exception& e) {
        REXLOG_ERROR("Frame generation: can't create the context: {}", e.what());
        g_frame_generation_failed = true;
        frame_generation_.reset();
        return real;
      }
    }
    fg.WaitFence();
    fg.output_general = true;
    fg.frames = 0;
    REXLOG_INFO("Frame generation: context for {}x{}", extent.width, extent.height);
  }

  // 1. The new real frame into the input lsfg reads as the newest (the frame count's parity).
  fg.WaitFence();
  dfn.vkResetCommandPool(device, fg.pool, 0);
  dfn.vkBeginCommandBuffer(fg.pre, &begin);
  GuestOutputImage& input = *fg.inputs[fg.frames % 2];
  ImageBarrier(dfn, fg.pre, real->image(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_WRITE_BIT, kGuestOutputInternalLayout,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  ImageBarrier(dfn, fg.pre, input.image(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  if (!fg.output_general) {
    // The previous generated frame may still be being painted (earlier in this queue).
    ImageBarrier(dfn, fg.pre, fg.output->image(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL);
    fg.output_general = true;
  }
  VkImageBlit blit = {};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
  dfn.vkCmdBlitImage(fg.pre, real->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input.image(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
  ImageBarrier(dfn, fg.pre, real->image(), VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kGuestOutputInternalStageMask,
               kGuestOutputInternalAccessMask, kGuestOutputInternalLayout);
  ImageBarrier(dfn, fg.pre, input.image(), VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
               VK_IMAGE_LAYOUT_GENERAL);
  dfn.vkEndCommandBuffer(fg.pre);

  // 2. Generate (lsfg submits on this queue itself: hold its lock), 3. the output to sampling.
  bool generated = false;
  {
    const VulkanDevice::Queue::Acquisition queue_acquisition =
        vulkan_device_->AcquireQueue(family, 0);
    submit.pCommandBuffers = &fg.pre;
    if (dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
      return real;
    }
    try {
      if (fg.performance) {
        LSFG_3_1P::presentContext(fg.context, -1, {});
      } else {
        LSFG_3_1::presentContext(fg.context, -1, {});
      }
    } catch (const std::exception& e) {
      REXLOG_ERROR("Frame generation: generating failed, off: {}", e.what());
      g_frame_generation_failed = true;
      frame_generation_.reset();
      return real;
    }
    ++fg.frames;
    // With two real frames in, the output holds the one between them.
    generated = fg.frames >= 2;
    dfn.vkBeginCommandBuffer(fg.post, &begin);
    ImageBarrier(dfn, fg.post, fg.output->image(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    dfn.vkEndCommandBuffer(fg.post);
    submit.pCommandBuffers = &fg.post;
    dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit, fg.fence);
    fg.fence_pending = true;
    fg.output_general = false;
  }
  if (!generated) return real;
  fg.pending_real = real;
  frame_generation_paint_again_ = true;
  return fg.output;
}

#else  // !REX_HAS_LSFG

struct VulkanPresenter::FrameGeneration {};

std::shared_ptr<VulkanPresenter::GuestOutputImage> VulkanPresenter::FrameGenerationSubstitute(
    const std::shared_ptr<GuestOutputImage>& real, uint64_t) {
  frame_generation_paint_again_ = false;
  return real;
}

#endif  // REX_HAS_LSFG

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
