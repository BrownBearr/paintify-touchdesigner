// Metal implementation of the small GL-shaped API used by pipeline.cpp.
// GLSL remains the source of truth; SPIRV-Cross supplies Metal resource indices.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include "gl_util.h"
#include "app_resources.h"
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_metal.h>
#include <map>
#include <spirv_msl.hpp>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
id<MTLDevice> device;
id<MTLCommandQueue> queue;
id<MTLCommandBuffer> commands;
CAMetalLayer *layer;
id<CAMetalDrawable> drawable;
GLFWwindow *window;
GLuint nextID = 1, current = 0, framebuffer = 0, indirect = 0;
struct Texture {
    id<MTLTexture> object;
    GLenum target = GL_TEXTURE_2D;
};
std::map<GLuint, Texture> textures;
std::map<GLuint, id<MTLBuffer>> buffers;
std::map<GLuint, std::array<GLuint, 2>> fbos;
std::array<GLuint, 16> sampled{}, images{}, storage{};
std::array<GLuint, 16> ubos{};
bool flattenHeight = false;
struct Resource {
    enum Kind { Ubo, Ssbo, Sampled, Image } kind;
    unsigned binding, index, sampler;
};
struct Stage {
    id<MTLFunction> function;
    std::vector<Resource> resources;
    std::map<std::string, std::pair<unsigned, unsigned>> uniforms;
    unsigned uniformIndex = 31, uniformSize = 0;
    bool needsSizes = false;
    MTLSize group = MTLSizeMake(1, 1, 1);
};
struct Shader {
    Stage comp, vert, frag;
    id<MTLComputePipelineState> compute;
    std::array<id<MTLRenderPipelineState>, 2> render;
    std::map<GLint, std::string> locations;
    std::map<std::string, std::array<uint32_t, 4>> values;
};
std::map<GLuint, Shader> programs;
int viewport[4] = {}, scissor[4] = {};
bool scissorOn = false, uiReady = false;
MTLClearColor clearColor = MTLClearColorMake(.09, .09, .10, 1);
std::vector<double> timings;
size_t activeTimer = 0;
bool timerOn = false;

