#include "shader.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

namespace shader {
namespace {

constexpr const char* SHADER_DIR = "./shd/";
constexpr int         MAX_INCLUDE_DEPTH = 16;
constexpr GLsizei     LOG_CHARS = 1024;

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Recursive #include resolver. OpenGL has no native #include, so the directive is
// replaced with the file's contents before glShaderSource. Paths resolve against
// the shader directory, matching C/C++ `#include "..."` semantics.
std::string resolveIncludes(const std::string& source, const core::RendererConfig& cfg,
                            int depth = 0) {
    if (depth > MAX_INCLUDE_DEPTH) {
        cfg.logMessage("SHADER::INCLUDE_TOO_DEEP: giving up past %d levels\n", MAX_INCLUDE_DEPTH);
        return source;
    }

    std::string out;
    out.reserve(source.size());

    for (size_t i = 0; i < source.size();) {
        size_t end  = source.find('\n', i);
        if (end == std::string::npos) end = source.size();
        std::string_view line(source.data() + i, end - i);
        i = (end < source.size()) ? end + 1 : source.size();

        size_t p = line.find_first_not_of(" \t");
        size_t q1 = (p != std::string_view::npos && line.compare(p, 8, "#include") == 0)
                        ? line.find('"', p + 8) : std::string_view::npos;
        size_t q2 = (q1 != std::string_view::npos) ? line.find('"', q1 + 1)
                                                   : std::string_view::npos;
        if (q2 == std::string_view::npos) {                       // ordinary line
            out.append(line);
            out += '\n';
            continue;
        }

        std::string name(line.substr(q1 + 1, q2 - q1 - 1));
        std::string body = readFile(SHADER_DIR + name);
        if (body.empty()) {
            // A missing header is a build error, not something to paper over: the
            // shader would fail later with a confusing "undefined identifier".
            cfg.logMessage("SHADER::INCLUDE_NOT_FOUND: \"%s\"\n", name.c_str());
            continue;
        }
        out += "// #include \"" + name + "\"\n";
        out += resolveIncludes(body, cfg, depth + 1);
        if (!out.empty() && out.back() != '\n') out += '\n';
    }
    return out;
}

void bindUniformBlock(GLuint program, const char* blockName, GLuint binding) {
    GLuint index = glGetUniformBlockIndex(program, blockName);
    if (index != GL_INVALID_INDEX) glUniformBlockBinding(program, index, binding);
}

} // namespace

// -----------------------------------------------------------------------------
// Defines
// -----------------------------------------------------------------------------
Defines& Defines::define(std::string_view name) {
    lines.emplace_back("#define " + std::string(name));
    return *this;
}
Defines& Defines::define(std::string_view name, std::string_view value) {
    lines.emplace_back("#define " + std::string(name) + ' ' + std::string(value));
    return *this;
}
Defines& Defines::define(std::string_view name, long long value) {
    return define(name, std::to_string(value));
}
Defines& Defines::define(std::string_view name, double value) {
    return define(name, std::to_string(value));
}
Defines& Defines::include(std::string_view path) {
    lines.emplace_back("#include \"" + std::string(path) + '"');
    return *this;
}

std::string Defines::preamble() const {
    std::string out;
    for (const std::string& l : lines) { out += l; out += '\n'; }
    return out;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------
Dispatch Dispatch::direct(glm::ivec2 globalSize, glm::ivec2 groupSize) {
    return direct(glm::ivec3(globalSize, 1), glm::ivec3(groupSize, 1));
}
Dispatch Dispatch::direct(glm::ivec3 globalSize, glm::ivec3 groupSize) {
    Dispatch d;
    d.isIndirect = false;
    d.globalSize = globalSize;
    d.groupSize  = groupSize;
    return d;
}
Dispatch Dispatch::indirect(GLuint buffer, GLintptr offset) {
    Dispatch d;
    d.isIndirect = true;
    d.buffer     = buffer;
    d.offset     = offset;
    return d;
}

// -----------------------------------------------------------------------------
// Pass
// -----------------------------------------------------------------------------
Pass::Pass(Library& library, Defines defines_, uint32_t ubos_)
    : ubos(ubos_), defines(std::move(defines_)) {
    library.passes.push_back(this);
}

bool Pass::link(const core::RendererConfig& cfg) {
    GLuint built = build(cfg);
    if (!built) return false;
    adopt(built, cfg);
    return true;
}

bool Pass::reload(const core::RendererConfig& cfg) {
    GLuint built = build(cfg);
    if (!built) return false;          // keep the last good program live
    if (id) glDeleteProgram(id);
    adopt(built, cfg);
    return true;
}

void Pass::destroy() {
    if (id) glDeleteProgram(id);
    id = 0;
    uniforms.clear();
}

// Take ownership of a freshly linked program: bind its UBO blocks and enumerate
// its uniforms once, so set() never calls the driver to look one up.
void Pass::adopt(GLuint newProgram, const core::RendererConfig& cfg) {
    id = newProgram;
    uniforms.clear();

    if (ubos & UBO_CAMERA) bindUniformBlock(id, "CameraUniform", 0);

    GLint count = 0, maxLen = 0;
    glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(id, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLen);
    std::vector<char> buf((maxLen > 1) ? (size_t)maxLen : 1u);

    for (GLint i = 0; i < count; i++) {
        GLsizei len = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(id, i, (GLsizei)buf.size(), &len, &size, &type, buf.data());
        std::string name(buf.data(), (size_t)len);
        // An array uniform is reported as "name[0]"; store the bare name so callers
        // address it the way it is written in the shader.
        if (size_t bracket = name.find('['); bracket != std::string::npos)
            name.resize(bracket);

        // Uniforms inside a UBO block report -1: they are addressed by block
        // binding, not by location.
        if (GLint l = glGetUniformLocation(id, name.c_str()); l >= 0)
            uniforms.emplace(std::move(name), l);
    }

    if (!cfg.debuggingEnabled) return;
    // set() on an unknown name is a defined no-op, so this listing is how you tell
    // a typo from a uniform the compiler dropped as unused.
    std::string names;
    for (const auto& [name, _] : uniforms) { names += ' '; names += name; }
    cfg.logMessage("[shader] program %u uniforms:%s\n", id,
                   names.empty() ? " (none)" : names.c_str());
}

GLint Pass::loc(std::string_view name) const {
    auto it = uniforms.find(name);
    return (it == uniforms.end()) ? -1 : it->second;
}

void Pass::set(std::string_view n, GLint  v) const { glProgramUniform1i (id, loc(n), v); }
void Pass::set(std::string_view n, GLuint v) const { glProgramUniform1ui(id, loc(n), v); }
void Pass::set(std::string_view n, float  v) const { glProgramUniform1f (id, loc(n), v); }
void Pass::set(std::string_view n, const glm::ivec2& v) const { glProgramUniform2i (id, loc(n), v.x, v.y); }
void Pass::set(std::string_view n, const glm::uvec3& v) const { glProgramUniform3ui(id, loc(n), v.x, v.y, v.z); }

GLuint Pass::compileStage(const char* path, GLenum type,
                          const core::RendererConfig& cfg) const {
    std::string src = readFile(path);
    if (src.empty()) {
        cfg.logMessage("SHADER::FILE_NOT_FOUND: %s\n", path);
        return 0;
    }

    // `#version` must be the first directive, so the preamble goes on the line
    // after it — before every #extension and #include. Includes are resolved
    // afterwards so that Defines::include() is expanded too.
    if (!defines.empty()) {
        size_t v   = src.find("#version");
        size_t at  = (v == std::string::npos) ? 0 : src.find('\n', v);
        at = (at == std::string::npos) ? src.size() : at + 1;
        src.insert(at, defines.preamble());
    }
    src = resolveIncludes(src, cfg);

    const char* code = src.c_str();
    GLuint stage = glCreateShader(type);
    glShaderSource(stage, 1, &code, nullptr);
    glCompileShader(stage);

    GLint ok = 0;
    glGetShaderiv(stage, GL_COMPILE_STATUS, &ok);
    if (ok) return stage;

    char log[LOG_CHARS];
    glGetShaderInfoLog(stage, LOG_CHARS, nullptr, log);
    cfg.logMessage("SHADER_COMPILE_ERROR (%s):\n%s\n", path, log);
    glDeleteShader(stage);
    return 0;
}

GLuint Pass::linkStages(const GLuint* stages, int count, const core::RendererConfig& cfg) {
    // A zero stage means compileStage already failed and logged. Give up rather
    // than link a program missing a stage and fail again with a vaguer message.
    for (int i = 0; i < count; i++) {
        if (stages[i]) continue;
        for (int j = 0; j < count; j++) if (stages[j]) glDeleteShader(stages[j]);
        return 0;
    }

    GLuint program = glCreateProgram();
    for (int i = 0; i < count; i++) glAttachShader(program, stages[i]);
    glLinkProgram(program);
    for (int i = 0; i < count; i++) glDeleteShader(stages[i]);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return program;

    char log[LOG_CHARS];
    glGetProgramInfoLog(program, LOG_CHARS, nullptr, log);
    cfg.logMessage("PROGRAM_LINK_ERROR:\n%s\n", log);
    glDeleteProgram(program);          // never hand back a dead handle
    return 0;
}

// -----------------------------------------------------------------------------
// ComputePass
// -----------------------------------------------------------------------------
GLuint ComputePass::build(const core::RendererConfig& cfg) {
    GLuint stage = compileStage(path, GL_COMPUTE_SHADER, cfg);
    return linkStages(&stage, 1, cfg);
}

void ComputePass::dispatch(const Dispatch& d) const {
    use();
    if (d.isIndirect) {
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, d.buffer);
        glDispatchComputeIndirect(d.offset);
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
        return;
    }
    glDispatchCompute((GLuint)((d.globalSize.x + d.groupSize.x - 1) / d.groupSize.x),
                      (GLuint)((d.globalSize.y + d.groupSize.y - 1) / d.groupSize.y),
                      (GLuint)((d.globalSize.z + d.groupSize.z - 1) / d.groupSize.z));
}

// -----------------------------------------------------------------------------
// FinalRasterPass
// -----------------------------------------------------------------------------
GLuint FinalRasterPass::build(const core::RendererConfig& cfg) {
    GLuint stages[2] = { compileStage(vertPath, GL_VERTEX_SHADER,   cfg),
                         compileStage(fragPath, GL_FRAGMENT_SHADER, cfg) };
    return linkStages(stages, 2, cfg);
}

void FinalRasterPass::draw() const {
    use();
    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void FinalRasterPass::destroy() {
    Pass::destroy();
    if (VBO)         glDeleteBuffers      (1, &VBO);
    if (VAO)         glDeleteVertexArrays (1, &VAO);
    if (framebuffer) glDeleteFramebuffers (1, &framebuffer);
    if (rbo)         glDeleteRenderbuffers(1, &rbo);
    if (texture)     glDeleteTextures     (1, &texture);
    VBO = VAO = framebuffer = rbo = texture = 0;
}

// -----------------------------------------------------------------------------
// Library
// -----------------------------------------------------------------------------
bool Library::linkAll(const core::RendererConfig& cfg) {
    bool ok = true;
    for (Pass* p : passes) ok &= p->link(cfg);
    return ok;
}

bool Library::reloadAll(const core::RendererConfig& cfg) {
    bool ok = true;
    for (Pass* p : passes) ok &= p->reload(cfg);
    return ok;
}

void Library::destroyAll() {
    for (Pass* p : passes) p->destroy();
}

// -----------------------------------------------------------------------------
void checkGLError(const char* label, bool& success, const core::RendererConfig& cfg) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        cfg.logMessage("[%f] GL_ERROR at %s: %u\n", glfwGetTime(), label, (unsigned)err);
        success = false;
    }
}

} // namespace shader
