// gpu.h over Vulkan 1.1. On macOS this runs through MoltenVK, which is the
// reason it exists: Apple's OpenGL stops at 4.1 and has no compute shaders.
//
// The pipeline is written against GL's model, and this file supplies it:
//
//  * Shaders are the same GLSL files the GL backend loads. glslang compiles
//    them under its relaxed Vulkan rules: loose uniforms (`uniform int uPass`)
//    are gathered into one default uniform block, gl_VertexID/gl_InstanceID
//    are mapped to their Vulkan equivalents, and GL's separate binding
//    namespaces are shifted into disjoint ranges of descriptor set 0
//    (samplers 0.., images 16.., the Params block 24, loose uniforms 25,
//    storage buffers 32..). The binding numbers written in the shaders keep
//    their meaning.
//
//  * Binding state is global and sticky, as in GL. At each dispatch or draw
//    the program's bindings -- found by reading its SPIR-V -- are filled from
//    that state and pushed with VK_KHR_push_descriptor. Uniform data (the
//    Params block and the loose uniforms) is copied into a ring buffer at the
//    same moment, which is what gives setParams() and Program::setInt() GL's
//    "the value when the command was issued" semantics.
//
//  * Every image lives in VK_IMAGE_LAYOUT_GENERAL, and every command is
//    separated from the one before it by a full memory barrier. That is more
//    synchronisation than strictly necessary, but nearly every stage consumes
//    the previous one's output anyway, and it means the pipeline's logic has
//    one copy rather than one per API.
//
//  * Work is recorded into one command buffer and submitted only when the
//    host needs a result: a readback, finish(), or presenting a frame. Every
//    submission is waited for, so there is never more than one in flight.
#include "gpu.h"
#include "gpu_vk.h"
#include "shader_source.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace gpu {

// ── Binding layout ─────────────────────────────────────────────────────────────
namespace {

constexpr uint32_t kTexBase = 0;          // `layout(binding = u) uniform sampler*`
constexpr uint32_t kImgBase = 16;         // `layout(binding = u) uniform image*`
constexpr uint32_t kUboBase = 24;         // the Params block, binding 0
// Loose uniforms, gathered by glslang into one block. glslang applies the UBO
// shift to this binding as well, so it lands at kUboBase + 1.
constexpr uint32_t kDefaultBinding = 1;
constexpr uint32_t kSsboBase = 32;        // `layout(std430, binding = b) buffer`
constexpr uint32_t kMaxUnits = 16, kMaxImages = 8, kMaxSsbo = 16;
const char* const kDefaultBlockName = "gDefault";

constexpr VkDeviceSize kRingSize = 32ull << 20;
// Uploads bigger than this get a buffer of their own rather than a ring slice.
constexpr VkDeviceSize kRingLargest = 16ull << 20;
constexpr uint32_t kQueryCount = 256;

void check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    fprintf(stderr, "Vulkan: %s failed (VkResult %d)\n", what, int(r));
    std::abort();
}

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

// ── Half floats ───────────────────────────────────────────────────────────────
// Vulkan copies texel data verbatim, so the conversions GL does on upload and
// readback happen here instead.
uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    uint32_t mant = x & 0x007fffffu;
    int32_t exp = int32_t((x >> 23) & 0xffu);
    if (exp == 255) return uint16_t(sign | 0x7c00u | (mant ? 0x200u : 0u));
    exp = exp - 127 + 15;
    if (exp >= 31) return uint16_t(sign | 0x7c00u);
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        mant |= 0x00800000u;
        const uint32_t shift = uint32_t(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u), mid = 1u << (shift - 1u);
        if (rem > mid || (rem == mid && (h & 1u))) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1fffu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;   // round to nearest even
    return uint16_t(sign | h);
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1fu, mant = h & 0x3ffu, bits;
    if (exp == 0) {
        if (!mant) {
            bits = sign;
        } else {
            exp = 1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            bits = sign | ((exp + 112u) << 23) | ((mant & 0x3ffu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7f800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// What GL does converting a float to an 8-bit normalised value.
uint8_t toUnorm8(float f) {
    if (!(f > 0.f)) return 0;
    if (f >= 1.f) return 255;
    return uint8_t(std::nearbyint(f * 255.f));
}

VkFormat vkFormat(Format f) {
    switch (f) {
        case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R16F:    return VK_FORMAT_R16_SFLOAT;
        case Format::RG16F:   return VK_FORMAT_R16G16_SFLOAT;
        default:              return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

size_t texelBytes(Format f) {
    switch (f) {
        case Format::RGBA16F: return 8;
        case Format::R16F:    return 2;
        case Format::RG16F:   return 4;
        default:              return 4;
    }
}

int channels(Format f) {
    switch (f) {
        case Format::R16F:  return 1;
        case Format::RG16F: return 2;
        default:            return 4;
    }
}

} // namespace

// ── Objects ─────────────────────────────────────────────────────────────────────

struct TextureObj {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;     // every level and layer, for sampling
    VkImageView level0 = VK_NULL_HANDLE;   // level 0 only, for storage and attachment
    VkSampler sampler = VK_NULL_HANDLE;    // owned by the sampler cache
    Format fmt = Format::RGBA8;
    int w = 0, h = 0, layers = 1, levels = 1;
    bool array = false;
};

struct BufferObj {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t size = 0;
};

struct FramebufferObj {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    int w = 0, h = 0;
    uint32_t count = 0;
};

namespace {

struct Binding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    bool arrayed = false;           // sampler2DArray
    Format storageFormat = Format::RGBA8;
    uint32_t blockSize = 0;         // uniform blocks
    bool isDefault = false;         // the loose uniforms, rather than Params
};

struct Member {
    uint32_t offset = 0, size = 0;
};

struct Reflection {
    std::vector<Binding> bindings;
    std::map<std::string, Member> uniforms;   // members of the default block
    uint32_t defaultSize = 0;
};

// Host-visible memory handed out in slices, for uniform data and uploads.
struct Slice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;
};

struct Context {
    vk::Device dev;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
    std::string description;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool recording = false;
    bool dirty = false;          // something was recorded since the last barrier
    uint64_t submission = 1;     // serial of the command buffer being recorded

    VkBuffer ring = VK_NULL_HANDLE;
    VkDeviceMemory ringMemory = VK_NULL_HANDLE;
    uint8_t* ringPtr = nullptr;
    VkDeviceSize ringOffset = 0;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    void* readbackPtr = nullptr;
    VkDeviceSize readbackSize = 0;
    bool readbackCoherent = true;

    std::vector<uint8_t> params;
    Texture tex[kMaxUnits] = {};
    Texture img[kMaxImages] = {};
    Buffer ssbo[kMaxSsbo] = {};

    Texture dummy2D = nullptr, dummyArray = nullptr;
    std::map<Format, Texture> dummyStorage;
    Buffer dummyBuffer = nullptr;

    std::map<uint32_t, VkSampler> samplers;

    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t nextQuery = 0;
    double timestampPeriodNs = 1.0;
    uint64_t timestampMask = ~0ull;

    std::vector<std::function<void()>> retired;
    bool glslangReady = false;
};

Context ctx;

// ── Memory ────────────────────────────────────────────────────────────────────
uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) {
    for (int pass = 0; pass < 2; ++pass) {
        const VkMemoryPropertyFlags need = pass == 0 ? (want | prefer) : want;
        for (uint32_t i = 0; i < ctx.memProps.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (ctx.memProps.memoryTypes[i].propertyFlags & need) == need)
                return i;
        }
    }
    fprintf(stderr, "Vulkan: no memory type with flags 0x%x\n", unsigned(want));
    std::abort();
}

VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want,
                        VkMemoryPropertyFlags prefer = 0,
                        VkMemoryPropertyFlags* got = nullptr) {
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(req.memoryTypeBits, want, prefer);
    if (got) *got = ctx.memProps.memoryTypes[ai.memoryTypeIndex].propertyFlags;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    check(vkAllocateMemory(ctx.dev.device, &ai, nullptr, &mem), "vkAllocateMemory");
    return mem;
}

void createRawBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                     VkMemoryPropertyFlags prefer, VkBuffer* buf, VkDeviceMemory* mem,
                     VkMemoryPropertyFlags* got = nullptr) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(ctx.dev.device, &bi, nullptr, buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ctx.dev.device, *buf, &req);
    *mem = allocate(req, want, prefer, got);
    check(vkBindBufferMemory(ctx.dev.device, *buf, *mem, 0), "vkBindBufferMemory");
}