[[noreturn]] void fail(const std::string &s) {
    fprintf(stderr, "%s\n", s.c_str());
    throw std::runtime_error(s);
}
void check(NSError *err, const char *what) {
    if (err)
        fail(std::string(what) + ": " + err.localizedDescription.UTF8String);
}
struct EncoderScope {
    id<MTLCommandEncoder> encoder;
    explicit EncoderScope(id<MTLCommandEncoder> e) : encoder(e) {}
    ~EncoderScope() { [encoder endEncoding]; }
};
id<MTLCommandBuffer> command() {
    if (!commands)
        commands = [queue commandBuffer];
    return commands;
}
void submit(bool wait = false) {
    if (!commands)
        return;
    auto c = commands;
    commands = nil;
    [c commit];
    if (wait || timerOn) {
        [c waitUntilCompleted];
        check(c.error, "Metal command");
        if (timerOn)
            timings[activeTimer] += (c.GPUEndTime - c.GPUStartTime) * 1000;
    }
}
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}
std::string quote(const std::string &s) {
    std::string r = "'";
    for (char c : s)
        r += c == '\'' ? "'\"'\"'" : std::string(1, c);
    return r + "'";
}
std::filesystem::path shaderDirectory() {
    for (const char *name : {"BRUSHKIT_SHADER_DIR", "PAINTIFY_SHADER_DIR"}) {
        const char *dir = std::getenv(name);
        if (dir && *dir)
            return dir;
    }
    return appResourcePath("shaders");
}
std::string shaderSource(const std::string &filename) {
    std::string source = read(shaderDirectory() / filename);
    if (source.empty())
        fail("Cannot read shader " + filename);
    for (const char *include : {"common.glsl", "style.glsl"}) {
        const std::string directive = std::string("#include \"") + include + "\"";
        size_t pos = source.find(directive);
        if (pos != std::string::npos)
            source.replace(pos, directive.size(), read(shaderDirectory() / include));
    }
    return source;
}
Stage compileStage(const std::string &filename) {
    std::string src = shaderSource(filename);
    // Vulkan SPIR-V puts standalone uniforms in a reflected default block.
    char pattern[] = "/tmp/paintify-shader-XXXXXX";
    char *path = mkdtemp(pattern);
    if (!path)
        fail("Cannot create shader scratch directory");
    std::filesystem::path temp(path);
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(p, ec);
        }
    } cleanup{temp};
    std::ofstream(temp / filename) << src;
    const auto bundledCompiler = appResourcePath("tools/glslang");
    std::string cmd = quote(std::filesystem::exists(bundledCompiler)
                            ? bundledCompiler.string() : std::string(SBR_GLSLANG)) +
                      " -V -R --auto-map-bindings --auto-map-locations --set-default-uniform-block "
                      "SbrUniforms 0 20 -o " +
                      quote((temp / "shader.spv").string()) + " " +
                      quote((temp / filename).string()) + " > " + quote((temp / "log").string()) +
                      " 2>&1";
    if (system(cmd.c_str()) != 0)
        fail(filename + ":\n" + read(temp / "log"));
    std::ifstream f(temp / "shader.spv", std::ios::binary | std::ios::ate);
    std::vector<uint32_t> words(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read((char *)words.data(), words.size() * 4);
    spirv_cross::CompilerMSL compiler(std::move(words));
    auto options = compiler.get_msl_options();
    options.set_msl_version(2, 3);
    compiler.set_msl_options(options);
    auto common = compiler.get_common_options();
    common.vertex.flip_vert_y = true;
    compiler.set_common_options(common);
    auto originalResources = compiler.get_shader_resources();
    // GL gives buffers, sampled textures, and images separate binding namespaces.
    // Preserve that separation before SPIRV-Cross resolves Vulkan descriptors.
    for (auto &r : originalResources.uniform_buffers)
        compiler.set_decoration(r.id, spv::DecorationDescriptorSet, 1);
    for (auto &r : originalResources.storage_buffers)
        compiler.set_decoration(r.id, spv::DecorationDescriptorSet, 2);
    for (auto &r : originalResources.sampled_images)
        compiler.set_decoration(r.id, spv::DecorationDescriptorSet, 3);
    for (auto &r : originalResources.storage_images)
        compiler.set_decoration(r.id, spv::DecorationDescriptorSet, 4);
    std::string msl = compiler.compile();
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                              options:nil
                                                error:&err];
    check(err, filename.c_str());
    Stage stage;
    const auto ep = compiler.get_entry_points_and_stages().front();
    std::string entry = compiler.get_cleansed_entry_point_name(ep.name, ep.execution_model);
    stage.function = [lib newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
    if (!stage.function)
        fail("Missing Metal entry point " + entry);
    stage.needsSizes = compiler.needs_buffer_size_buffer();
    auto resources = compiler.get_shader_resources();
    for (auto &r : resources.uniform_buffers) {
        unsigned index = compiler.get_automatic_msl_resource_binding(r.id);
        if (r.name == "SbrUniforms" || compiler.get_name(r.base_type_id) == "SbrUniforms") {
            const auto &t = compiler.get_type(r.base_type_id);
            stage.uniformIndex = index;
            stage.uniformSize = (unsigned(compiler.get_declared_struct_size(t)) + 15u) & ~15u;
            for (unsigned i = 0; i < t.member_types.size(); ++i)
                stage.uniforms[compiler.get_member_name(r.base_type_id, i)] = {
                    compiler.type_struct_member_offset(t, i),
                    unsigned(compiler.get_declared_struct_member_size(t, i))};
        } else
            stage.resources.push_back(
                {Resource::Ubo, compiler.get_decoration(r.id, spv::DecorationBinding), index, 0});
    }
    for (auto &r : resources.storage_buffers)
        stage.resources.push_back({Resource::Ssbo,
                                   compiler.get_decoration(r.id, spv::DecorationBinding),
                                   compiler.get_automatic_msl_resource_binding(r.id), 0});
    for (auto &r : resources.sampled_images)
        stage.resources.push_back({Resource::Sampled,
                                   compiler.get_decoration(r.id, spv::DecorationBinding),
                                   compiler.get_automatic_msl_resource_binding(r.id),
                                   compiler.get_automatic_msl_resource_binding_secondary(r.id)});
    for (auto &r : resources.storage_images)
        stage.resources.push_back({Resource::Image,
                                   compiler.get_decoration(r.id, spv::DecorationBinding),
                                   compiler.get_automatic_msl_resource_binding(r.id), 0});
    if (ep.execution_model == spv::ExecutionModelGLCompute)
        stage.group =
            MTLSizeMake(compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0),
                        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1),
                        compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2));
    return stage;
}
MTLPixelFormat format(GLenum f) {
    if (f == GL_RGBA8)
        return MTLPixelFormatRGBA8Unorm;
    if (f == GL_RGBA16F)
        return MTLPixelFormatRGBA16Float;
    if (f == GL_RG16F)
        return MTLPixelFormatRG16Float;
    if (f == GL_R16F)
        return MTLPixelFormatR16Float;
    fail("Unsupported Metal texture format");
}
size_t pixelBytes(MTLPixelFormat f) {
    return f == MTLPixelFormatRGBA8Unorm    ? 4
           : f == MTLPixelFormatRGBA16Float ? 8
           : f == MTLPixelFormatRG16Float   ? 4
                                            : 2;
}
unsigned channels(GLenum f) { return f == GL_RGBA ? 4 : f == GL_RG ? 2 : 1; }
uint16_t half(float v) {
    __fp16 h = v;
    uint16_t b;
    memcpy(&b, &h, 2);
    return b;
}
float unhalf(uint16_t b) {
    __fp16 h;
    memcpy(&h, &b, 2);
    return float(h);
}
id<MTLSamplerState> sampler(bool nearest = false, bool repeat = false) {
    static id<MTLSamplerState> states[2][2];
    auto &s = states[nearest][repeat];
    if (!s) {
        auto d = [MTLSamplerDescriptor new];
        d.minFilter = d.magFilter =
            nearest ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
        d.mipFilter = MTLSamplerMipFilterLinear;
        d.sAddressMode = repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
        d.tAddressMode = MTLSamplerAddressModeClampToEdge;
        s = [device newSamplerStateWithDescriptor:d];
    }
    return s;
}
void bind(id encoder, const Stage &stage, int kind, const Shader &shader) {
    for (const auto &r : stage.resources) {
        if (r.kind == Resource::Ubo || r.kind == Resource::Ssbo) {
            id<MTLBuffer> b =
                buffers[r.kind == Resource::Ubo ? ubos.at(r.binding) : storage.at(r.binding)];
            if (!b) {
                // Resources used only by another uPass branch can be unbound.
                static id<MTLBuffer> dummy;
                if (!dummy)
                    dummy = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                b = dummy;
                buffers[0] = dummy;
            }
            if (kind == 0)
                [(id<MTLComputeCommandEncoder>)encoder setBuffer:b offset:0 atIndex:r.index];
            else if (kind == 1)
                [(id<MTLRenderCommandEncoder>)encoder setVertexBuffer:b offset:0 atIndex:r.index];
            else
                [(id<MTLRenderCommandEncoder>)encoder setFragmentBuffer:b offset:0 atIndex:r.index];
        } else {
            auto t =
                textures[r.kind == Resource::Image ? images.at(r.binding) : sampled.at(r.binding)];
            if (!t.object) {
                // Inactive shader branches still declare resources. Metal needs
                // a valid binding even when that branch never samples it.
                static id<MTLTexture> dummy;
                if (!dummy) {
                    auto d = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                     width:1
                                                    height:1
                                                 mipmapped:NO];
                    d.storageMode = MTLStorageModeShared;
                    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                    dummy = [device newTextureWithDescriptor:d];
                    uint16_t zero[4] = {};
                    [dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                             mipmapLevel:0
                               withBytes:zero
                             bytesPerRow:8];
                }
                t.object = dummy;
            }
            if (kind == 0)
                [(id<MTLComputeCommandEncoder>)encoder setTexture:t.object atIndex:r.index];
            else if (kind == 1)
                [(id<MTLRenderCommandEncoder>)encoder setVertexTexture:t.object atIndex:r.index];
            else
                [(id<MTLRenderCommandEncoder>)encoder setFragmentTexture:t.object atIndex:r.index];
            if (r.kind == Resource::Sampled) {
                auto s = sampler(false, t.target == GL_TEXTURE_2D_ARRAY);
                if (kind == 0)
                    [(id<MTLComputeCommandEncoder>)encoder setSamplerState:s atIndex:r.sampler];
                else if (kind == 1)
                    [(id<MTLRenderCommandEncoder>)encoder setVertexSamplerState:s
                                                                        atIndex:r.sampler];
                else
                    [(id<MTLRenderCommandEncoder>)encoder setFragmentSamplerState:s
                                                                          atIndex:r.sampler];
            }
        }
    }
    if (stage.needsSizes) {
        std::array<uint32_t, 32> sizes{};
        for (const auto &r : stage.resources)
            if (r.kind == Resource::Ssbo)
                sizes.at(r.index) = uint32_t(buffers.at(storage.at(r.binding)).length);
        if (kind == 0)
            [(id<MTLComputeCommandEncoder>)encoder setBytes:sizes.data()
                                                     length:sizeof(sizes)
                                                    atIndex:25];
        else if (kind == 1)
            [(id<MTLRenderCommandEncoder>)encoder setVertexBytes:sizes.data()
                                                          length:sizeof(sizes)
                                                         atIndex:25];
        else
            [(id<MTLRenderCommandEncoder>)encoder setFragmentBytes:sizes.data()
                                                            length:sizeof(sizes)
                                                           atIndex:25];
    }
    if (stage.uniformSize) {
        std::vector<uint8_t> data(stage.uniformSize);
        for (auto &[name, layout] : stage.uniforms) {
            auto v = shader.values.find(name);
            if (v != shader.values.end())
                memcpy(data.data() + layout.first, v->second.data(),
                       std::min(size_t(layout.second), sizeof(v->second)));
        }
        if (kind == 0)
            [(id<MTLComputeCommandEncoder>)encoder setBytes:data.data()
                                                     length:data.size()
                                                    atIndex:stage.uniformIndex];
        else if (kind == 1)
            [(id<MTLRenderCommandEncoder>)encoder setVertexBytes:data.data()
                                                          length:data.size()
                                                         atIndex:stage.uniformIndex];
        else
            [(id<MTLRenderCommandEncoder>)encoder setFragmentBytes:data.data()
                                                            length:data.size()
                                                           atIndex:stage.uniformIndex];
    }
}

} // namespace

