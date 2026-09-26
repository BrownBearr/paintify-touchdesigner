// gpu.h over OpenGL 4.6. Each entry point is the GL call sequence the
// pipeline used to issue inline, so this backend renders exactly what the
// renderer rendered before the Vulkan backend existed.
#include "gpu.h"
#include "gpu_gl.h"
#include "shader_source.h"

#include <glad/glad.h>

#include <cstdio>
#include <cstring>

namespace gpu {

struct TextureObj {
    GLuint id = 0;
    int w = 0, h = 0, layers = 1, levels = 1;
    Format fmt = Format::RGBA8;
};

struct BufferObj {
    GLuint id = 0;
    size_t size = 0;
};

struct FramebufferObj {
    GLuint id = 0;
    int w = 0, h = 0;
    int count = 0;
};

struct Program::Impl {
    GLuint id = 0;
    ~Impl() { if (id) glDeleteProgram(id); }
};

namespace {

GLuint g_vao = 0;
GLuint g_paramsUbo = 0;
size_t g_paramsSize = 0;

GLenum internalFormat(Format f) {
    switch (f) {
        case Format::RGBA16F: return GL_RGBA16F;
        case Format::R16F:    return GL_R16F;
        case Format::RG16F:   return GL_RG16F;
        default:              return GL_RGBA8;
    }
}

GLenum wrapMode(Wrap w) { return w == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE; }

void APIENTRY debugCb(GLenum, GLenum type, GLuint, GLenum severity,
                      GLsizei, const GLchar* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    fprintf(stderr, "[GL%s] %s\n", type == GL_DEBUG_TYPE_ERROR ? " ERROR" : "", msg);
}

GLuint compile(GLenum type, const std::string& src, std::string* err) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 1), ' ');
        glGetShaderInfoLog(s, len, nullptr, &log[0]);
        if (err) *err += log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLint location(const Program& p, const char* name) {
    return p.impl ? glGetUniformLocation(p.impl->id, name) : -1;
}

} // namespace

namespace gl {
unsigned textureId(Texture t) { return t ? t->id : 0; }
} // namespace gl

bool init(const InitOptions& opt, std::string* err) {
    (void)opt;
    if (!glDispatchCompute) {
        if (err) *err = "OpenGL 4.3+ is not available (glad was not loaded?)";
        return false;
    }
    if (glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(debugCb, nullptr);
    }
    glGenVertexArrays(1, &g_vao);   // core profile needs one bound, even empty
    return true;
}

void shutdown() {
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_paramsUbo) glDeleteBuffers(1, &g_paramsUbo);
    g_vao = 0;
    g_paramsUbo = 0;
    g_paramsSize = 0;
}

const char* backendName() { return "OpenGL"; }

std::string describe() {
    const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    return std::string("GL ") + (v ? v : "?") + " | " + (r ? r : "?");
}

// --- resources ---------------------------------------------------------------

