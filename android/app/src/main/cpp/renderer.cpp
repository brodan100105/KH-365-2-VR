#include "renderer.h"

#include <cmath>
#include <cstddef>

#include "log.h"

namespace khvr {

namespace {

const char *kLitVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
uniform mat4 uMvp;
uniform vec3 uLight;
out vec4 vColor;
void main() {
    // Half-Lambert keeps the DS's flat vertex colors readable from every side.
    float l = 0.55 + 0.45 * dot(normalize(aNormal), uLight);
    vColor = vec4(aColor.rgb * l, aColor.a);
    gl_Position = uMvp * vec4(aPos, 1.0);
})";

const char *kLitFs = R"(#version 300 es
precision mediump float;
in vec4 vColor;
uniform float uAlpha;
out vec4 oColor;
void main() { oColor = vec4(vColor.rgb, vColor.a * uAlpha); })";

const char *kTexVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
uniform mat4 uMvp;
out vec2 vUv;
void main() { vUv = aUv; gl_Position = uMvp * vec4(aPos, 1.0); })";

const char *kTexFs = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uTex;
uniform float uAlpha;
out vec4 oColor;
// Fully transparent texels (the HUD's empty areas) write no depth, so they hide nothing behind.
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a * uAlpha < 0.02) discard;
    oColor = vec4(c.rgb, c.a * uAlpha);
})";

const char *kLineVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uMvp;
void main() { gl_Position = uMvp * vec4(aPos, 1.0); })";

const char *kLineFs = R"(#version 300 es
precision mediump float;
uniform vec4 uColor;
out vec4 oColor;
void main() { oColor = uColor; })";

const char *kOverlayVs = R"(#version 300 es
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 0.0, 1.0);
})";

const char *kOverlayFs = R"(#version 300 es
precision mediump float;
in vec2 vNdc;
uniform float uVignette;
uniform float uBlink;
out vec4 oColor;
void main() {
    float r = length(vNdc);
    float v = uVignette * smoothstep(0.45, 1.05, r);
    oColor = vec4(0.0, 0.0, 0.0, clamp(max(v, uBlink), 0.0, 1.0));
})";

GLuint Compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader: %s", log);
    }
    return s;
}

GLuint Link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = Compile(GL_VERTEX_SHADER, vs), f = Compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOGE("program: %s", log);
        return 0;
    }
    return p;
}

void SetBlend(bool on) {
    if (on) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
}

}  // namespace

void Texture::Upload(int w, int h, const uint32_t *rgba, bool linear) {
    if (!id_) glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    if (w != w_ || h != h_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        w_ = w;
        h_ = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    linear_ = linear;
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void RenderTarget::Ensure(int w, int h) {
    if (w == w_ && h == h_ && fbo_) return;
    if (!fbo_) {
        glGenFramebuffers(1, &fbo_);
        glGenTextures(1, &color_);
        glGenRenderbuffers(1, &depth_);
    }
    glBindTexture(GL_TEXTURE_2D, color_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_);
    w_ = w;
    h_ = h;
}

void RenderTarget::Bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w_, h_);
}

bool Renderer::Init() {
    litProgram_ = Link(kLitVs, kLitFs);
    texProgram_ = Link(kTexVs, kTexFs);
    lineProgram_ = Link(kLineVs, kLineFs);
    overlayProgram_ = Link(kOverlayVs, kOverlayFs);
    if (!litProgram_ || !texProgram_ || !lineProgram_ || !overlayProgram_) return false;
    litMvp_ = glGetUniformLocation(litProgram_, "uMvp");
    litAlpha_ = glGetUniformLocation(litProgram_, "uAlpha");
    litLight_ = glGetUniformLocation(litProgram_, "uLight");
    texMvp_ = glGetUniformLocation(texProgram_, "uMvp");
    texAlpha_ = glGetUniformLocation(texProgram_, "uAlpha");
    texSampler_ = glGetUniformLocation(texProgram_, "uTex");
    lineMvp_ = glGetUniformLocation(lineProgram_, "uMvp");
    lineColor_ = glGetUniformLocation(lineProgram_, "uColor");
    ovVignette_ = glGetUniformLocation(overlayProgram_, "uVignette");
    ovBlink_ = glGetUniformLocation(overlayProgram_, "uBlink");
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    return true;
}