namespace glu {
bool Program::build() {
    try {
        Shader p;
        if (paths.size() == 1) {
            p.comp = compileStage(paths[0]);
            NSError *e = nil;
            p.compute = [device newComputePipelineStateWithFunction:p.comp.function error:&e];
            check(e, "Compute pipeline");
        } else {
            p.vert = compileStage(paths[0]);
            p.frag = compileStage(paths[1]);
            auto d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = p.vert.function;
            d.fragmentFunction = p.frag.function;
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
            d.colorAttachments[1].pixelFormat = MTLPixelFormatR16Float;
            for (int i = 0; i < 2; ++i) {
                auto a = d.colorAttachments[i];
                a.blendingEnabled = YES;
                a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOne;
                a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor =
                    i == 0 ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
            }
            for (int mode = 0; mode < 2; ++mode) {
                d.colorAttachments[1].destinationRGBBlendFactor =
                    d.colorAttachments[1].destinationAlphaBlendFactor =
                        mode ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
                NSError *e = nil;
                p.render[mode] = [device newRenderPipelineStateWithDescriptor:d error:&e];
                check(e, "Stroke pipeline");
            }
        }
        if (id)
            programs.erase(id);
        id = nextID++;
        programs[id] = std::move(p);
        mtimes.clear();
        for (auto &path : paths)
            mtimes.push_back(std::filesystem::last_write_time(shaderDirectory() / path)
                                 .time_since_epoch()
                                 .count());
        mtimes.push_back(std::filesystem::last_write_time(shaderDirectory() / "common.glsl")
                             .time_since_epoch()
                             .count());
        mtimes.push_back(std::filesystem::last_write_time(shaderDirectory() / "style.glsl")
                             .time_since_epoch()
                             .count());
        lastError.clear();
        return true;
    } catch (const std::exception &e) {
        lastError = e.what();
        return false;
    }
}
bool Program::loadCompute(const std::string &p) {
    paths = {p};
    return build();
}
bool Program::loadRaster(const std::string &a, const std::string &b) {
    paths = {a, b};
    return build();
}
bool Program::sourcesChanged() const {
    if (mtimes.size() != paths.size() + 2)
        return true;
    auto all = paths;
    all.push_back("common.glsl");
    all.push_back("style.glsl");
    for (size_t i = 0; i < all.size(); ++i) {
        std::error_code e;
        auto t = std::filesystem::last_write_time(shaderDirectory() / all[i], e);
        if (e || t.time_since_epoch().count() != mtimes[i])
            return true;
    }
    return false;
}
bool Program::reloadIfChanged() { return sourcesChanged() && build(); }
std::string loadShaderSource(const std::string &p, std::string *) { return shaderSource(p); }
GLuint createBuffer(GLenum, GLsizeiptr bytes, const void *data, GLenum) {
    auto b = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (!b)
        fail("Metal buffer allocation failed");
    if (data)
        memcpy(b.contents, data, bytes);
    GLuint id = nextID++;
    buffers[id] = b;
    return id;
}
GLuint createTexture2D(int w, int h, GLenum f, bool mips) {
    GLuint t;
    glCreateTextures(GL_TEXTURE_2D, 1, &t);
    int n = 1;
    if (mips)
        for (int m = std::max(w, h); m > 1; m >>= 1)
            ++n;
    glTextureStorage2D(t, n, f, w, h);
    return t;
}
void enableDebugOutput() {}
void GpuTimer::init() {
    q[0] = GLuint(timings.size());
    timings.push_back(0);
}
void GpuTimer::begin() {
    submit(true);
    timings[q[0]] = 0;
    activeTimer = q[0];
    timerOn = true;
    started = true;
}
void GpuTimer::end() {
    if (!started)
        return;
    submit(true);
    lastMs = timings[q[0]];
    timerOn = false;
    started = false;
}
} // namespace glu
void glUseProgram(GLuint p) { current = p; }
GLint glGetUniformLocation(GLuint p, const char *n) {
    auto &loc = programs.at(p).locations;
    for (auto &[i, name] : loc)
        if (name == n)
            return i;
    GLint i = GLint(loc.size());
    loc[i] = n;
    return i;
}
void glDeleteProgram(GLuint p) { programs.erase(p); }
void glFinish() { submit(true); }
void glCreateTextures(GLenum target, int n, GLuint *out) {
    for (int i = 0; i < n; ++i) {
        out[i] = nextID++;
        textures[out[i]].target = target;
    }
}
void glDeleteTextures(int n, const GLuint *t) {
    submit(true);
    for (int i = 0; i < n; ++i)
        textures.erase(t[i]);
}
void glTextureStorage3D(GLuint t, int levels, GLenum f, int w, int h, int layers) {
    auto d = [MTLTextureDescriptor new];
    d.textureType =
        textures[t].target == GL_TEXTURE_2D_ARRAY ? MTLTextureType2DArray : MTLTextureType2D;
    d.pixelFormat = format(f);
    d.width = w;
    d.height = h;
    d.arrayLength = layers;
    d.mipmapLevelCount = levels;
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    textures[t].object = [device newTextureWithDescriptor:d];
    if (!textures[t].object)
        fail("Metal texture allocation failed");
}
void glTextureStorage2D(GLuint t, int levels, GLenum f, int w, int h) {
    glTextureStorage3D(t, levels, f, w, h, 1);
}
void glTextureParameteri(GLuint, GLenum, GLint) {}
void glTextureSubImage3D(GLuint t, int level, int x, int y, int z, int w, int h, int depth,
                         GLenum f, GLenum type, const void *data) {
    submit(true);
    auto tex = textures.at(t).object;
    size_t bpp = pixelBytes(tex.pixelFormat), count = size_t(w) * h;
    for (int s = 0; s < depth; ++s) {
        const uint8_t *p =
            (const uint8_t *)data + size_t(s) * count * channels(f) * (type == GL_FLOAT ? 4 : 1);
        std::vector<uint16_t> converted;
        if (type == GL_FLOAT) {
            converted.resize(count * channels(f));
            for (size_t i = 0; i < converted.size(); ++i)
                converted[i] = half(((const float *)p)[i]);
            p = (uint8_t *)converted.data();
        }
        [tex replaceRegion:MTLRegionMake2D(x, y, w, h)
               mipmapLevel:level
                     slice:z + s
                 withBytes:p
               bytesPerRow:w * bpp
             bytesPerImage:count * bpp];
    }
}
void glTextureSubImage2D(GLuint t, int l, int x, int y, int w, int h, GLenum f, GLenum ty,
                         const void *p) {
    glTextureSubImage3D(t, l, x, y, 0, w, h, 1, f, ty, p);
}
void glGenerateTextureMipmap(GLuint t) {
    auto tex = textures.at(t).object;
    if (tex.mipmapLevelCount <= 1)
        return;
    auto e = [command() blitCommandEncoder];
    EncoderScope end(e);
    [e generateMipmapsForTexture:tex];
}
void glGetTextureImage(GLuint t, int level, GLenum f, GLenum type, GLsizei capacity, void *out) {
    submit(true);
    auto tex = textures.at(t).object;
    size_t w = std::max(size_t(1), tex.width >> level),
           h = std::max(size_t(1), tex.height >> level), count = w * h;
    size_t bpp = pixelBytes(tex.pixelFormat);
    std::vector<uint8_t> raw(count * bpp);
    [tex getBytes:raw.data()
        bytesPerRow:w * bpp
         fromRegion:MTLRegionMake2D(0, 0, w, h)
        mipmapLevel:level];
    unsigned c = channels(f);
    size_t need = count * c * (type == GL_FLOAT ? 4 : 1);
    if (need > size_t(capacity))
        fail("Readback buffer too small");
    for (size_t i = 0; i < count * c; ++i) {
        float v = tex.pixelFormat == MTLPixelFormatRGBA8Unorm ? raw[i] / 255.f
                                                              : unhalf(((uint16_t *)raw.data())[i]);
        if (type == GL_FLOAT)
            ((float *)out)[i] = v;
        else
            ((uint8_t *)out)[i] = uint8_t(std::clamp(v, 0.f, 1.f) * 255 + .5f);
    }
}
void glClearTexImage(GLuint t, int level, GLenum f, GLenum type, const void *value) {
    auto tex = textures.at(t).object;
    int w = std::max(1, int(tex.width) >> level), h = std::max(1, int(tex.height) >> level);
    unsigned c = channels(f);
    std::vector<float> data(size_t(w) * h * c);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = type == GL_FLOAT ? ((const float *)value)[i % c]
                                   : ((const uint8_t *)value)[i % c] / 255.f;
    glTextureSubImage2D(t, level, 0, 0, w, h, f, GL_FLOAT, data.data());
}
void glCopyImageSubData(GLuint a, GLenum, int al, int ax, int ay, int az, GLuint b, GLenum, int bl,
                        int bx, int by, int bz, int w, int h, int depth) {
    auto e = [command() blitCommandEncoder];
    EncoderScope end(e);
    for (int i = 0; i < depth; ++i)
        [e copyFromTexture:textures.at(a).object
                  sourceSlice:az + i
                  sourceLevel:al
                 sourceOrigin:MTLOriginMake(ax, ay, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:textures.at(b).object
             destinationSlice:bz + i
             destinationLevel:bl
            destinationOrigin:MTLOriginMake(bx, by, 0)];
}
void glBindTextureUnit(GLuint u, GLuint t) { sampled.at(u) = t; }
void glBindImageTexture(GLuint u, GLuint t, int, GLboolean, int, GLenum, GLenum) {
    images.at(u) = t;
}
void glDeleteBuffers(int n, const GLuint *b) {
    submit(true);
    for (int i = 0; i < n; ++i)
        buffers.erase(b[i]);
}
void glNamedBufferSubData(GLuint b, GLintptr o, GLsizeiptr n, const void *d) {
    submit(true);
    memcpy((char *)buffers.at(b).contents + o, d, n);
}
void glGetNamedBufferSubData(GLuint b, GLintptr o, GLsizeiptr n, void *d) {
    submit(true);
    memcpy(d, (char *)buffers.at(b).contents + o, n);
}
void glClearNamedBufferSubData(GLuint b, GLenum, GLintptr o, GLsizeiptr n, GLenum, GLenum,
                               const void *v) {
    submit(true);
    auto p = (uint32_t *)((char *)buffers.at(b).contents + o);
    for (size_t i = 0; i < size_t(n) / 4; ++i)
        p[i] = *(const uint32_t *)v;
}
void glClearNamedBufferData(GLuint b, GLenum i, GLenum f, GLenum ty, const void *v) {
    glClearNamedBufferSubData(b, i, 0, buffers.at(b).length, f, ty, v);
}
void glCopyNamedBufferSubData(GLuint a, GLuint b, GLintptr ao, GLintptr bo, GLsizeiptr n) {
    auto e = [command() blitCommandEncoder];
    EncoderScope end(e);
    [e copyFromBuffer:buffers.at(a)
             sourceOffset:ao
                 toBuffer:buffers.at(b)
        destinationOffset:bo
                     size:n];
}
void glBindBufferBase(GLenum target, GLuint i, GLuint b) {
    if (target == GL_UNIFORM_BUFFER)
        ubos.at(i) = b;
    else
        storage.at(i) = b;
}
void glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_DRAW_INDIRECT_BUFFER)
        indirect = b;
}
void glCreateFramebuffers(int n, GLuint *p) {
    for (int i = 0; i < n; ++i) {
        p[i] = nextID++;
        fbos[p[i]] = {0, 0};
    }
}
void glDeleteFramebuffers(int n, const GLuint *p) {
    for (int i = 0; i < n; ++i)
        fbos.erase(p[i]);
}
void glNamedFramebufferTexture(GLuint f, GLenum attachment, GLuint t, int) {
    fbos.at(f)[attachment == GL_COLOR_ATTACHMENT1 ? 1 : 0] = t;
}
void glNamedFramebufferDrawBuffers(GLuint, int, const GLenum *) {}
void glBindFramebuffer(GLenum, GLuint f) { framebuffer = f; }
void glViewport(int x, int y, int w, int h) {
    viewport[0] = x;
    viewport[1] = y;
    viewport[2] = w;
    viewport[3] = h;
}
void glDrawArraysIndirect(GLenum, const void *) {
    auto &p = programs.at(current);
    auto d = [MTLRenderPassDescriptor renderPassDescriptor];
    auto f = fbos.at(framebuffer);
    for (int i = 0; i < 2; ++i) {
        d.colorAttachments[i].texture = textures.at(f[i]).object;
        d.colorAttachments[i].loadAction = MTLLoadActionLoad;
        d.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    auto c = command();
    auto e = [c renderCommandEncoderWithDescriptor:d];
    EncoderScope end(e);
    [e setRenderPipelineState:p.render[flattenHeight ? 1 : 0]];
    [e setViewport:MTLViewport{double(viewport[0]), double(viewport[1]), double(viewport[2]),
                               double(viewport[3]), 0, 1}];
    bind(e, p.vert, 1, p);
    bind(e, p.frag, 2, p);
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip
              indirectBuffer:buffers.at(indirect)
        indirectBufferOffset:0];
}
static void uniform(GLint i, const std::array<uint32_t, 4> &v) {
    auto &p = programs.at(current);
    p.values[p.locations.at(i)] = v;
}
void glUniform1i(GLint i, int v) { uniform(i, {uint32_t(v), 0, 0, 0}); }
void glUniform1ui(GLint i, GLuint v) { uniform(i, {v, 0, 0, 0}); }
void glUniform2i(GLint i, int a, int b) { uniform(i, {uint32_t(a), uint32_t(b), 0, 0}); }
void glUniform3f(GLint i, float a, float b, float c) {
    std::array<uint32_t, 4> v{};
    memcpy(&v[0], &a, 4);
    memcpy(&v[1], &b, 4);
    memcpy(&v[2], &c, 4);
    uniform(i, v);
}
void glDispatchCompute(GLuint x, GLuint y, GLuint z) {
    auto &p = programs.at(current);
    auto c = command();
    auto e = [c computeCommandEncoder];
    EncoderScope end(e);
    [e setComputePipelineState:p.compute];
    bind(e, p.comp, 0, p);
    [e dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:p.comp.group];
}
void glMemoryBarrier(GLbitfield) {} // Encoders execute in order with tracked Metal resources.
void glTextureBarrier() {}
void glGenVertexArrays(int n, GLuint *p) {
    for (int i = 0; i < n; ++i)
        p[i] = nextID++;
}
void glDeleteVertexArrays(int, const GLuint *) {}
void glBindVertexArray(GLuint) {}
void glBlendFunci(GLuint attachment, GLenum, GLenum dst) {
    if (attachment == 1)
        flattenHeight = dst == GL_ONE_MINUS_SRC_ALPHA;
}
void glEnable(GLenum v) {
    if (v == GL_SCISSOR_TEST)
        scissorOn = true;
}
void glDisable(GLenum v) {
    if (v == GL_SCISSOR_TEST)
        scissorOn = false;
}
GLboolean glIsEnabled(GLenum v) { return v == GL_SCISSOR_TEST && scissorOn; }
void glGetIntegerv(GLenum, GLint *p) { memcpy(p, scissor, sizeof(scissor)); }
void glScissor(int x, int y, int w, int h) {
    scissor[0] = x;
    scissor[1] = y;
    scissor[2] = w;
    scissor[3] = h;
}
void glClearColor(float r, float g, float b, float a) {
    clearColor = MTLClearColorMake(r, g, b, a);
}
namespace {
id<MTLRenderPipelineState> screenPipeline;
MTLRenderPassDescriptor *screenPass(bool clear = false) {
    if (!drawable) {
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        drawable = [layer nextDrawable];
    }
    if (!drawable)
        return nil;
    auto d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = drawable.texture;
    d.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.colorAttachments[0].clearColor = clearColor;
    return d;
}
} // namespace
void glClear(GLbitfield) {
    auto d = screenPass(!scissorOn);
    if (d) {
        auto e = [command() renderCommandEncoderWithDescriptor:d];
        EncoderScope end(e);
        if (scissorOn) {
            static id<MTLRenderPipelineState> clearPipeline;
            if (!clearPipeline) {
                const char *source = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 clearVertex(uint i[[vertex_id]]) {
    return float4(float2((i&1)!=0,(i&2)!=0)*2-1,0,1);
}
fragment float4 clearFragment(constant float4& color[[buffer(0)]]) { return color; }
)";
                NSError *err = nil;
                auto lib = [device newLibraryWithSource:[NSString stringWithUTF8String:source]
                                                options:nil error:&err];
                check(err, "Backdrop shader");
                auto p = [MTLRenderPipelineDescriptor new];
                p.vertexFunction = [lib newFunctionWithName:@"clearVertex"];
                p.fragmentFunction = [lib newFunctionWithName:@"clearFragment"];
                p.colorAttachments[0].pixelFormat = layer.pixelFormat;
                clearPipeline = [device newRenderPipelineStateWithDescriptor:p error:&err];
                check(err, "Backdrop pipeline");
            }
            const int w = int(drawable.texture.width), h = int(drawable.texture.height);
            const int x = std::clamp(scissor[0], 0, w);
            const int y = std::clamp(h - scissor[1] - scissor[3], 0, h);
            const int right = std::clamp(scissor[0] + scissor[2], x, w);
            const int bottom = std::clamp(h - scissor[1], y, h);
            if (right == x || bottom == y) return;
            const float color[] = {float(clearColor.red), float(clearColor.green),
                                   float(clearColor.blue), float(clearColor.alpha)};
            [e setRenderPipelineState:clearPipeline];
            [e setScissorRect:MTLScissorRect{NSUInteger(x), NSUInteger(y),
                NSUInteger(right - x), NSUInteger(bottom - y)}];
            [e setFragmentBytes:color length:sizeof(color) atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
    }
}
void glBlitNamedFramebuffer(GLuint f, GLuint, int sx0, int sy0, int sx1, int sy1, int x0, int y0,
                            int x1, int y1, GLbitfield, GLenum filter) {
    auto d = screenPass();
    if (!d || x0 == x1 || y0 == y1)
        return;
    if (!screenPipeline) {
        const char *src = R"(
#include <metal_stdlib>
using namespace metal;
struct V{float4 p[[position]];float2 uv;};
vertex V displayVertex(uint i[[vertex_id]],constant float4* p[[buffer(0)]]){float2 q=float2((i&1)!=0,(i&2)!=0);V o;o.p=float4(mix(p[0].xy,p[0].zw,q),0,1);o.uv=mix(p[1].xy,p[1].zw,q);return o;}
fragment float4 displayFragment(V v[[stage_in]],texture2d<float> t[[texture(0)]],sampler s[[sampler(0)]]){return float4(t.sample(s,v.uv).rgb,1);}
)";
        NSError *err = nil;
        auto lib = [device newLibraryWithSource:[NSString stringWithUTF8String:src]
                                        options:nil
                                          error:&err];
        check(err, "Preview shader");
        auto p = [MTLRenderPipelineDescriptor new];
        p.vertexFunction = [lib newFunctionWithName:@"displayVertex"];
        p.fragmentFunction = [lib newFunctionWithName:@"displayFragment"];
        p.colorAttachments[0].pixelFormat = layer.pixelFormat;
        screenPipeline = [device newRenderPipelineStateWithDescriptor:p error:&err];
        check(err, "Preview pipeline");
    }
    auto tex = textures.at(fbos.at(f)[0]).object;
    float w = drawable.texture.width, h = drawable.texture.height;
    // GL screen coordinates grow upward; Metal's viewport starts at the top.
    float v[8] = {2 * x0 / w - 1,         2 * y0 / h - 1,         2 * x1 / w - 1,
                  2 * y1 / h - 1,         float(sx0) / tex.width, float(sy0) / tex.height,
                  float(sx1) / tex.width, float(sy1) / tex.height};
    auto e = [command() renderCommandEncoderWithDescriptor:d];
    EncoderScope end(e);
    [e setRenderPipelineState:screenPipeline];
    [e setVertexBytes:v length:sizeof(v) atIndex:0];
    [e setFragmentTexture:tex atIndex:0];
    [e setFragmentSamplerState:sampler(filter == GL_NEAREST) atIndex:0];
    if (scissorOn) {
        int x = std::clamp(scissor[0], 0, int(w)),
            y = std::clamp(int(h) - scissor[1] - scissor[3], 0, int(h));
        int right = std::clamp(scissor[0] + scissor[2], x, int(w)),
            bottom = std::clamp(int(h) - scissor[1], y, int(h));
        [e setScissorRect:MTLScissorRect{NSUInteger(x), NSUInteger(y), NSUInteger(right - x),
                                         NSUInteger(bottom - y)}];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
namespace metal {
bool init(GLFWwindow *w, std::string *err) {
    @autoreleasepool {
        try {
            window = w;
            device = MTLCreateSystemDefaultDevice();
            if (!device)
                fail("This Mac has no Metal GPU");
            queue = [device newCommandQueue];
            if (w) {
                NSWindow *n = glfwGetCocoaWindow(w);
                layer = [CAMetalLayer layer];
                layer.device = device;
                layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer.framebufferOnly = YES;
                n.contentView.wantsLayer = YES;
                n.contentView.layer = layer;
            }
            printf("Metal | %s\n", device.name.UTF8String);
            return true;
        } catch (const std::exception &e) {
            if (err)
                *err = e.what();
            return false;
        }
    }
}
void newFrame() {
    if (!uiReady) {
        ImGui_ImplMetal_Init(device);
        uiReady = true;
    }
    auto d = screenPass();
    if (d)
        ImGui_ImplMetal_NewFrame(d);
}
void drawUI() {
    auto d = screenPass();
    if (!d)
        return;
    auto e = [command() renderCommandEncoderWithDescriptor:d];
    EncoderScope end(e);
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command(), e);
}
uint64_t uiTexture(GLuint texture) {
    return uint64_t(uintptr_t((__bridge void*)textures.at(texture).object));
}
void rebuildFonts() {
    if (uiReady) ImGui_ImplMetal_DestroyFontsTexture();
}
void present() {
    if (drawable) {
        [command() presentDrawable:drawable];
        drawable = nil;
    }
    submit(true);
}
void shutdownUI() {
    submit(true);
    if (uiReady)
        ImGui_ImplMetal_Shutdown();
    uiReady = false;
}
} // namespace metal

extern "C" void *objc_autoreleasePoolPush();
extern "C" void objc_autoreleasePoolPop(void *);
namespace metal {
AutoreleaseScope::AutoreleaseScope() : pool(objc_autoreleasePoolPush()) {}
AutoreleaseScope::~AutoreleaseScope() { objc_autoreleasePoolPop(pool); }
} // namespace metal

const unsigned char *glGetString(GLenum) {
    return reinterpret_cast<const unsigned char *>(device.name.UTF8String);
}
void glUniform1f(GLint i, float v) {
    std::array<uint32_t, 4> bits{};
    memcpy(bits.data(), &v, sizeof(v));
    uniform(i, bits);
}
