#pragma once
#include "core.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace shader {

class Library;


class Defines {
public:
    Defines& define(std::string_view name);                             // #define NAME
    Defines& define(std::string_view name, std::string_view value);
    Defines& define(std::string_view name, long long value);
    Defines& define(std::string_view name, double value);
    Defines& include(std::string_view path);                            // #include "path"

    bool        empty()    const { return lines.empty(); }
    std::string preamble() const;   // the block injected after #version

private:
    std::vector<std::string> lines;
};

// UBO blocks a pass declares. Bound automatically on every (re)link, so a hot
// reload can never leave a program with an unbound block — which is silent.
enum PassUBO : uint32_t {
    UBO_NONE   = 0u,
    UBO_CAMERA = 1u << 0   // CameraUniform -> binding 0
};

// -----------------------------------------------------------------------------
// Dispatch shape for a compute pass. Direct sizes the grid from a global extent;
// Indirect reads the group counts from a buffer the GPU filled.
// -----------------------------------------------------------------------------
struct Dispatch {
    static Dispatch direct(glm::ivec2 globalSize, glm::ivec2 groupSize = glm::ivec2(8, 8));
    static Dispatch direct(glm::ivec3 globalSize, glm::ivec3 groupSize);
    static Dispatch indirect(GLuint buffer, GLintptr offset = 0);

    bool       isIndirect = false;
    glm::ivec3 globalSize{};
    glm::ivec3 groupSize{1};
    GLuint     buffer = 0;
    GLintptr   offset = 0;
};

// -----------------------------------------------------------------------------
// Pass — a linked GPU program plus its uniform cache.
//
// Owns its source paths and its Defines, so building it needs nothing but the
// config: `link()` and `reload()` take no shader arguments, and a hot reload
// cannot drift from the original build. Subclasses supply only `build()`.
// -----------------------------------------------------------------------------
class Pass {
public:
    Pass(const Pass&)            = delete;
    Pass& operator=(const Pass&) = delete;
    virtual ~Pass()              = default;

    // Build for the first time. On failure the pass keeps its previous program
    // (none, initially) so a broken shader never yields a half-built pipeline.
    bool link(const core::RendererConfig& cfg);
    // Rebuild from disk; the live program is only replaced once the new one links.
    bool reload(const core::RendererConfig& cfg);
    virtual void destroy();

    void   use()     const { glUseProgram(id); }
    GLuint program() const { return id; }
    bool   valid()   const { return id != 0; }

    // glProgramUniform*, not glUniform*: the write names its program explicitly
    // instead of landing on whichever one happens to be current. That removes the
    // "set a uniform before use()" failure mode — which is silent, since the write
    // succeeds against the wrong program without a GL error.
    void set(std::string_view name, GLint  v) const;
    void set(std::string_view name, GLuint v) const;
    void set(std::string_view name, float  v) const;
    void set(std::string_view name, const glm::ivec2& v) const;
    void set(std::string_view name, const glm::uvec3& v) const;

protected:
    Pass(Library& library, Defines defines, uint32_t ubos);

    // Compile the stages and link them. Returns 0 on failure (already logged).
    virtual GLuint build(const core::RendererConfig& cfg) = 0;

    // Compile one stage with this pass's defines applied.
    GLuint compileStage(const char* path, GLenum type, const core::RendererConfig& cfg) const;
    // Attach, link, and delete the stages. Returns 0 on failure.
    static GLuint linkStages(const GLuint* stages, int count, const core::RendererConfig& cfg);

private:
    void adopt(GLuint newProgram, const core::RendererConfig& cfg);
    GLint loc(std::string_view name) const;

    GLuint   id   = 0;
    uint32_t ubos = UBO_NONE;
    Defines  defines;
    std::map<std::string, GLint, std::less<>> uniforms;
};

// -----------------------------------------------------------------------------
class ComputePass : public Pass {
public:
    ComputePass(Library& library, const char* path,
                Defines defines = {}, uint32_t ubos = UBO_NONE)
        : Pass(library, std::move(defines), ubos), path(path) {}

    // Binds the program itself, so a dispatch can never run against whatever was
    // left current by the previous pass.
    void dispatch(const Dispatch& d) const;

protected:
    GLuint build(const core::RendererConfig& cfg) override;

private:
    const char* path;
};

// -----------------------------------------------------------------------------
// The fullscreen blit to the swapchain, and the offscreen target it can draw to
// instead. The quad and the render target belong to the pass that draws them, so
// they are torn down with it.
// -----------------------------------------------------------------------------
class FinalRasterPass : public Pass {
public:
    FinalRasterPass(Library& library, const char* vertPath, const char* fragPath,
                    Defines defines = {}, uint32_t ubos = UBO_NONE)
        : Pass(library, std::move(defines), ubos),
          vertPath(vertPath), fragPath(fragPath) {}

    // Binds the program and the quad, then draws. Same contract as
    // ComputePass::dispatch: a pass is never drawn against a program it does not own.
    void draw() const;
    void destroy() override;

    GLuint VBO         = 0;
    GLuint VAO         = 0;
    GLuint framebuffer = 0;
    GLuint rbo         = 0;
    GLuint texture     = 0;   // colour attachment of `framebuffer`

protected:
    GLuint build(const core::RendererConfig& cfg) override;

private:
    const char* vertPath;
    const char* fragPath;
};

// -----------------------------------------------------------------------------
// Library — every pass registers itself here on construction, so building,
// hot-reloading and tearing down the whole pipeline are one call each and cannot
// miss a pass that was added later.
// -----------------------------------------------------------------------------
class Library {
public:
    bool linkAll  (const core::RendererConfig& cfg);
    bool reloadAll(const core::RendererConfig& cfg);
    void destroyAll();

private:
    friend class Pass;
    std::vector<Pass*> passes;
};

// Drain the GL error queue; clears `success` and logs on any error found.
void checkGLError(const char* label, bool& success, const core::RendererConfig& cfg);

} // namespace shader