// ── Command recording ─────────────────────────────────────────────────────────
void runRetired() {
    std::vector<std::function<void()>> fns;
    fns.swap(ctx.retired);
    for (auto& f : fns) f();
}

VkCommandBuffer recording() {
    if (!ctx.recording) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(ctx.cmd, &bi), "vkBeginCommandBuffer");
        ctx.recording = true;
    }
    return ctx.cmd;
}

// A full memory barrier, when anything was recorded since the last one. The
// first command after a submission gets one too: a barrier's first scope
// covers earlier submissions, which is what orders them against this one.
VkCommandBuffer beginOp() {
    VkCommandBuffer cb = recording();
    if (ctx.dirty) {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr,
                             0, nullptr);
        ctx.dirty = false;
    }
    return cb;
}

void endOp() { ctx.dirty = true; }

void submitAndWait(VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal) {
    if (!ctx.recording && !wait && !signal) { runRetired(); return; }
    VkCommandBuffer cb = recording();
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    ctx.recording = false;

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &waitStage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    check(vkQueueSubmit(ctx.dev.queue, 1, &si, ctx.fence), "vkQueueSubmit");
    check(vkWaitForFences(ctx.dev.device, 1, &ctx.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    check(vkResetFences(ctx.dev.device, 1, &ctx.fence), "vkResetFences");
    check(vkResetCommandPool(ctx.dev.device, ctx.pool, 0), "vkResetCommandPool");

    ctx.dirty = true;
    ctx.ringOffset = 0;
    ++ctx.submission;
    runRetired();
}

// A slice of host-visible memory that stays valid until the next submission.
Slice stream(VkDeviceSize size, VkDeviceSize align) {
    Slice s;
    if (size > kRingLargest) {
        VkDeviceMemory mem;
        createRawBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        0, &s.buffer, &mem);
        void* p = nullptr;
        check(vkMapMemory(ctx.dev.device, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
        s.ptr = static_cast<uint8_t*>(p);
        VkBuffer b = s.buffer;
        ctx.retired.push_back([b, mem] {
            vkDestroyBuffer(ctx.dev.device, b, nullptr);
            vkFreeMemory(ctx.dev.device, mem, nullptr);
        });
        return s;
    }
    VkDeviceSize off = alignUp(ctx.ringOffset, align);
    if (off + size > kRingSize) {
        // Full. Flushing frees the whole ring; the work already recorded runs
        // first, so nothing reorders.
        submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        off = 0;
    }
    ctx.ringOffset = off + size;
    s.buffer = ctx.ring;
    s.offset = off;
    s.ptr = ctx.ringPtr + off;
    return s;
}

// Makes a device write visible to the host, before the fence that ends it.
void hostBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
}

void ensureReadback(VkDeviceSize size) {
    if (size <= ctx.readbackSize) return;
    if (ctx.readback) {
        vkDestroyBuffer(ctx.dev.device, ctx.readback, nullptr);
        vkFreeMemory(ctx.dev.device, ctx.readbackMemory, nullptr);
    }
    VkMemoryPropertyFlags got = 0;
    createRawBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &ctx.readback, &ctx.readbackMemory, &got);
    ctx.readbackCoherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    check(vkMapMemory(ctx.dev.device, ctx.readbackMemory, 0, VK_WHOLE_SIZE, 0,
                      &ctx.readbackPtr), "vkMapMemory");
    ctx.readbackSize = size;
}

void invalidateReadback() {
    if (ctx.readbackCoherent) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = ctx.readbackMemory;
    r.size = VK_WHOLE_SIZE;
    vkInvalidateMappedMemoryRanges(ctx.dev.device, 1, &r);
}

// ── Samplers ──────────────────────────────────────────────────────────────────
VkSampler sampler(bool mips, Wrap u, Wrap v) {
    const uint32_t key = (mips ? 1u : 0u) | (u == Wrap::Repeat ? 2u : 0u) |
                         (v == Wrap::Repeat ? 4u : 0u);
    auto it = ctx.samplers.find(key);
    if (it != ctx.samplers.end()) return it->second;

    // GL_LINEAR, or GL_LINEAR_MIPMAP_LINEAR on a mipped texture. A non-mipped
    // GL texture samples its base level only, hence maxLod 0 there.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = u == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                        : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = v == Wrap::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                        : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.f;
    si.maxLod = mips ? VK_LOD_CLAMP_NONE : 0.f;
    VkSampler s = VK_NULL_HANDLE;
    check(vkCreateSampler(ctx.dev.device, &si, nullptr, &s), "vkCreateSampler");
    ctx.samplers[key] = s;
    return s;
}

// ── Textures ──────────────────────────────────────────────────────────────────
Texture makeTexture(int w, int h, int layers, bool array, Format f, bool mips, Wrap u, Wrap v) {
    Texture t = new TextureObj;
    t->w = w; t->h = h; t->layers = layers; t->array = array; t->fmt = f;
    if (mips) {
        int m = (w > h ? w : h), levels = 1;
        while (m > 1) { m >>= 1; ++levels; }
        t->levels = levels;
    }

    const VkFormat vf = vkFormat(f);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(ctx.dev.physical, vf, &fp);
    const VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)    usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)    usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = vf;
    ci.extent = {uint32_t(w), uint32_t(h), 1};
    ci.mipLevels = uint32_t(t->levels);
    ci.arrayLayers = uint32_t(layers);
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(ctx.dev.device, &ci, nullptr, &t->image), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(ctx.dev.device, t->image, &req);
    t->memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkBindImageMemory(ctx.dev.device, t->image, t->memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vf;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(t->levels), 0, uint32_t(layers)};
    check(vkCreateImageView(ctx.dev.device, &vi, nullptr, &t->view), "vkCreateImageView");
    if (!array) {
        vi.subresourceRange.levelCount = 1;
        check(vkCreateImageView(ctx.dev.device, &vi, nullptr, &t->level0), "vkCreateImageView");
    }
    t->sampler = sampler(mips, u, v);

    // Straight to GENERAL, where every image stays, and cleared: GL drivers
    // hand out zeroed storage in practice, and nothing should depend on
    // whatever memory the image happens to land on.
    VkCommandBuffer cb = beginOp();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->image;
    ib.subresourceRange = vi.subresourceRange;
    ib.subresourceRange.levelCount = uint32_t(t->levels);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ib);
    const VkClearColorValue zero{};
    vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
    endOp();
    return t;
}

