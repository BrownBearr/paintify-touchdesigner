#pragma once
// What the Vulkan backend exposes beyond gpu.h, for display_vk.cpp: the
// device, the command buffer the pipeline records into, and the GLSL
// compiler. Nothing outside the display should need it.
#include "gpu.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <string>
#include <vector>

namespace gpu {
namespace vk {

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t apiVersion = 0;
};
const Device& device();

// The command buffer everything is being recorded into, begun if it was not,
// with a barrier already recorded against whatever came before -- so whatever
// the caller records next sees the pipeline's finished writes.
VkCommandBuffer commands();

// Submits what has been recorded and waits for it. `wait` is waited on at
// `waitStage`; `signal` is signalled on completion. Either may be null.
void submit(VkSemaphore wait = VK_NULL_HANDLE,
            VkPipelineStageFlags waitStage = 0,
            VkSemaphore signal = VK_NULL_HANDLE);

// Destroys something once the GPU can no longer be using it.
void retire(std::function<void()> fn);

// GLSL to SPIR-V, one module per stage, linked together so the stages'
// interfaces agree. Plain Vulkan GLSL: no relaxed rules or binding shifts.
enum class Stage { Vertex, Fragment, Compute };
bool compile(const std::vector<std::pair<Stage, std::string>>& sources,
             std::vector<std::vector<uint32_t>>* spirv, std::string* err);

// A texture as a combined image sampler: its sampled view, in its layout,
// with a filtering sampler that clamps to the edge.
VkDescriptorImageInfo sampled(Texture t, bool nearest);

VkShaderModule createModule(const std::vector<uint32_t>& spirv);

} // namespace vk
} // namespace gpu
