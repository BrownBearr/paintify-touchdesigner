#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
using GLuint = uint32_t;
using GLenum = uint32_t;
using GLint = int;
using GLsizei = int;
using GLboolean = uint8_t;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;
using GLbitfield = uint32_t;
constexpr GLenum GL_BLEND = 2;
constexpr GLenum GL_BUFFER_UPDATE_BARRIER_BIT = 3;
constexpr GLenum GL_CLAMP_TO_EDGE = 4;
constexpr GLenum GL_COLOR_ATTACHMENT0 = 5;
constexpr GLenum GL_COLOR_ATTACHMENT1 = 6;
constexpr GLenum GL_COLOR_BUFFER_BIT = 7;
constexpr GLenum GL_COMMAND_BARRIER_BIT = 8;
constexpr GLenum GL_COPY_WRITE_BUFFER = 9;
constexpr GLenum GL_DEPTH_TEST = 10;
constexpr GLenum GL_DRAW_INDIRECT_BUFFER = 11;
constexpr GLenum GL_DYNAMIC_COPY = 12;
constexpr GLenum GL_DYNAMIC_DRAW = 13;
constexpr GLenum GL_DYNAMIC_READ = 14;
constexpr GLenum GL_FALSE = 0;
constexpr GLenum GL_FLOAT = 16;
constexpr GLenum GL_FRAMEBUFFER = 17;
constexpr GLenum GL_LINEAR = 18;
constexpr GLenum GL_NEAREST = 19;
constexpr GLenum GL_ONE = 20;
constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 21;
constexpr GLenum GL_R16F = 22;
constexpr GLenum GL_R32UI = 23;
constexpr GLenum GL_READ_WRITE = 24;
constexpr GLenum GL_RED = 25;
constexpr GLenum GL_RED_INTEGER = 26;
constexpr GLenum GL_REPEAT = 27;
constexpr GLenum GL_RG = 28;
constexpr GLenum GL_RG16F = 29;
constexpr GLenum GL_RGBA = 30;
constexpr GLenum GL_RGBA16F = 31;
constexpr GLenum GL_RGBA8 = 32;
constexpr GLenum GL_SCISSOR_BOX = 33;
constexpr GLenum GL_SCISSOR_TEST = 34;
constexpr GLenum GL_SHADER_IMAGE_ACCESS_BARRIER_BIT = 35;
constexpr GLenum GL_SHADER_STORAGE_BARRIER_BIT = 36;
constexpr GLenum GL_SHADER_STORAGE_BUFFER = 37;
constexpr GLenum GL_TEXTURE_2D = 38;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 39;
constexpr GLenum GL_TEXTURE_FETCH_BARRIER_BIT = 40;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 41;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 42;
constexpr GLenum GL_TEXTURE_UPDATE_BARRIER_BIT = 43;
constexpr GLenum GL_TEXTURE_WRAP_S = 44;
constexpr GLenum GL_TEXTURE_WRAP_T = 45;
constexpr GLenum GL_TIME_ELAPSED = 46;
constexpr GLenum GL_TRIANGLE_STRIP = 47;
constexpr GLenum GL_UNIFORM_BUFFER = 48;
constexpr GLenum GL_UNSIGNED_BYTE = 49;
constexpr GLenum GL_UNSIGNED_INT = 50;
constexpr GLenum GL_WRITE_ONLY = 51;
void glUseProgram(GLuint);
GLint glGetUniformLocation(GLuint, const char *);
void glDeleteProgram(GLuint);
void glFinish();
void glCreateTextures(GLenum, int, GLuint *);
void glDeleteTextures(int, const GLuint *);
void glTextureStorage2D(GLuint, int, GLenum, int, int);
void glTextureStorage3D(GLuint, int, GLenum, int, int, int);
void glTextureParameteri(GLuint, GLenum, GLint);
void glTextureSubImage2D(GLuint, int, int, int, int, int, GLenum, GLenum, const void *);
void glTextureSubImage3D(GLuint, int, int, int, int, int, int, int, GLenum, GLenum, const void *);
void glGenerateTextureMipmap(GLuint);
void glGetTextureImage(GLuint, int, GLenum, GLenum, GLsizei, void *);
void glClearTexImage(GLuint, int, GLenum, GLenum, const void *);
void glCopyImageSubData(GLuint, GLenum, int, int, int, int, GLuint, GLenum, int, int, int, int, int,
                        int, int);
void glBindTextureUnit(GLuint, GLuint);
void glBindImageTexture(GLuint, GLuint, int, GLboolean, int, GLenum, GLenum);
void glDeleteBuffers(int, const GLuint *);
void glNamedBufferSubData(GLuint, GLintptr, GLsizeiptr, const void *);
void glGetNamedBufferSubData(GLuint, GLintptr, GLsizeiptr, void *);
void glClearNamedBufferData(GLuint, GLenum, GLenum, GLenum, const void *);
void glClearNamedBufferSubData(GLuint, GLenum, GLintptr, GLsizeiptr, GLenum, GLenum, const void *);
void glCopyNamedBufferSubData(GLuint, GLuint, GLintptr, GLintptr, GLsizeiptr);
void glBindBufferBase(GLenum, GLuint, GLuint);
void glBindBuffer(GLenum, GLuint);
void glCreateFramebuffers(int, GLuint *);
void glDeleteFramebuffers(int, const GLuint *);
void glNamedFramebufferTexture(GLuint, GLenum, GLuint, int);
void glNamedFramebufferDrawBuffers(GLuint, int, const GLenum *);
void glBindFramebuffer(GLenum, GLuint);
void glViewport(int, int, int, int);
void glDrawArraysIndirect(GLenum, const void *);
void glUniform1i(GLint, int);
void glUniform1ui(GLint, GLuint);
void glUniform2i(GLint, int, int);
void glUniform3f(GLint, float, float, float);
void glDispatchCompute(GLuint, GLuint, GLuint);
void glMemoryBarrier(GLbitfield);
void glTextureBarrier();
void glGenVertexArrays(int, GLuint *);
void glDeleteVertexArrays(int, const GLuint *);
void glBindVertexArray(GLuint);
void glBlendFunci(GLuint, GLenum, GLenum);
void glEnable(GLenum);
void glDisable(GLenum);
GLboolean glIsEnabled(GLenum);
void glGetIntegerv(GLenum, GLint *);
void glScissor(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(GLbitfield);
void glBlitNamedFramebuffer(GLuint, GLuint, int, int, int, int, int, int, int, int, GLbitfield,
                            GLenum);

struct GLFWwindow;
namespace metal {
bool init(GLFWwindow *, std::string *);
void newFrame();
void drawUI();
void present();
void shutdownUI();
uint64_t uiTexture(GLuint);
void rebuildFonts();
} // namespace metal

namespace metal {
struct AutoreleaseScope {
    void *pool;
    AutoreleaseScope();
    ~AutoreleaseScope();
    AutoreleaseScope(const AutoreleaseScope &) = delete;
    AutoreleaseScope &operator=(const AutoreleaseScope &) = delete;
};
} // namespace metal

constexpr GLenum GL_FRAMEBUFFER_BARRIER_BIT = 100;
constexpr GLenum GL_RENDERER = 101;
const unsigned char *glGetString(GLenum);
void glUniform1f(GLint, float);