Texture createTexture2D(int w, int h, Format f, bool mips) {
    Texture t = new TextureObj;
    t->w = w; t->h = h; t->fmt = f;
    glCreateTextures(GL_TEXTURE_2D, 1, &t->id);
    int levels = 1;
    if (mips) {
        int m = (w > h ? w : h);
        while (m > 1) { m >>= 1; ++levels; }
    }
    t->levels = levels;
    glTextureStorage2D(t->id, levels, internalFormat(f), w, h);
    glTextureParameteri(t->id, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(t->id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(t->id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(t->id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

Texture createTextureArray(int w, int h, int layers, Format f, const float* data,
                           Wrap u, Wrap v) {
    Texture t = new TextureObj;
    t->w = w; t->h = h; t->layers = layers; t->fmt = f;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &t->id);
    glTextureStorage3D(t->id, 1, internalFormat(f), w, h, layers);
    const GLenum fmt = (f == Format::R16F) ? GL_RED : (f == Format::RG16F) ? GL_RG : GL_RGBA;
    glTextureSubImage3D(t->id, 0, 0, 0, 0, w, h, layers, fmt, GL_FLOAT, data);
    glTextureParameteri(t->id, GL_TEXTURE_WRAP_S, wrapMode(u));
    glTextureParameteri(t->id, GL_TEXTURE_WRAP_T, wrapMode(v));
    glTextureParameteri(t->id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(t->id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

void destroy(Texture& t) {
    if (!t) return;
    glDeleteTextures(1, &t->id);
    delete t;
    t = nullptr;
}

int width(Texture t) { return t ? t->w : 0; }
int height(Texture t) { return t ? t->h : 0; }
Format format(Texture t) { return t ? t->fmt : Format::RGBA8; }

Buffer createBuffer(size_t bytes, BufferUse use) {
    Buffer b = new BufferObj;
    b->size = bytes;
    const GLenum usage = use == BufferUse::Readback ? GL_DYNAMIC_READ
                       : use == BufferUse::Uniform  ? GL_DYNAMIC_DRAW
                                                    : GL_DYNAMIC_COPY;
    glCreateBuffers(1, &b->id);
    glNamedBufferData(b->id, GLsizeiptr(bytes), nullptr, usage);
    return b;
}

void destroy(Buffer& b) {
    if (!b) return;
    glDeleteBuffers(1, &b->id);
    delete b;
    b = nullptr;
}

Framebuffer createFramebuffer(std::initializer_list<Texture> colors) {
    Framebuffer fb = new FramebufferObj;
    glCreateFramebuffers(1, &fb->id);
    GLenum targets[8];
    int n = 0;
    for (Texture t : colors) {
        if (n >= 8) break;
        glNamedFramebufferTexture(fb->id, GL_COLOR_ATTACHMENT0 + n, t->id, 0);
        targets[n] = GL_COLOR_ATTACHMENT0 + n;
        fb->w = t->w; fb->h = t->h;
        ++n;
    }
    fb->count = n;
    glNamedFramebufferDrawBuffers(fb->id, n, targets);
    return fb;
}

void destroy(Framebuffer& fb) {
    if (!fb) return;
    glDeleteFramebuffers(1, &fb->id);
    delete fb;
    fb = nullptr;
}

// --- transfers -----------------------------------------------------------------

void uploadTexture(Texture t, const void* rgba8) {
    glTextureSubImage2D(t->id, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
}

void copyTexture(Texture src, Texture dst) {
    glCopyImageSubData(src->id, GL_TEXTURE_2D, 0, 0, 0, 0,
                       dst->id, GL_TEXTURE_2D, 0, 0, 0, 0, src->w, src->h, 1);
}

void clearTexture(Texture t, float r, float g, float b, float a) {
    const float v[4] = {r, g, b, a};
    glClearTexImage(t->id, 0, GL_RGBA, GL_FLOAT, v);
}

void generateMipmaps(Texture t) { glGenerateTextureMipmap(t->id); }

void clearBuffer(Buffer b, size_t offset, size_t size, uint32_t value) {
    if (offset == 0 && (size == 0 || size == b->size)) {
        glClearNamedBufferData(b->id, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
        return;
    }
    if (size == 0) size = b->size - offset;
    glClearNamedBufferSubData(b->id, GL_R32UI, GLintptr(offset), GLsizeiptr(size),
                              GL_RED_INTEGER, GL_UNSIGNED_INT, &value);
}

void copyBuffer(Buffer src, size_t srcOffset, Buffer dst, size_t dstOffset, size_t size) {
    glCopyNamedBufferSubData(src->id, dst->id, GLintptr(srcOffset), GLintptr(dstOffset),
                             GLsizeiptr(size));
}

void readBuffer(Buffer b, size_t offset, size_t size, void* out) {
    glGetNamedBufferSubData(b->id, GLintptr(offset), GLsizeiptr(size), out);
}

void readTexture(Texture t, int level, ReadAs as, void* out) {
    int w = t->w, h = t->h;
    for (int i = 0; i < level; ++i) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    GLenum fmt = GL_RGBA, type = GL_FLOAT;
    size_t bpp = 16;
    switch (as) {
        case ReadAs::RGBA8:   fmt = GL_RGBA; type = GL_UNSIGNED_BYTE; bpp = 4; break;
        case ReadAs::RGBA32F: fmt = GL_RGBA; bpp = 16; break;
        case ReadAs::R32F:    fmt = GL_RED;  bpp = 4;  break;
        case ReadAs::RG32F:   fmt = GL_RG;   bpp = 8;  break;
    }
    glGetTextureImage(t->id, level, fmt, type, GLsizei(size_t(w) * size_t(h) * bpp), out);
}

// --- programs --------------------------------------------------------------------

bool Program::build() {
    lastError.clear();
    std::vector<GLuint> stages;
    for (size_t i = 0; i < paths.size(); ++i) {
        GLenum type = GL_COMPUTE_SHADER;
        if (paths.size() == 2) type = (i == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
        std::string src = shadersrc::load(paths[i], &lastError);
        if (src.empty()) {
            lastError = paths[i] + ": " + lastError;
            for (size_t k = 0; k < stages.size(); ++k) glDeleteShader(stages[k]);
            return false;
        }
        GLuint s = compile(type, src, &lastError);
        if (!s) {
            lastError = paths[i] + ":\n" + lastError;
            for (size_t k = 0; k < stages.size(); ++k) glDeleteShader(stages[k]);
            return false;
        }
        stages.push_back(s);
    }

    GLuint prog = glCreateProgram();
    for (size_t i = 0; i < stages.size(); ++i) glAttachShader(prog, stages[i]);
    glLinkProgram(prog);
    for (size_t i = 0; i < stages.size(); ++i) {
        glDetachShader(prog, stages[i]);
        glDeleteShader(stages[i]);
    }

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 1), ' ');
        glGetProgramInfoLog(prog, len, nullptr, &log[0]);
        lastError = paths[0] + " link:\n" + log;
        glDeleteProgram(prog);
        return false;
    }

    impl = std::make_shared<Impl>();
    impl->id = prog;
    mtimes = shadersrc::mtimes(paths);
    return true;
}

void Program::setInt(const char* name, int v) {
    if (impl) glProgramUniform1i(impl->id, location(*this, name), v);
}
void Program::setUInt(const char* name, uint32_t v) {
    if (impl) glProgramUniform1ui(impl->id, location(*this, name), v);
}
void Program::setIVec2(const char* name, int x, int y) {
    if (impl) glProgramUniform2i(impl->id, location(*this, name), x, y);
}
void Program::setVec3(const char* name, float x, float y, float z) {
    if (impl) glProgramUniform3f(impl->id, location(*this, name), x, y, z);
}

// --- binding and execution -----------------------------------------------------

void bindTexture(unsigned unit, Texture t) { glBindTextureUnit(unit, t ? t->id : 0); }

void bindImage(unsigned unit, Texture t, Access a) {
    const GLenum access = a == Access::Read ? GL_READ_ONLY
                        : a == Access::Write ? GL_WRITE_ONLY : GL_READ_WRITE;
    glBindImageTexture(unit, t ? t->id : 0, 0, GL_FALSE, 0, access,
                       internalFormat(t ? t->fmt : Format::RGBA8));
}

void bindStorage(unsigned binding, Buffer b) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, b ? b->id : 0);
}

void setParams(const void* data, size_t bytes) {
    if (!g_paramsUbo || bytes > g_paramsSize) {
        if (g_paramsUbo) glDeleteBuffers(1, &g_paramsUbo);
        glCreateBuffers(1, &g_paramsUbo);
        glNamedBufferData(g_paramsUbo, GLsizeiptr(bytes), nullptr, GL_DYNAMIC_DRAW);
        g_paramsSize = bytes;
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, g_paramsUbo);
    }
    glNamedBufferSubData(g_paramsUbo, 0, GLsizeiptr(bytes), data);
}

void dispatch(Program& p, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!p.impl) return;
    glUseProgram(p.impl->id);
    glDispatchCompute(gx, gy, gz);
}

void drawIndirect(Program& p, Framebuffer fb, std::initializer_list<Blend> blend,
                  Buffer indirect) {
    if (!p.impl || !fb) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fb->id);
    glViewport(0, 0, fb->w, fb->h);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    GLuint i = 0;
    for (Blend b : blend) {
        if (b == Blend::Additive)               glBlendFunci(i, GL_ONE, GL_ONE);
        else if (b == Blend::PremultipliedOver) glBlendFunci(i, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        else                                    glBlendFunci(i, GL_ONE, GL_ZERO);
        ++i;
    }

    glUseProgram(p.impl->id);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirect->id);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void memoryBarrier(unsigned bits) {
    GLbitfield gl = 0;
    if (bits & BarrierTextureFetch)  gl |= GL_TEXTURE_FETCH_BARRIER_BIT;
    if (bits & BarrierImageAccess)   gl |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    if (bits & BarrierStorage)       gl |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (bits & BarrierCommand)       gl |= GL_COMMAND_BARRIER_BIT;
    if (bits & BarrierBufferUpdate)  gl |= GL_BUFFER_UPDATE_BARRIER_BIT;
    if (bits & BarrierTextureUpdate) gl |= GL_TEXTURE_UPDATE_BARRIER_BIT;
    if (bits & BarrierFramebuffer)   gl |= GL_FRAMEBUFFER_BARRIER_BIT;
    if (gl) glMemoryBarrier(gl);
}

void textureBarrier() { glTextureBarrier(); }

void finish() { glFinish(); }

// --- timers ----------------------------------------------------------------------

void Timer::begin() {
    if (!q[0]) glGenQueries(2, q);
    glBeginQuery(GL_TIME_ELAPSED, q[cur]);
    started = true;
}

void Timer::end() {
    if (!started) return;
    glEndQuery(GL_TIME_ELAPSED);
    started = false;
    // Read the *other* query: it is a frame old and therefore already resolved,
    // so the timings never cost a pipeline stall.
    GLuint other = q[1 - cur];
    if (!issued[1 - cur]) { cur = 1 - cur; issued[cur] = true; return; }
    GLint avail = 0;
    glGetQueryObjectiv(other, GL_QUERY_RESULT_AVAILABLE, &avail);
    if (avail) {
        GLuint64 ns = 0;
        glGetQueryObjectui64v(other, GL_QUERY_RESULT, &ns);
        lastMs = static_cast<double>(ns) * 1e-6;
    }
    issued[cur] = true;
    cur = 1 - cur;
}

} // namespace gpu