Texture dummyStorage(Format f) {
    auto it = ctx.dummyStorage.find(f);
    if (it != ctx.dummyStorage.end()) return it->second;
    Texture t = makeTexture(1, 1, 1, false, f, false, Wrap::Clamp, Wrap::Clamp);
    ctx.dummyStorage[f] = t;
    return t;
}

void destroyTexture(Texture t) {
    VkImage image = t->image;
    VkDeviceMemory mem = t->memory;
    VkImageView view = t->view, level0 = t->level0;
    delete t;
    ctx.retired.push_back([=] {
        if (level0) vkDestroyImageView(ctx.dev.device, level0, nullptr);
        vkDestroyImageView(ctx.dev.device, view, nullptr);
        vkDestroyImage(ctx.dev.device, image, nullptr);
        vkFreeMemory(ctx.dev.device, mem, nullptr);
    });
    if (!ctx.recording) runRetired();
}

// ── SPIR-V reflection ─────────────────────────────────────────────────────────
// Just enough of it to build a descriptor set layout and to find the loose
// uniforms: which resources each binding holds, and the default block's
// member offsets.
std::string spvString(const uint32_t* w, size_t words) {
    std::string s;
    for (size_t i = 0; i < words; ++i) {
        for (int b = 0; b < 4; ++b) {
            const char c = char((w[i] >> (8 * b)) & 0xffu);
            if (!c) return s;
            s += c;
        }
    }
    return s;
}

bool reflect(const std::vector<uint32_t>& code, Reflection* out, std::string* err) {
    enum : uint32_t {
        OpName = 5, OpMemberName = 6, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
        OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeStruct = 30,
        OpTypePointer = 32, OpFunction = 54, OpVariable = 59, OpDecorate = 71,
        OpMemberDecorate = 72,
        DecBlock = 2, DecBufferBlock = 3, DecBinding = 33, DecOffset = 35,
        StorageUniformConstant = 0, StorageUniform = 2, StorageBuffer = 12,
    };
    if (code.size() < 5 || code[0] != 0x07230203u) { *err = "not SPIR-V"; return false; }

    std::map<uint32_t, std::string> names;
    std::map<std::pair<uint32_t, uint32_t>, std::string> memberNames;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberOffsets;
    std::map<uint32_t, uint32_t> bindingOf;
    std::set<uint32_t> blocks, bufferBlocks;
    std::map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> types;
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> vars;   // id, (type, class)

    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xffffu;
        if (wc == 0 || i + wc > code.size()) { *err = "malformed SPIR-V"; return false; }
        const uint32_t* w = &code[i];
        if (op == OpFunction) break;   // declarations all come before code
        switch (op) {
            case OpName:       names[w[1]] = spvString(w + 2, wc - 2); break;
            case OpMemberName: memberNames[{w[1], w[2]}] = spvString(w + 3, wc - 3); break;
            case OpDecorate:
                if (wc >= 4 && w[2] == DecBinding) bindingOf[w[1]] = w[3];
                else if (w[2] == DecBlock) blocks.insert(w[1]);
                else if (w[2] == DecBufferBlock) bufferBlocks.insert(w[1]);
                break;
            case OpMemberDecorate:
                if (wc >= 5 && w[3] == DecOffset) memberOffsets[{w[1], w[2]}] = w[4];
                break;
            case OpVariable: vars.push_back({w[2], {w[1], w[3]}}); break;
            default:
                if ((op >= 19 && op <= 32) && wc >= 2)
                    types[w[1]] = {op, std::vector<uint32_t>(w + 2, w + wc)};
                break;
        }
        i += wc;
    }

    std::function<uint32_t(uint32_t)> sizeOf = [&](uint32_t id) -> uint32_t {
        auto it = types.find(id);
        if (it == types.end()) return 16;
        const auto& t = it->second;
        switch (t.first) {
            case OpTypeBool: return 4;
            case OpTypeInt:
            case OpTypeFloat: return t.second.empty() ? 4 : t.second[0] / 8;
            case OpTypeVector: return t.second.size() >= 2 ? t.second[1] * sizeOf(t.second[0]) : 16;
            default: return 16;
        }
    };
    auto blockSize = [&](uint32_t structId) {
        const auto& members = types[structId].second;
        uint32_t size = 0;
        for (uint32_t m = 0; m < members.size(); ++m) {
            auto off = memberOffsets.find({structId, m});
            if (off != memberOffsets.end())
                size = std::max(size, off->second + sizeOf(members[m]));
        }
        return size;
    };

    for (const auto& v : vars) {
        auto b = bindingOf.find(v.first);
        if (b == bindingOf.end()) continue;
        const auto ptr = types.find(v.second.first);
        if (ptr == types.end() || ptr->second.first != OpTypePointer) continue;
        const uint32_t storage = ptr->second.second[0];
        const uint32_t pointee = ptr->second.second[1];
        auto t = types.find(pointee);
        if (t == types.end()) continue;

        Binding bind;
        bind.binding = b->second;
        if (t->second.first == OpTypeSampledImage) {
            const auto& image = types[t->second.second[0]].second;
            bind.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bind.arrayed = image.size() > 3 && image[3] != 0;
        } else if (t->second.first == OpTypeImage) {
            const auto& image = t->second.second;
            if (image.size() < 7 || image[5] != 2) continue;   // not a storage image
            bind.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            switch (image[6]) {                  // SPIR-V ImageFormat
                case 2: bind.storageFormat = Format::RGBA16F; break;
                case 7: bind.storageFormat = Format::RG16F; break;
                case 9: bind.storageFormat = Format::R16F; break;
                default: bind.storageFormat = Format::RGBA8; break;
            }
        } else if (t->second.first == OpTypeStruct) {
            const bool ssbo = storage == StorageBuffer ||
                              (storage == StorageUniform && bufferBlocks.count(pointee));
            if (!ssbo && storage != StorageUniform) continue;
            bind.type = ssbo ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (!ssbo) {
                bind.blockSize = blockSize(pointee);
                if (names[pointee] == kDefaultBlockName) {
                    bind.isDefault = true;
                    const auto& members = t->second.second;
                    for (uint32_t m = 0; m < members.size(); ++m) {
                        Member mem;
                        mem.offset = memberOffsets[{pointee, m}];
                        mem.size = sizeOf(members[m]);
                        out->uniforms[memberNames[{pointee, m}]] = mem;
                    }
                    out->defaultSize = std::max(out->defaultSize, bind.blockSize);
                }
            }
        } else {
            continue;
        }
        (void)StorageUniformConstant;
        out->bindings.push_back(bind);
    }
    return true;
}

