// OpenGL ES 3 drawing for the VR app: lit vertex-colored triangles (the game world, hands,
// Keyblade), textured quads and curved screens (giant screen, HUD, bottom screen, settings
// panel), lines (laser pointer), and a full-screen comfort overlay (vignette and blink).
#pragma once

#include <GLES3/gl3.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "khvr/game_source.h"
#include "khvr/math.h"

namespace khvr {

class Texture {
public:
    // `mipmaps`: for a texture seen at a slant or from afar (a floor), filtered without shimmer.
    void Upload(int w, int h, const uint32_t *rgba, bool linear, bool mipmaps = false);
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

// Standing in the world, DrawTexturedWorld can show in-between frames (each vertex glides from
// where the previous game frame had it over the time between game frames, when the frames match
// triangle for triangle) and turn sprites to face the viewer (right / up: the viewer's, in world
// space).
struct WorldMotion {
    bool inBetween = false;
    bool sprites = false;
    Vec3 right{1, 0, 0}, up{0, 1, 0};
    // Where the viewer is (world space): translucent triangles are drawn back to front from here
    // when the game leaves their order to the DS.
    bool hasEye = false;
    Vec3 eye;
};

class Renderer {
public:
    bool Init();

    // Lit triangles. `viewProj` maps the list's space to clip space; the directional light is
    // given in the list's own space.
    // `onTop`: drawn over everything (no depth test), as a lock-on sight is.
    void DrawTriangles(const WorldDrawList &list, const Mat4 &viewProj, float alpha = 1.0f, bool onTop = false);
    // A textured quad of size w x h centered on `pose` (in the space `viewProj` expects),
    // facing +Z of the pose. Curvature bends it around a vertical axis toward the viewer.
    // `flipV` is for textures rendered by GL (bottom row first) rather than uploaded images.
    void DrawQuad(GLuint texture, const Pose &pose, float w, float h, const Mat4 &viewProj,
                  float alpha = 1.0f, float curvature = 0.0f, float distance = 1.0f,
                  bool flipV = false, bool onTop = false);
    // Edge-preserving smoothing (0..1) for the following DrawQuads, meant for DS screens only;
    // set it back to 0 for anything else.
    void SetScreenSmoothing(float amount) { smoothing_ = amount; }
    void DrawLine(const Vec3 &a, const Vec3 &b, uint32_t color, const Mat4 &viewProj);
    // The comfort vignette, `vignette` (0..1) of the way in, `strength` dark at the rim and `size`
    // of the view at its fullest, and the blink (0..1 black). `eye` is this eye's view and `both`
    // both eyes' together, as tangents of their edges (left, right, down, up): the vignette is one
    // circle round both.
    void DrawComfortOverlay(float vignette, float strength, float size, float blink,
                            const float eye[4], const float both[4]);
    // The game's textured world (already lit by the DS): opaque triangles, then translucent ones
    // over them. Its textures are kept on the GPU by key and uploaded only when new.
    // `nearest`: texels stay sharp squares when enlarged (else they blend smoothly).
    // `motion`: see WorldMotion.
    // `slot`: each world drawn every frame has its own (0 the game's world, 1 the Keyblade in
    // your hand), so they do not take turns rebuilding.
    static constexpr int kWorldSlots = 2;
    void DrawTexturedWorld(const TexturedWorld &world, const Mat4 &viewProj, bool nearest,
                           const WorldMotion &motion = WorldMotion(), int slot = 0);

private:
    GLuint litProgram_ = 0, texProgram_ = 0, lineProgram_ = 0, overlayProgram_ = 0;
    GLint litMvp_ = -1, litAlpha_ = -1, litLight_ = -1;
    GLint texMvp_ = -1, texAlpha_ = -1, texSampler_ = -1, texSmooth_ = -1, texDsPixel_ = -1;
    float smoothing_ = 0.0f;
    GLint lineMvp_ = -1, lineColor_ = -1;
    GLint ovVignette_ = -1, ovBlink_ = -1, ovStrength_ = -1, ovSize_ = -1, ovEye_ = -1, ovBoth_ = -1;
    GLuint vao_ = 0, vbo_ = 0;
    // DrawTexturedWorld: its vertices, regrouped by texture and blending once per revision.
    GLuint worldProgram_ = 0;
    GLint worldMvp_ = -1, worldUseTex_ = -1, worldDecal_ = -1, worldSampler_ = -1, worldFade_ = -1, worldCut_ = -1;
    GLint worldBlend_ = -1, worldSprites_ = -1, worldRight_ = -1, worldUp_ = -1;
    struct WorldBatch {
        int32_t texture;  // index into the world's textures, -1 none
        bool blend, depthWrite, decal;
        float cut;        // texels below this alpha are not drawn
        int cull;         // 0 both faces, 1 front faces only, 2 back faces only
        bool lines;       // wireframe edges: pairs of vertices (GL_LINES)
        GLint first;
        GLsizei count;
    };
    // What DrawTexturedWorld keeps of one textured world between its revisions (a slot each).
    struct WorldCache {
        GLuint worldVbo = 0, worldMotionVbo = 0;
        // In-between frames: the previous revision's triangles (texture key and flags) and vertex
        // positions and sprite centres, in its order; when the new revision's match, its vertices
        // glide from these.
        std::vector<uint64_t> prevTriangles;
        std::vector<Vec3> prevPositions, prevCenters;
        double revisionTime = 0.0, gameFrameSeconds = 1.0 / 60.0;
        bool motionMatched = false;
        std::vector<WorldBatch> worldBatches;
        uint32_t worldRevision = ~0u;
        std::vector<GLuint> worldTextureIds;  // per texture of the current revision
    };
    WorldCache worldCaches_[kWorldSlots];
    struct GpuTexture {
        GLuint id = 0;
        uint32_t lastUsed = 0;
    };
    std::unordered_map<uint64_t, GpuTexture> worldTextures_;
    uint32_t worldFrames_ = 0;
    std::vector<float> scratch_;
};

}  // namespace khvr
