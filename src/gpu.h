#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// The GPU operations the painting pipeline is written against.
//
// There are two implementations, chosen at build time:
//
//   gpu_gl.cpp   OpenGL 4.6. Windows, where Spout needs a GL context.
//   gpu_vk.cpp   Vulkan 1.1. macOS, through MoltenVK -- Apple's OpenGL stops
//                at 4.1 and has no compute shaders at all, so the GL backend
//                cannot run there. Also builds on Linux, which is how it is
//                tested without a Mac.
//
// Both run the same GLSL files. The Vulkan backend compiles them with
// glslang's relaxed Vulkan rules, which is what lets the loose `uniform int
// uPass` declarations and the GL-style binding numbers stand unchanged: loose
// uniforms are gathered into one default block, and each resource kind's
// bindings are shifted into their own range of a single descriptor set.
//
// The model is deliberately GL's rather than Vulkan's: binding points are
// global and persist until rebound, uniforms live on the program, and the
// pipeline says where it needs a memoryBarrier(). The Vulkan backend turns the
// bindings into push descriptors at each dispatch and separates every command
// from the one before it with a full barrier. That is conservative, but it is
// what the pipeline needs anyway -- nearly every stage consumes the previous
// one's output -- and it keeps a single copy of the pipeline's logic.
namespace gpu {

enum class Format { RGBA8, RGBA16F, R16F, RG16F };
enum class Wrap { Clamp, Repeat };
enum class Access { Read, Write, ReadWrite };
enum class Blend { None, PremultipliedOver, Additive };
// What readTexture converts to on the way out.
enum class ReadAs { RGBA8, RGBA32F, R32F, RG32F };
// A usage hint; only the GL backend makes use of it.
enum class BufferUse { Device, Readback, Uniform };

// Named after the glMemoryBarrier bits they stand for. The GL backend issues
// exactly those; the Vulkan backend already orders every command after the
// previous one, so for it these are no-ops.
enum : unsigned {
    BarrierTextureFetch  = 1u << 0,
    BarrierImageAccess   = 1u << 1,
    BarrierStorage       = 1u << 2,
    BarrierCommand       = 1u << 3,
    BarrierBufferUpdate  = 1u << 4,
    BarrierTextureUpdate = 1u << 5,
    BarrierFramebuffer   = 1u << 6,
};

struct TextureObj;
struct BufferObj;
struct FramebufferObj;
// Handles are pointers so that "none" is nullptr and they can be swapped,
// compared and tested like the GLuints they replace.
using Texture = TextureObj*;
using Buffer = BufferObj*;
using Framebuffer = FramebufferObj*;

struct InitOptions {
    bool debug = false;
    // Vulkan: instance extensions the window system needs for a surface.
    // Empty for a headless run, which then needs no window at all.
    std::vector<std::string> instanceExtensions;
};

// GL: the context must already be current. Vulkan: creates the instance and
// device.
bool init(const InitOptions& opt, std::string* err);
void shutdown();
// "OpenGL" or "Vulkan".
const char* backendName();
// Backend, API version and device, for the startup line.
std::string describe();

// --- resources -------------------------------------------------------------
Texture createTexture2D(int w, int h, Format f, bool mips = false);
// Filtered array texture filled from float data, one value per texel. Only
// the brush tiles use this.
Texture createTextureArray(int w, int h, int layers, Format f, const float* data,
                           Wrap u, Wrap v);
void destroy(Texture& t);          // and sets it to nullptr
int width(Texture t);
int height(Texture t);
Format format(Texture t);

Buffer createBuffer(size_t bytes, BufferUse use = BufferUse::Device);
void destroy(Buffer& b);

// Colour attachments only, all the same size.
Framebuffer createFramebuffer(std::initializer_list<Texture> colors);
void destroy(Framebuffer& fb);

// --- transfers, in order with the GPU work around them ---------------------
void uploadTexture(Texture t, const void* rgba8);      // level 0, RGBA8 only
void copyTexture(Texture src, Texture dst);             // level 0, all of src
void clearTexture(Texture t, float r, float g, float b, float a);   // level 0
void generateMipmaps(Texture t);
// size 0 means "to the end of the buffer".
void clearBuffer(Buffer b, size_t offset = 0, size_t size = 0, uint32_t value = 0);
void copyBuffer(Buffer src, size_t srcOffset, Buffer dst, size_t dstOffset, size_t size);

// Readbacks. Both wait for the GPU, so they are for the end of a frame and
// for diagnostics, never for the middle of one.
void readBuffer(Buffer b, size_t offset, size_t size, void* out);
void readTexture(Texture t, int level, ReadAs as, void* out);

// --- programs ----------------------------------------------------------------
// A compute or vertex+fragment program that remembers where it came from, so
// F5 can rebuild it. Copying shares the compiled program; assigning a freshly
// built one over an old one releases the old one.
class Program {
public:
    bool loadCompute(const std::string& comp);
    bool loadRaster(const std::string& vert, const std::string& frag);
    bool reloadIfChanged();           // true if it rebuilt successfully
    bool sourcesChanged() const;
    bool valid() const { return impl != nullptr; }

    // Uniforms outside any block. They persist on the program, as in GL.
    void setInt(const char* name, int v);
    void setUInt(const char* name, uint32_t v);
    void setIVec2(const char* name, int x, int y);
    void setVec3(const char* name, float x, float y, float z);

    std::vector<std::string> paths;   // compute: 1 path; raster: vert, frag
    std::vector<int64_t> mtimes;
    std::string lastError;

    struct Impl;
    std::shared_ptr<Impl> impl;

private:
    bool build();
};

// --- binding and execution ---------------------------------------------------
// Binding numbers are the ones written in the shaders.
void bindTexture(unsigned unit, Texture t);            // `layout(binding = u) sampler`
void bindImage(unsigned unit, Texture t, Access a);    // level 0, the texture's format
void bindStorage(unsigned binding, Buffer b);          // std430 buffer blocks
// The `Params` uniform block at binding 0. The contents are captured when a
// dispatch or draw is issued, so updating it between two dispatches gives
// each the value it was issued with.
void setParams(const void* data, size_t bytes);

void dispatch(Program& p, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ);
// One triangle-strip draw whose command (vertex count, instance count, first,
// base instance) is read from `indirect` at offset 0. `blend` gives each
// colour attachment's blending.
void drawIndirect(Program& p, Framebuffer fb, std::initializer_list<Blend> blend,
                  Buffer indirect);

void memoryBarrier(unsigned bits);
// Lets a pass sample what the previous draw rendered (glTextureBarrier).
void textureBarrier();
// Waits for everything issued so far.
void finish();

// A GPU stage timer, double-buffered so reading it never stalls: end() reads
// the measurement before the one it just finished.
struct Timer {
    void begin();
    void end();
    double lastMs = 0.0;

    // Backend state. GL: two query objects. Vulkan: q[0] is one past the
    // first of this timer's four timestamp slots, and `submission` records
    // which command buffer wrote each pair, since a pair can only be read
    // once the commands that wrote it have run.
    uint32_t q[2] = {0, 0};
    int cur = 0;
    bool started = false;
    bool issued[2] = {false, false};
    uint64_t submission[2] = {0, 0};
};

} // namespace gpu