// ── glslang ───────────────────────────────────────────────────────────────────
EShLanguage language(vk::Stage s) {
    switch (s) {
        case vk::Stage::Vertex:   return EShLangVertex;
        case vk::Stage::Fragment: return EShLangFragment;
        default:                  return EShLangCompute;
    }
}

// `relaxed` is how the pipeline's GL-flavoured shaders are compiled; the
// display's own small shaders are plain Vulkan GLSL.
bool compileStages(const std::vector<std::pair<vk::Stage, std::string>>& sources,
                   const std::vector<std::string>& names, bool relaxed,
                   std::vector<std::vector<uint32_t>>* spirv, std::string* err) {
    if (!ctx.glslangReady) {
        glslang::InitializeProcess();
        ctx.glslangReady = true;
    }
    const EShMessages msgs = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    glslang::TProgram program;
    for (size_t i = 0; i < sources.size(); ++i) {
        const EShLanguage lang = language(sources[i].first);
        auto sh = std::make_unique<glslang::TShader>(lang);
        const char* src = sources[i].second.c_str();
        const char* name = names[i].c_str();
        sh->setStringsWithLengthsAndNames(&src, nullptr, &name, 1);
        sh->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        sh->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        sh->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        if (relaxed) {
            sh->setEnvInputVulkanRulesRelaxed();
            sh->setGlobalUniformBlockName(kDefaultBlockName);
            sh->setGlobalUniformSet(0);
            sh->setGlobalUniformBinding(kDefaultBinding);
            sh->setShiftBinding(glslang::EResTexture, kTexBase);
            sh->setShiftBinding(glslang::EResSampler, kTexBase);
            sh->setShiftBinding(glslang::EResImage, kImgBase);
            sh->setShiftBinding(glslang::EResUbo, kUboBase);
            sh->setShiftBinding(glslang::EResSsbo, kSsboBase);
            sh->setAutoMapLocations(true);
        }
        if (!sh->parse(GetDefaultResources(), 460, false, msgs)) {
            *err = names[i] + ":\n" + sh->getInfoLog();
            return false;
        }
        program.addShader(sh.get());
        shaders.push_back(std::move(sh));
    }
    if (!program.link(msgs)) {
        *err = names[0] + " link:\n" + program.getInfoLog();
        return false;
    }
    if (!program.mapIO()) {
        *err = names[0] + " binding map:\n" + program.getInfoLog();
        return false;
    }
    spirv->clear();
    for (const auto& s : sources) {
        std::vector<unsigned int> words;
        glslang::SpvOptions opt;
        opt.disableOptimizer = true;
        glslang::GlslangToSpv(*program.getIntermediate(language(s.first)), words, &opt);
        spirv->emplace_back(words.begin(), words.end());
    }
    return true;
}

} // namespace

// ── Programs ──────────────────────────────────────────────────────────────────

struct Program::Impl {
    bool compute = true;
    VkShaderModule modules[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;       // compute
    std::map<std::pair<VkRenderPass, uint32_t>, VkPipeline> graphics;
    std::vector<Binding> bindings;
    std::map<std::string, Member> uniforms;
    std::vector<uint8_t> uniformData;

    ~Impl() {
        if (!ctx.dev.device) return;
        std::vector<VkPipeline> pipes;
        if (pipeline) pipes.push_back(pipeline);
        for (auto& g : graphics) pipes.push_back(g.second);
        VkShaderModule m0 = modules[0], m1 = modules[1];
        VkDescriptorSetLayout sl = setLayout;
        VkPipelineLayout pl = layout;
        ctx.retired.push_back([=] {
            for (VkPipeline p : pipes) vkDestroyPipeline(ctx.dev.device, p, nullptr);
            if (m0) vkDestroyShaderModule(ctx.dev.device, m0, nullptr);
            if (m1) vkDestroyShaderModule(ctx.dev.device, m1, nullptr);
            vkDestroyPipelineLayout(ctx.dev.device, pl, nullptr);
            vkDestroyDescriptorSetLayout(ctx.dev.device, sl, nullptr);
        });
        if (!ctx.recording) runRetired();
    }

    void set(const char* name, const void* data, uint32_t bytes) {
        auto it = uniforms.find(name);
        if (it == uniforms.end()) return;     // unused: GL's location -1
        std::memcpy(uniformData.data() + it->second.offset, data,
                    std::min(bytes, it->second.size));
    }
};

bool Program::build() {
    lastError.clear();
    const bool compute = paths.size() == 1;
    std::vector<std::pair<vk::Stage, std::string>> sources;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::string err;
        std::string src = shadersrc::load(paths[i], &err);
        if (src.empty()) { lastError = paths[i] + ": " + err; return false; }
        const vk::Stage st = compute ? vk::Stage::Compute
                                     : (i == 0 ? vk::Stage::Vertex : vk::Stage::Fragment);
        sources.push_back({st, src});
    }

    std::vector<std::vector<uint32_t>> spirv;
    if (!compileStages(sources, paths, /*relaxed=*/true, &spirv, &lastError)) return false;

    auto p = std::make_shared<Impl>();
    p->compute = compute;
    std::map<uint32_t, Binding> merged;
    for (size_t i = 0; i < spirv.size(); ++i) {
        Reflection r;
        if (!reflect(spirv[i], &r, &lastError)) { lastError = paths[i] + ": " + lastError; return false; }
        for (const Binding& b : r.bindings) merged[b.binding] = b;
        for (const auto& u : r.uniforms) p->uniforms[u.first] = u.second;
        if (r.defaultSize > p->uniformData.size()) p->uniformData.resize(r.defaultSize, 0);
    }
    for (const auto& b : merged) p->bindings.push_back(b.second);

    const VkShaderStageFlags stages = compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                              : (VK_SHADER_STAGE_VERTEX_BIT |
                                                 VK_SHADER_STAGE_FRAGMENT_BIT);
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Binding& b : p->bindings) {
        VkDescriptorSetLayoutBinding d{};
        d.binding = b.binding;
        d.descriptorType = b.type;
        d.descriptorCount = 1;
        d.stageFlags = stages;
        lb.push_back(d);
    }
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sli.bindingCount = uint32_t(lb.size());
    sli.pBindings = lb.data();
    check(vkCreateDescriptorSetLayout(ctx.dev.device, &sli, nullptr, &p->setLayout),
          "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &p->setLayout;
    check(vkCreatePipelineLayout(ctx.dev.device, &pli, nullptr, &p->layout),
          "vkCreatePipelineLayout");

    for (size_t i = 0; i < spirv.size(); ++i) p->modules[i] = vk::createModule(spirv[i]);

    if (compute) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = p->modules[0];
        ci.stage.pName = "main";
        ci.layout = p->layout;
        const VkResult r = vkCreateComputePipelines(ctx.dev.device, VK_NULL_HANDLE, 1, &ci,
                                                    nullptr, &p->pipeline);
        if (r != VK_SUCCESS) {
            lastError = paths[0] + ": vkCreateComputePipelines failed (" +
                        std::to_string(int(r)) + ")";
            return false;
        }
    }

    impl = p;
    mtimes = shadersrc::mtimes(paths);
    return true;
}

