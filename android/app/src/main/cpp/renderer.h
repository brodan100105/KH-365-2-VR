// OpenGL ES 3 drawing for the VR app: lit vertex-colored triangles (the game world, hands,
// Keyblade), textured quads and curved screens (giant screen, HUD, bottom screen, settings
// panel), lines (laser pointer), and a full-screen comfort overlay (vignette and blink).
#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <vector>

#include "khvr/game_source.h"
#include "khvr/math.h"

namespace khvr {

class Texture {
public:
    void Upload(int w, int h, const uint32_t *rgba, bool linear);
    GLuint id() const { return id_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    GLuint id_ = 0;
    int w_ = 0, h_ = 0;
    bool linear_ = true;
};

// An offscreen color+depth target (the giant screen's 3D view).
class RenderTarget {
public:
    void Ensure(int w, int h);
    void Bind() const;
    GLuint texture() const { return color_; }
    int width() const { return w_; }
    int height() const { return h_; }

private:
    GLuint fbo_ = 0, color_ = 0, depth_ = 0;
    int w_ = 0, h_ = 0;
};

class Renderer {
public:
    bool Init();

    // Lit triangles. `viewProj` maps the list's space to clip space; the directional light is
    // given in the list's own space.
    void DrawTriangles(const WorldDrawList &list, const Mat4 &viewProj, float alpha = 1.0f);
    // A textured quad of size w x h centered on `pose` (in the space `viewProj` expects),
    // facing +Z of the pose. Curvature bends it around a vertical axis toward the viewer.
    // `flipV` is for textures rendered by GL (bottom row first) rather than uploaded images.
    void DrawQuad(GLuint texture, const Pose &pose, float w, float h, const Mat4 &viewProj,
                  float alpha = 1.0f, float curvature = 0.0f, float distance = 1.0f,
                  bool flipV = false);
    void DrawLine(const Vec3 &a, const Vec3 &b, uint32_t color, const Mat4 &viewProj);
    void DrawComfortOverlay(float vignette, float blink);

private:
    GLuint litProgram_ = 0, texProgram_ = 0, lineProgram_ = 0, overlayProgram_ = 0;
    GLint litMvp_ = -1, litAlpha_ = -1, litLight_ = -1;
    GLint texMvp_ = -1, texAlpha_ = -1, texSampler_ = -1;
    GLint lineMvp_ = -1, lineColor_ = -1;
    GLint ovVignette_ = -1, ovBlink_ = -1;
    GLuint vao_ = 0, vbo_ = 0;
    std::vector<float> scratch_;
};

}  // namespace khvr
