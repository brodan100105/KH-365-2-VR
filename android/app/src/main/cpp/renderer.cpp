#include "renderer.h"

#include <algorithm>
#include <cstring>

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

const char *kWorldVs = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uMvp;
out vec2 vUv;
out vec4 vColor;
void main() {
    vUv = aUv;
    vColor = aColor;
    gl_Position = uMvp * vec4(aPos, 1.0);
})";

// The DS's own blending of texture and vertex colour: multiplied (modulate), or the texture over
// the colour by its alpha (decal). Fully transparent texels are not drawn.
const char *kWorldFs = R"(#version 300 es
precision mediump float;
in vec2 vUv;
in vec4 vColor;
uniform sampler2D uTex;
uniform int uUseTex;
uniform int uDecal;
// The DS's fades of its 3D layer: (target, amount) twice; target 1 white, 0 black.
uniform vec4 uFade;
uniform float uCut;
out vec4 oColor;
void main() {
    vec4 c = vColor;
    if (uUseTex == 1) {
        vec4 t = texture(uTex, vUv);
        if (uDecal == 1) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);
        else c *= t;
    }
    if (c.a < uCut) discard;
    c.rgb = mix(c.rgb, vec3(uFade.x), uFade.y);
    c.rgb = mix(c.rgb, vec3(uFade.z), uFade.w);
    oColor = c;
})";

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
uniform float uSmooth;   // DS screens: edge-preserving smoothing, 0 = off
uniform vec2 uDsPixel;   // one DS pixel in texture coordinates
out vec4 oColor;
// Fully transparent texels (the HUD's empty areas) write no depth, so they hide nothing behind.
void main() {
    vec4 c = texture(uTex, vUv);
    if (uSmooth > 0.0) {
        // A 3x3 bilateral filter at DS-pixel spacing: each neighbour counts by how close its
        // colour is to this one, so gradients, dithering and jagged edges blend while
        // high-contrast edges (text, outlines) stay sharp. uSmooth widens what counts as close.
        float sigma = mix(0.03, 0.22, uSmooth);
        vec4 sum = c;
        float weight = 1.0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                vec4 n = texture(uTex, vUv + vec2(float(dx), float(dy)) * uDsPixel * 0.75);
                vec3 d = n.rgb - c.rgb;
                float w = exp(-dot(d, d) / (2.0 * sigma * sigma)) * (abs(n.a - c.a) < 0.5 ? 1.0 : 0.0);
                sum += n * w;
                weight += w;
            }
        c = mix(c, sum / weight, min(1.0, uSmooth * 1.5));
    }
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

void Texture::Upload(int w, int h, const uint32_t *rgba, bool linear, bool mipmaps) {
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
    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        // Anisotropic filtering keeps a floor sharp toward the horizon, where the GPU has it.
        static const bool aniso = [] {
            const char *ext = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
            return ext && std::strstr(ext, "GL_EXT_texture_filter_anisotropic");
        }();
        if (aniso) glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY_EXT */, 8.0f);
    }
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
    worldProgram_ = Link(kWorldVs, kWorldFs);
    if (!litProgram_ || !texProgram_ || !lineProgram_ || !overlayProgram_ || !worldProgram_) return false;
    worldMvp_ = glGetUniformLocation(worldProgram_, "uMvp");
    worldUseTex_ = glGetUniformLocation(worldProgram_, "uUseTex");
    worldDecal_ = glGetUniformLocation(worldProgram_, "uDecal");
    worldSampler_ = glGetUniformLocation(worldProgram_, "uTex");
    worldFade_ = glGetUniformLocation(worldProgram_, "uFade");
    worldCut_ = glGetUniformLocation(worldProgram_, "uCut");
    glGenBuffers(1, &worldVbo_);
    litMvp_ = glGetUniformLocation(litProgram_, "uMvp");
    litAlpha_ = glGetUniformLocation(litProgram_, "uAlpha");
    litLight_ = glGetUniformLocation(litProgram_, "uLight");
    texMvp_ = glGetUniformLocation(texProgram_, "uMvp");
    texAlpha_ = glGetUniformLocation(texProgram_, "uAlpha");
    texSampler_ = glGetUniformLocation(texProgram_, "uTex");
    texSmooth_ = glGetUniformLocation(texProgram_, "uSmooth");
    texDsPixel_ = glGetUniformLocation(texProgram_, "uDsPixel");
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