void Program::setInt(const char* name, int v) { if (impl) impl->set(name, &v, 4); }
void Program::setUInt(const char* name, uint32_t v) { if (impl) impl->set(name, &v, 4); }
void Program::setIVec2(const char* name, int x, int y) {
    const int v[2] = {x, y};
    if (impl) impl->set(name, v, 8);
}
void Program::setVec3(const char* name, float x, float y, float z) {
    const float v[3] = {x, y, z};
    if (impl) impl->set(name, v, 12);
}

namespace {

// Fills the program's bindings from the current binding state and pushes
// them. Unbound slots get a small dummy of the right kind, since Vulkan, unlike
// GL, will not tolerate a missing descriptor even where the shader's control
// flow never reads it.
void pushBindings(Program::Impl& p, VkCommandBuffer cb, VkPipelineBindPoint point,
                  const std::vector<Slice>& uniforms) {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> images(p.bindings.size());
    std::vector<VkDescriptorBufferInfo> buffers(p.bindings.size());
    writes.reserve(p.bindings.size());
    size_t uniformIndex = 0;

    for (size_t i = 0; i < p.bindings.size(); ++i) {
        const Binding& b = p.bindings[i];
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = b.type;
        switch (b.type) {
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                const uint32_t unit = b.binding - kTexBase;
                Texture t = unit < kMaxUnits ? ctx.tex[unit] : nullptr;
                if (!t || t->array != b.arrayed) t = b.arrayed ? ctx.dummyArray : ctx.dummy2D;
                images[i] = {t->sampler, t->view, VK_IMAGE_LAYOUT_GENERAL};
                w.pImageInfo = &images[i];
                break;
            }
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                const uint32_t unit = b.binding - kImgBase;
                Texture t = unit < kMaxImages ? ctx.img[unit] : nullptr;
                if (!t || t->array || t->fmt != b.storageFormat) t = dummyStorage(b.storageFormat);
                images[i] = {VK_NULL_HANDLE, t->level0, VK_IMAGE_LAYOUT_GENERAL};
                w.pImageInfo = &images[i];
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
                const Slice& s = uniforms[uniformIndex++];
                buffers[i] = {s.buffer, s.offset, std::max<VkDeviceSize>(b.blockSize, 16)};
                w.pBufferInfo = &buffers[i];
                break;
            }
            default: {
                const uint32_t slot = b.binding - kSsboBase;
                Buffer buf = slot < kMaxSsbo ? ctx.ssbo[slot] : nullptr;
                if (!buf) buf = ctx.dummyBuffer;
                buffers[i] = {buf->buffer, 0, VK_WHOLE_SIZE};
                w.pBufferInfo = &buffers[i];
                break;
            }
        }
        writes.push_back(w);
    }
    if (!writes.empty())
        ctx.pushDescriptorSet(cb, point, p.layout, 0, uint32_t(writes.size()), writes.data());
}

// Copies the program's uniform blocks into the ring, before anything is
// recorded -- the ring may have to flush to make room, and a flush in the
// middle of recording a dispatch would split it across two command buffers.
std::vector<Slice> captureUniforms(Program::Impl& p) {
    const VkDeviceSize align = std::max<VkDeviceSize>(
        ctx.props.limits.minUniformBufferOffsetAlignment, 16);
    VkDeviceSize need = 0;
    for (const Binding& b : p.bindings)
        if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            need += alignUp(std::max<uint32_t>(b.blockSize, 16), align) + align;
    if (alignUp(ctx.ringOffset, align) + need > kRingSize)
        submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);

    std::vector<Slice> out;
    for (const Binding& b : p.bindings) {
        if (b.type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
        const uint32_t size = std::max<uint32_t>(b.blockSize, 16);
        Slice s = stream(size, align);
        std::memset(s.ptr, 0, size);
        const std::vector<uint8_t>& src = b.isDefault ? p.uniformData : ctx.params;
        std::memcpy(s.ptr, src.data(), std::min<size_t>(size, src.size()));
        out.push_back(s);
    }
    return out;
}