void Renderer::DrawTriangles(const WorldDrawList &list, const Mat4 &viewProj, float alpha) {
    if (list.triangles.empty()) return;
    glUseProgram(litProgram_);
    glUniformMatrix4fv(litMvp_, 1, GL_FALSE, viewProj.m);
    glUniform1f(litAlpha_, alpha);
    Vec3 light = Normalize(Vec3{0.4f, 0.8f, 0.45f});
    glUniform3f(litLight_, light.x, light.y, light.z);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // Orphan and refill: the list changes every game frame.
    glBufferData(GL_ARRAY_BUFFER, list.triangles.size() * sizeof(WorldVertex), list.triangles.data(),
                 GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WorldVertex),
                          reinterpret_cast<void *>(offsetof(WorldVertex, position)));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(WorldVertex),
                          reinterpret_cast<void *>(offsetof(WorldVertex, normal)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(WorldVertex),
                          reinterpret_cast<void *>(offsetof(WorldVertex, color)));
    SetBlend(alpha < 1.0f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(list.triangles.size()));
    glDisable(GL_CULL_FACE);
    glDisableVertexAttribArray(2);
}

void Renderer::DrawQuad(GLuint texture, const Pose &pose, float w, float h, const Mat4 &viewProj,
                        float alpha, float curvature, float distance, bool flipV) {
    // Curved screens are split into vertical strips along an arc of radius distance/curvature.
    const int strips = curvature > 0.01f ? 32 : 1;
    const float radius = curvature > 0.01f ? distance / curvature : 0.0f;
    const float arc = radius > 0 ? w / radius : 0.0f;
    scratch_.clear();
    auto vertex = [&](float u, float v) {
        float x, z;
        if (radius > 0) {
            float a = (u - 0.5f) * arc;
            x = radius * std::sin(a);
            z = radius * (1.0f - std::cos(a));  // edges come toward the viewer (+Z)
        } else {
            x = (u - 0.5f) * w;
            z = 0.0f;
        }
        Vec3 p = pose.Transform({x, (0.5f - v) * h, z});
        scratch_.insert(scratch_.end(), {p.x, p.y, p.z, u, flipV ? 1.0f - v : v});
    };
    for (int i = 0; i < strips; ++i) {
        float u0 = static_cast<float>(i) / strips, u1 = static_cast<float>(i + 1) / strips;
        vertex(u0, 0); vertex(u0, 1); vertex(u1, 1);
        vertex(u0, 0); vertex(u1, 1); vertex(u1, 0);
    }
    glUseProgram(texProgram_);
    glUniformMatrix4fv(texMvp_, 1, GL_FALSE, viewProj.m);
    glUniform1f(texAlpha_, alpha);
    glUniform1i(texSampler_, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, scratch_.size() * sizeof(float), scratch_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          reinterpret_cast<void *>(3 * sizeof(float)));
    SetBlend(true);
    glEnable(GL_DEPTH_TEST);
    glDrawArrays(GL_TRIANGLES, 0, strips * 6);
}

void Renderer::DrawLine(const Vec3 &a, const Vec3 &b, uint32_t color, const Mat4 &viewProj) {
    float v[6] = {a.x, a.y, a.z, b.x, b.y, b.z};
    glUseProgram(lineProgram_);
    glUniformMatrix4fv(lineMvp_, 1, GL_FALSE, viewProj.m);
    glUniform4f(lineColor_, (color & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f,
                ((color >> 16) & 0xff) / 255.0f, (color >> 24) / 255.0f);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    SetBlend(true);
    glLineWidth(3.0f);
    glDrawArrays(GL_LINES, 0, 2);
}

void Renderer::DrawComfortOverlay(float vignette, float blink) {
    if (vignette <= 0.001f && blink <= 0.001f) return;
    glUseProgram(overlayProgram_);
    glUniform1f(ovVignette_, vignette);
    glUniform1f(ovBlink_, blink);
    glBindVertexArray(vao_);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisable(GL_DEPTH_TEST);
    SetBlend(true);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
}

}  // namespace khvr