void Renderer::DrawTexturedWorld(const TexturedWorld &world, const Mat4 &viewProj, bool nearest) {
    if (world.vertices.empty()) return;
    ++worldFrames_;
    if (world.revision != worldRevision_) {
        worldRevision_ = world.revision;
        // Regroup the triangles in three passes, as the DS draws them:
        //   1. opaque triangles, by texture;
        //   2. the solid texels of triangles translucent only through their texture (hair,
        //      leaves): drawn as opaque, so they hide what lies behind them;
        //   3. translucent triangles in the game's order (it sorts them), blended, setting depth
        //      only when the polygon asks; the soft texels of pass 2's triangles among them.
        const size_t triangles = world.vertices.size() / 3;
        std::vector<TexturedVertex> sorted;
        sorted.reserve(world.vertices.size() * 2);
        worldBatches_.clear();
        auto cullOf = [&](size_t t) {
            const uint8_t f = world.triangleFlags[t];
            const bool front = f & kTriFront, back = f & kTriBack;
            return front && !back ? 1 : back && !front ? 2 : 0;
        };
        auto add = [&](size_t t, bool blend, bool depthWrite, float cut) {
            const int32_t tex = world.triangleTexture[t];
            const bool decal = world.triangleFlags[t] & kTriDecal;
            const bool lines = world.triangleFlags[t] & kTriLine;
            const int cull = lines ? 0 : cullOf(t);
            WorldBatch *b = worldBatches_.empty() ? nullptr : &worldBatches_.back();
            if (!b || b->texture != tex || b->blend != blend || b->depthWrite != depthWrite || b->decal != decal ||
                b->cut != cut || b->cull != cull || b->lines != lines)
                worldBatches_.push_back(
                    {tex, blend, depthWrite, decal, cut, cull, lines, static_cast<GLint>(sorted.size()), 0});
            const int n = lines ? 2 : 3;  // an edge is its first two vertices
            for (int k = 0; k < n; ++k) sorted.push_back(world.vertices[t * 3 + k]);
            worldBatches_.back().count += n;
        };
        std::vector<size_t> pass1, pass2, pass3;
        for (size_t t = 0; t < triangles; ++t) {
            const uint8_t f = world.triangleFlags[t];
            if (!(f & kTriTranslucent)) {
                pass1.push_back(t);
            } else {
                if (f & kTriTexAlpha) pass2.push_back(t);
                pass3.push_back(t);
            }
        }
        auto byKey = [&](size_t a, size_t b) {
            const bool la = world.triangleFlags[a] & kTriLine, lb = world.triangleFlags[b] & kTriLine;
            if (la != lb) return lb;  // edges after the opaque triangles, in batches of their own
            if (world.triangleTexture[a] != world.triangleTexture[b]) return world.triangleTexture[a] < world.triangleTexture[b];
            return cullOf(a) < cullOf(b);
        };
        std::stable_sort(pass1.begin(), pass1.end(), byKey);
        std::stable_sort(pass2.begin(), pass2.end(), byKey);
        for (size_t t : pass1) add(t, false, true, 0.02f);
        for (size_t t : pass2) add(t, false, true, 0.99f);
        for (size_t t : pass3) add(t, true, (world.triangleFlags[t] & kTriDepthWrite) != 0, 0.02f);
        glBindBuffer(GL_ARRAY_BUFFER, worldVbo_);
        glBufferData(GL_ARRAY_BUFFER, sorted.size() * sizeof(TexturedVertex), sorted.data(), GL_STREAM_DRAW);
        // Textures: reuse what the GPU has by key, upload the new ones.
        worldTextureIds_.assign(world.textures.size(), 0);
        for (size_t i = 0; i < world.textures.size(); ++i) {
            const WorldTexture &t = world.textures[i];
            GpuTexture &g = worldTextures_[t.key];
            if (!g.id) {
                glGenTextures(1, &g.id);
                glBindTexture(GL_TEXTURE_2D, g.id);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                auto wrap = [](bool repeat, bool flip) {
                    return repeat ? (flip ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE;
                };
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(t.repeatS, t.flipS));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(t.repeatT, t.flipT));
            }
            g.lastUsed = worldFrames_;
            worldTextureIds_[i] = g.id;
        }
        // Textures not used for a while (about ten seconds at 72 Hz, both eyes) go.
        for (auto it = worldTextures_.begin(); it != worldTextures_.end();) {
            if (worldFrames_ - it->second.lastUsed > 1500) {
                glDeleteTextures(1, &it->second.id);
                it = worldTextures_.erase(it);
            } else {
                ++it;
            }
        }
    }
    glUseProgram(worldProgram_);
    glUniformMatrix4fv(worldMvp_, 1, GL_FALSE, viewProj.m);
    glUniform1i(worldSampler_, 0);
    auto fade = [&](int i, float *target, float *amount) {
        *target = world.fadeMode[i] == 1 ? 1.0f : 0.0f;
        *amount = world.fadeMode[i] ? world.fadeFactor[i] / 16.0f : 0.0f;
    };
    float f[4];
    fade(0, &f[0], &f[1]);
    fade(1, &f[2], &f[3]);
    glUniform4f(worldFade_, f[0], f[1], f[2], f[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, worldVbo_);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TexturedVertex),
                          reinterpret_cast<void *>(offsetof(TexturedVertex, position)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TexturedVertex),
                          reinterpret_cast<void *>(offsetof(TexturedVertex, u)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(TexturedVertex),
                          reinterpret_cast<void *>(offsetof(TexturedVertex, color)));
    glEnable(GL_DEPTH_TEST);
    // Equal depths pass: pass 3 draws pass 2's triangles again over themselves.
    glDepthFunc(GL_LEQUAL);
    glFrontFace(GL_CCW);  // the DS's front faces are counterclockwise too
    for (const WorldBatch &b : worldBatches_) {
        SetBlend(b.blend);
        glDepthMask(b.depthWrite ? GL_TRUE : GL_FALSE);
        glUniform1f(worldCut_, b.cut);
        if (b.cull == 0) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(b.cull == 1 ? GL_BACK : GL_FRONT);
        }
        const bool textured = b.texture >= 0 && static_cast<size_t>(b.texture) < worldTextureIds_.size();
        glUniform1i(worldUseTex_, textured ? 1 : 0);
        glUniform1i(worldDecal_, b.decal ? 1 : 0);
        if (textured) {
            glBindTexture(GL_TEXTURE_2D, worldTextureIds_[b.texture]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
        }
        // A wireframe edge is one DS pixel wide on its 256 x 192 screen: a few headset pixels.
        if (b.lines) glLineWidth(3.0f);
        glDrawArrays(b.lines ? GL_LINES : GL_TRIANGLES, b.first, b.count);
        if (b.lines) glLineWidth(1.0f);
    }
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    SetBlend(false);
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
    glUniform1f(texSmooth_, smoothing_);
    glUniform2f(texDsPixel_, 1.0f / 256.0f, 1.0f / 192.0f);
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