VkPipeline graphicsPipeline(Program::Impl& p, Framebuffer fb, const std::vector<Blend>& blend) {
    uint32_t key = 0;
    for (size_t i = 0; i < blend.size(); ++i) key |= uint32_t(blend[i]) << (2 * i);
    auto it = p.graphics.find({fb->renderPass, key});
    if (it != p.graphics.end()) return it->second;

    VkPipelineShaderStageCreateInfo st[2]{};
    for (int i = 0; i < 2; ++i) {
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        st[i].module = p.modules[i];
        st[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    std::vector<VkPipelineColorBlendAttachmentState> att(fb->count);
    for (uint32_t i = 0; i < fb->count; ++i) {
        const Blend b = i < blend.size() ? blend[i] : Blend::None;
        VkPipelineColorBlendAttachmentState& a = att[i];
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        a.blendEnable = b == Blend::None ? VK_FALSE : VK_TRUE;
        a.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        a.dstColorBlendFactor = b == Blend::Additive ? VK_BLEND_FACTOR_ONE
                                                     : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        a.colorBlendOp = VK_BLEND_OP_ADD;
        a.srcAlphaBlendFactor = a.srcColorBlendFactor;
        a.dstAlphaBlendFactor = a.dstColorBlendFactor;
        a.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(att.size());
    cb.pAttachments = att.data();
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vin;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &ds;
    gi.layout = p.layout;
    gi.renderPass = fb->renderPass;
    VkPipeline pipe = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(ctx.dev.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipe),
          "vkCreateGraphicsPipelines");
    p.graphics[{fb->renderPass, key}] = pipe;
    return pipe;
}

// ── Instance and device ──────────────────────────────────────────────────────
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        fprintf(stderr, "[Vulkan%s] %s\n",
                severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? " ERROR" : "",
                data->pMessage);
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

} // namespace

bool init(const InitOptions& opt, std::string* err) {
    const bool validate = opt.debug || envOn("PAINTIFY_VK_VALIDATION");

    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> instExts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, instExts.data());

    std::vector<const char*> exts;
    for (const std::string& e : opt.instanceExtensions) exts.push_back(e.c_str());
    // MoltenVK is a "portability" implementation: the loader hides it unless
    // the application says it knows what that means.
    VkInstanceCreateFlags flags = 0;
    if (hasExtension(instExts, "VK_KHR_portability_enumeration")) {
        exts.push_back("VK_KHR_portability_enumeration");
        flags |= 0x00000001;   // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
    const bool debugUtils = validate && hasExtension(instExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (validate) {
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        for (const auto& l : lp)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) fprintf(stderr, "Vulkan validation layer not installed\n");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu-sbr";
    app.pEngineName = "paintify";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = flags;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ici, nullptr, &ctx.dev.instance);
    if (r != VK_SUCCESS) {
        if (err) {
            *err = "vkCreateInstance failed (" + std::to_string(int(r)) + ").";
#ifdef __APPLE__
            *err += " Is MoltenVK installed? `brew install molten-vk vulkan-loader`.";
#endif
        }
        return false;
    }
    ctx.dev.apiVersion = VK_API_VERSION_1_1;

    if (debugUtils) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.dev.instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = debugCallback;
        if (create) create(ctx.dev.instance, &mi, nullptr, &ctx.messenger);
    }

    // Device: the most capable one with a graphics+compute queue and push
    // descriptors. PAINTIFY_VK_DEVICE picks one by (part of) its name.
    vkEnumeratePhysicalDevices(ctx.dev.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(ctx.dev.instance, &n, devs.data());
    const char* want = std::getenv("PAINTIFY_VK_DEVICE");
    int bestScore = -1;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> de(ne);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, de.data());
        if (!hasExtension(de, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;

        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qf.data());
        int family = -1;
        for (uint32_t i = 0; i < nq; ++i) {
            const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((qf[i].queueFlags & need) == need) { family = int(i); break; }
        }
        if (family < 0) continue;

        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? 4
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2 : 1;
        if (want && *want && std::strstr(p.deviceName, want)) score += 100;
        if (score > bestScore) {
            bestScore = score;
            ctx.dev.physical = d;
            ctx.dev.queueFamily = uint32_t(family);
            ctx.props = p;
            ctx.timestampMask = qf[family].timestampValidBits >= 64
                              ? ~0ull : ((1ull << qf[family].timestampValidBits) - 1ull);
            if (qf[family].timestampValidBits == 0) ctx.timestampMask = 0;
        }
    }
    if (!ctx.dev.physical) {
        if (err) *err = "no Vulkan device with graphics, compute and VK_KHR_push_descriptor";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(ctx.dev.physical, &ctx.memProps);

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(ctx.dev.physical, &have);
    if (!have.independentBlend || !have.shaderStorageImageExtendedFormats) {
        if (err) *err = std::string(ctx.props.deviceName) +
                        " lacks independentBlend or shaderStorageImageExtendedFormats";
        return false;
    }
    VkPhysicalDeviceFeatures feats{};
    feats.independentBlend = VK_TRUE;
    feats.shaderStorageImageExtendedFormats = VK_TRUE;

    vkEnumerateDeviceExtensionProperties(ctx.dev.physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> devExts(n);
    vkEnumerateDeviceExtensionProperties(ctx.dev.physical, nullptr, &n, devExts.data());
    std::vector<const char*> dexts = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    // Required by the spec whenever the device offers it, which MoltenVK does.
    if (hasExtension(devExts, "VK_KHR_portability_subset"))
        dexts.push_back("VK_KHR_portability_subset");
    if (!opt.instanceExtensions.empty() &&
        hasExtension(devExts, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = ctx.dev.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pEnabledFeatures = &feats;
    r = vkCreateDevice(ctx.dev.physical, &dci, nullptr, &ctx.dev.device);
    if (r != VK_SUCCESS) {
        if (err) *err = "vkCreateDevice failed (" + std::to_string(int(r)) + ")";
        return false;
    }
    vkGetDeviceQueue(ctx.dev.device, ctx.dev.queueFamily, 0, &ctx.dev.queue);
    ctx.pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(ctx.dev.device, "vkCmdPushDescriptorSetKHR"));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx.dev.queueFamily;
    check(vkCreateCommandPool(ctx.dev.device, &pci, nullptr, &ctx.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = ctx.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(ctx.dev.device, &cai, &ctx.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check(vkCreateFence(ctx.dev.device, &fci, nullptr, &ctx.fence), "vkCreateFence");

    createRawBuffer(kRingSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    0, &ctx.ring, &ctx.ringMemory);
    void* ringPtr = nullptr;
    check(vkMapMemory(ctx.dev.device, ctx.ringMemory, 0, VK_WHOLE_SIZE, 0, &ringPtr), "vkMapMemory");
    ctx.ringPtr = static_cast<uint8_t*>(ringPtr);

    if (ctx.timestampMask && ctx.props.limits.timestampComputeAndGraphics) {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kQueryCount;
        if (vkCreateQueryPool(ctx.dev.device, &qpi, nullptr, &ctx.queries) != VK_SUCCESS)
            ctx.queries = VK_NULL_HANDLE;
        ctx.timestampPeriodNs = ctx.props.limits.timestampPeriod;
    }

    ctx.dummy2D = makeTexture(1, 1, 1, false, Format::RGBA8, false, Wrap::Clamp, Wrap::Clamp);
    ctx.dummyArray = makeTexture(1, 1, 1, true, Format::R16F, false, Wrap::Clamp, Wrap::Clamp);
    ctx.dummyBuffer = createBuffer(256);

    const uint32_t v = ctx.props.apiVersion;
    char desc[512];
    snprintf(desc, sizeof(desc), "Vulkan %u.%u.%u | %s", VK_VERSION_MAJOR(v),
             VK_VERSION_MINOR(v), VK_VERSION_PATCH(v), ctx.props.deviceName);
    ctx.description = desc;
    return true;
}

void shutdown() {
    if (!ctx.dev.device) return;
    if (ctx.recording) submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vkDeviceWaitIdle(ctx.dev.device);

    destroy(ctx.dummy2D);
    destroy(ctx.dummyArray);
    for (auto& d : ctx.dummyStorage) { Texture t = d.second; destroy(t); }
    ctx.dummyStorage.clear();
    destroy(ctx.dummyBuffer);
    runRetired();

    for (auto& s : ctx.samplers) vkDestroySampler(ctx.dev.device, s.second, nullptr);
    ctx.samplers.clear();
    if (ctx.queries) vkDestroyQueryPool(ctx.dev.device, ctx.queries, nullptr);
    if (ctx.readback) {
        vkDestroyBuffer(ctx.dev.device, ctx.readback, nullptr);
        vkFreeMemory(ctx.dev.device, ctx.readbackMemory, nullptr);
    }
    vkDestroyBuffer(ctx.dev.device, ctx.ring, nullptr);
    vkFreeMemory(ctx.dev.device, ctx.ringMemory, nullptr);
    vkDestroyFence(ctx.dev.device, ctx.fence, nullptr);
    vkDestroyCommandPool(ctx.dev.device, ctx.pool, nullptr);
    vkDestroyDevice(ctx.dev.device, nullptr);
    if (ctx.messenger) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.dev.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(ctx.dev.instance, ctx.messenger, nullptr);
    }
    vkDestroyInstance(ctx.dev.instance, nullptr);
    if (ctx.glslangReady) glslang::FinalizeProcess();
    ctx = Context();
}

const char* backendName() { return "Vulkan"; }
std::string describe() { return ctx.description; }

// ── Resources ─────────────────────────────────────────────────────────────────

Texture createTexture2D(int w, int h, Format f, bool mips) {
    return makeTexture(w, h, 1, false, f, mips, Wrap::Clamp, Wrap::Clamp);
}

Texture createTextureArray(int w, int h, int layers, Format f, const float* data,
                           Wrap u, Wrap v) {
    Texture t = makeTexture(w, h, layers, true, f, false, u, v);
    const int ch = channels(f);
    const size_t count = size_t(w) * size_t(h) * size_t(layers) * size_t(ch);
    const VkDeviceSize bytes = VkDeviceSize(size_t(w) * h * layers * texelBytes(f));
    Slice s = stream(bytes, 16);
    if (f == Format::RGBA8) {
        for (size_t i = 0; i < count; ++i) s.ptr[i] = toUnorm8(data[i]);
    } else {
        uint16_t* dst = reinterpret_cast<uint16_t*>(s.ptr);
        for (size_t i = 0; i < count; ++i) dst[i] = floatToHalf(data[i]);
    }
    VkCommandBuffer cb = beginOp();
    VkBufferImageCopy c{};
    c.bufferOffset = s.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, uint32_t(layers)};
    c.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyBufferToImage(cb, s.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    endOp();
    return t;
}

void destroy(Texture& t) {
    if (!t) return;
    for (auto& b : ctx.tex) if (b == t) b = nullptr;
    for (auto& b : ctx.img) if (b == t) b = nullptr;
    destroyTexture(t);
    t = nullptr;
}

int width(Texture t) { return t ? t->w : 0; }
int height(Texture t) { return t ? t->h : 0; }
Format format(Texture t) { return t ? t->fmt : Format::RGBA8; }

Buffer createBuffer(size_t bytes, BufferUse) {
    Buffer b = new BufferObj;
    b->size = bytes;
    createRawBuffer(VkDeviceSize(std::max<size_t>(bytes, 4)),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &b->buffer, &b->memory);
    // GL buffers created with no data read back as zeros on every driver
    // this has run on; keep that.
    VkCommandBuffer cb = beginOp();
    vkCmdFillBuffer(cb, b->buffer, 0, VK_WHOLE_SIZE, 0);
    endOp();
    return b;
}

void destroy(Buffer& b) {
    if (!b) return;
    for (auto& s : ctx.ssbo) if (s == b) s = nullptr;
    VkBuffer buf = b->buffer;
    VkDeviceMemory mem = b->memory;
    delete b;
    b = nullptr;
    ctx.retired.push_back([=] {
        vkDestroyBuffer(ctx.dev.device, buf, nullptr);
        vkFreeMemory(ctx.dev.device, mem, nullptr);
    });
    if (!ctx.recording) runRetired();
}

Framebuffer createFramebuffer(std::initializer_list<Texture> colors) {
    Framebuffer fb = new FramebufferObj;
    std::vector<VkAttachmentDescription> att;
    std::vector<VkAttachmentReference> refs;
    std::vector<VkImageView> views;
    for (Texture t : colors) {
        VkAttachmentDescription a{};
        a.format = vkFormat(t->fmt);
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        refs.push_back({uint32_t(att.size()), VK_IMAGE_LAYOUT_GENERAL});
        att.push_back(a);
        views.push_back(t->level0);
        fb->w = t->w;
        fb->h = t->h;
    }
    fb->count = uint32_t(att.size());

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = uint32_t(refs.size());
    sub.pColorAttachments = refs.data();
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = uint32_t(att.size());
    rpi.pAttachments = att.data();
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    check(vkCreateRenderPass(ctx.dev.device, &rpi, nullptr, &fb->renderPass), "vkCreateRenderPass");

    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = fb->renderPass;
    fi.attachmentCount = uint32_t(views.size());
    fi.pAttachments = views.data();
    fi.width = uint32_t(fb->w);
    fi.height = uint32_t(fb->h);
    fi.layers = 1;
    check(vkCreateFramebuffer(ctx.dev.device, &fi, nullptr, &fb->framebuffer), "vkCreateFramebuffer");
    return fb;
}

void destroy(Framebuffer& fb) {
    if (!fb) return;
    VkFramebuffer f = fb->framebuffer;
    VkRenderPass rp = fb->renderPass;
    delete fb;
    fb = nullptr;
    // Graphics pipelines cached against this render pass stay valid: a
    // pipeline only needs a *compatible* pass, and the next framebuffer of
    // the same formats is one. They are released with their program.
    ctx.retired.push_back([=] {
        vkDestroyFramebuffer(ctx.dev.device, f, nullptr);
        vkDestroyRenderPass(ctx.dev.device, rp, nullptr);
    });
    if (!ctx.recording) runRetired();
}

// ── Transfers ─────────────────────────────────────────────────────────────────

void uploadTexture(Texture t, const void* rgba8) {
    const VkDeviceSize bytes = VkDeviceSize(t->w) * t->h * 4;
    Slice s = stream(bytes, 16);
    std::memcpy(s.ptr, rgba8, size_t(bytes));
    VkCommandBuffer cb = beginOp();
    VkBufferImageCopy c{};
    c.bufferOffset = s.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {uint32_t(t->w), uint32_t(t->h), 1};
    vkCmdCopyBufferToImage(cb, s.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    endOp();
}

void copyTexture(Texture src, Texture dst) {
    VkCommandBuffer cb = beginOp();
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {uint32_t(std::min(src->w, dst->w)), uint32_t(std::min(src->h, dst->h)), 1};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    endOp();
}

void clearTexture(Texture t, float r, float g, float b, float a) {
    VkCommandBuffer cb = beginOp();
    VkClearColorValue v{};
    v.float32[0] = r; v.float32[1] = g; v.float32[2] = b; v.float32[3] = a;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(t->layers)};
    vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    endOp();
}

void generateMipmaps(Texture t) {
    int w = t->w, h = t->h;
    for (int level = 1; level < t->levels; ++level) {
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        VkCommandBuffer cb = beginOp();
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level - 1), 0, 1};
        b.srcOffsets[1] = {w, h, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
        b.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &b, VK_FILTER_LINEAR);
        endOp();
        w = nw;
        h = nh;
    }
}

void clearBuffer(Buffer b, size_t offset, size_t size, uint32_t value) {
    VkCommandBuffer cb = beginOp();
    vkCmdFillBuffer(cb, b->buffer, offset, size ? VkDeviceSize(size) : VK_WHOLE_SIZE, value);
    endOp();
}

void copyBuffer(Buffer src, size_t srcOffset, Buffer dst, size_t dstOffset, size_t size) {
    VkCommandBuffer cb = beginOp();
    const VkBufferCopy c{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(cb, src->buffer, dst->buffer, 1, &c);
    endOp();
}

void readBuffer(Buffer b, size_t offset, size_t size, void* out) {
    if (!size) return;
    ensureReadback(size);
    VkCommandBuffer cb = beginOp();
    const VkBufferCopy c{offset, 0, size};
    vkCmdCopyBuffer(cb, b->buffer, ctx.readback, 1, &c);
    hostBarrier(cb);
    endOp();
    submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    invalidateReadback();
    std::memcpy(out, ctx.readbackPtr, size);
}

void readTexture(Texture t, int level, ReadAs as, void* out) {
    int w = t->w, h = t->h;
    for (int i = 0; i < level; ++i) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    const size_t texels = size_t(w) * size_t(h);
    const size_t bytes = texels * texelBytes(t->fmt);
    ensureReadback(bytes);
    VkCommandBuffer cb = beginOp();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
    c.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, ctx.readback, 1, &c);
    hostBarrier(cb);
    endOp();
    submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    invalidateReadback();

    // Texel by texel, as GL's glGetTextureImage converts: missing channels
    // read as 0 (alpha as 1), floats clamp and round into 8 bits.
    const int srcCh = channels(t->fmt);
    const int dstCh = as == ReadAs::R32F ? 1 : as == ReadAs::RG32F ? 2 : 4;
    const uint8_t* src8 = static_cast<const uint8_t*>(ctx.readbackPtr);
    const uint16_t* src16 = static_cast<const uint16_t*>(ctx.readbackPtr);
    uint8_t* out8 = static_cast<uint8_t*>(out);
    float* outF = static_cast<float*>(out);
    for (size_t i = 0; i < texels; ++i) {
        float v[4] = {0.f, 0.f, 0.f, 1.f};
        for (int c2 = 0; c2 < srcCh; ++c2) {
            v[c2] = t->fmt == Format::RGBA8 ? float(src8[i * 4 + c2]) / 255.f
                                            : halfToFloat(src16[i * srcCh + c2]);
        }
        if (as == ReadAs::RGBA8) {
            if (t->fmt == Format::RGBA8) {
                std::memcpy(out8 + i * 4, src8 + i * 4, 4);
            } else {
                for (int c2 = 0; c2 < 4; ++c2) out8[i * 4 + c2] = toUnorm8(v[c2]);
            }
        } else {
            for (int c2 = 0; c2 < dstCh; ++c2) outF[i * dstCh + c2] = v[c2];
        }
    }
}

// ── Binding and execution ─────────────────────────────────────────────────────

void bindTexture(unsigned unit, Texture t) { if (unit < kMaxUnits) ctx.tex[unit] = t; }
void bindImage(unsigned unit, Texture t, Access) { if (unit < kMaxImages) ctx.img[unit] = t; }
void bindStorage(unsigned binding, Buffer b) { if (binding < kMaxSsbo) ctx.ssbo[binding] = b; }

void setParams(const void* data, size_t bytes) {
    ctx.params.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes);
}

void dispatch(Program& p, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!p.impl || !p.impl->compute) return;
    const std::vector<Slice> uniforms = captureUniforms(*p.impl);
    VkCommandBuffer cb = beginOp();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.impl->pipeline);
    pushBindings(*p.impl, cb, VK_PIPELINE_BIND_POINT_COMPUTE, uniforms);
    vkCmdDispatch(cb, gx, gy, gz);
    endOp();
}

void drawIndirect(Program& p, Framebuffer fb, std::initializer_list<Blend> blend,
                  Buffer indirect) {
    if (!p.impl || p.impl->compute || !fb) return;
    const std::vector<Blend> blends(blend);
    VkPipeline pipe = graphicsPipeline(*p.impl, fb, blends);
    const std::vector<Slice> uniforms = captureUniforms(*p.impl);
    VkCommandBuffer cb = beginOp();

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = fb->renderPass;
    rbi.framebuffer = fb->framebuffer;
    rbi.renderArea = {{0, 0}, {uint32_t(fb->w), uint32_t(fb->h)}};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkViewport vp{0.f, 0.f, float(fb->w), float(fb->h), 0.f, 1.f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rbi.renderArea);
    pushBindings(*p.impl, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, uniforms);
    vkCmdDrawIndirect(cb, indirect->buffer, 0, 1, 16);
    vkCmdEndRenderPass(cb);
    endOp();
}

void memoryBarrier(unsigned) {}
void textureBarrier() {}
void finish() { submitAndWait(VK_NULL_HANDLE, 0, VK_NULL_HANDLE); }

// ── Timers ────────────────────────────────────────────────────────────────────

void Timer::begin() {
    if (!ctx.queries) return;
    if (!q[0]) {
        if (ctx.nextQuery + 4 > kQueryCount) return;
        q[0] = ctx.nextQuery + 1;
        ctx.nextQuery += 4;
    }
    const uint32_t base = q[0] - 1 + uint32_t(cur) * 2;
    VkCommandBuffer cb = recording();
    vkCmdResetQueryPool(cb, ctx.queries, base, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx.queries, base);
    started = true;
}

void Timer::end() {
    if (!started) return;
    started = false;
    const uint32_t base = q[0] - 1 + uint32_t(cur) * 2;
    vkCmdWriteTimestamp(recording(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx.queries, base + 1);
    submission[cur] = ctx.submission;
    issued[cur] = true;

    // Read the other pair, and only once the command buffer that wrote it has
    // run: every submission is waited for, so an earlier serial means done.
    const int other = 1 - cur;
    if (issued[other] && submission[other] < ctx.submission) {
        uint64_t r[4] = {0, 0, 0, 0};
        const VkResult res = vkGetQueryPoolResults(
            ctx.dev.device, ctx.queries, q[0] - 1 + uint32_t(other) * 2, 2, sizeof(r), r,
            2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (res == VK_SUCCESS && r[1] && r[3])
            lastMs = double((r[2] - r[0]) & ctx.timestampMask) * ctx.timestampPeriodNs * 1e-6;
    }
    cur = other;
}

// ── For the display ───────────────────────────────────────────────────────────
namespace vk {

const Device& device() { return ctx.dev; }

VkCommandBuffer commands() {
    VkCommandBuffer cb = beginOp();
    endOp();
    return cb;
}

void submit(VkSemaphore wait, VkPipelineStageFlags waitStage, VkSemaphore signal) {
    submitAndWait(wait, waitStage, signal);
}

void retire(std::function<void()> fn) {
    ctx.retired.push_back(std::move(fn));
    if (!ctx.recording) runRetired();
}

bool compile(const std::vector<std::pair<Stage, std::string>>& sources,
             std::vector<std::vector<uint32_t>>* spirv, std::string* err) {
    std::vector<std::string> names;
    for (size_t i = 0; i < sources.size(); ++i) names.push_back("builtin" + std::to_string(i));
    return compileStages(sources, names, /*relaxed=*/false, spirv, err);
}

VkDescriptorImageInfo sampled(Texture t, bool) {
    return {t->sampler, t->view, VK_IMAGE_LAYOUT_GENERAL};
}

VkShaderModule createModule(const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = spirv.size() * sizeof(uint32_t);
    si.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(ctx.dev.device, &si, nullptr, &m), "vkCreateShaderModule");
    return m;
}

} // namespace vk
} // namespace gpu
