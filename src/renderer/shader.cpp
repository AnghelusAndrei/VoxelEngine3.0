#include "shader.hpp"
#include <fstream>
#include <sstream>
#include <cmath>

// Recursive #include resolver.  OpenGL does not natively support `#include`;
// we substitute the directive with the included file's contents on the host
// before calling glShaderSource.  Includes are resolved relative to the
// *including* file's directory, matching C/C++ `#include "..."` semantics.
static std::string resolveIncludes(const std::string& source,
                                   const std::string& sourceDir,
                                   int depth = 0)
{
    if (depth > 16) return source;
    std::string out;
    out.reserve(source.size());
    size_t i = 0;
    while (i < source.size()) {
        size_t lineEnd = source.find('\n', i);
        if (lineEnd == std::string::npos) lineEnd = source.size();
        std::string line = source.substr(i, lineEnd - i);

        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p < line.size() && line.compare(p, 8, "#include") == 0) {
            size_t q1 = line.find('"', p + 8);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos
                                                   : line.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                std::string inc     = line.substr(q1 + 1, q2 - q1 - 1);
                std::string incPath = sourceDir + inc;
                std::ifstream incFile(incPath);
                if (incFile.is_open()) {
                    std::stringstream ss;
                    ss << incFile.rdbuf();
                    out += "// #include \"" + inc + "\"\n";
                    out += resolveIncludes(ss.str(), sourceDir, depth + 1);
                    if (!out.empty() && out.back() != '\n') out += '\n';
                    i = (lineEnd < source.size()) ? lineEnd + 1 : source.size();
                    continue;
                }
            }
        }
        out.append(line);
        if (lineEnd < source.size()) out += '\n';
        i = (lineEnd < source.size()) ? lineEnd + 1 : source.size();
    }
    return out;
}

namespace shader {

GLuint compile(const char* path, GLuint gl_type, const core::RendererConfig& cfg) {
    std::string raw;
    std::ifstream file;
    file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    try {
        file.open(path);
        std::stringstream ss;
        ss << file.rdbuf();
        raw = ss.str();
    } catch (std::ifstream::failure& e) {
        cfg.logMessage("[%f] SHADER::FILE_NOT_FOUND: %s\n", glfwGetTime(), e.what());
        return 0;
    }

    std::string pathStr(path);
    size_t slash = pathStr.find_last_of("/\\");
    std::string dir = (slash == std::string::npos) ? std::string()
                                                   : pathStr.substr(0, slash + 1);
    std::string src = resolveIncludes(raw, dir);
    const char* code = src.c_str();

    GLuint shader = glCreateShader(gl_type);
    glShaderSource(shader, 1, &code, NULL);
    glCompileShader(shader);

    int ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, 1024, NULL, log);
        cfg.logMessage("[%f] SHADER_COMPILE_ERROR:\n%s\n", glfwGetTime(), log);
    }
    return shader;
}

void linkCompute(core::ComputePass& pass, const char* shaderFile,
                 const core::RendererConfig& cfg) {
    GLuint s     = compile(shaderFile, GL_COMPUTE_SHADER, cfg);
    pass.program = glCreateProgram();
    glAttachShader(pass.program, s);
    glLinkProgram(pass.program);
    glDeleteShader(s);
    checkProgram(pass.program, cfg);
}

void linkRaster(core::RasterPass& pass, const char* vertFile, const char* fragFile,
                const core::RendererConfig& cfg) {
    GLuint vs = compile(vertFile, GL_VERTEX_SHADER,   cfg);
    GLuint fs = compile(fragFile, GL_FRAGMENT_SHADER, cfg);
    pass.program = glCreateProgram();
    glAttachShader(pass.program, vs);
    glAttachShader(pass.program, fs);
    glLinkProgram(pass.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    checkProgram(pass.program, cfg);
}

void relinkCompute(core::ComputePass& pass, const char* shaderFile,
                   const core::RendererConfig& cfg) {
    GLuint s  = compile(shaderFile, GL_COMPUTE_SHADER, cfg);
    GLuint np = glCreateProgram();
    glAttachShader(np, s);
    glLinkProgram(np);
    glDeleteShader(s);
    checkProgram(np, cfg);
    glDeleteProgram(pass.program);
    pass.program = np;
}

void relinkRaster(core::RasterPass& pass, const char* vertFile, const char* fragFile,
                  const core::RendererConfig& cfg) {
    GLuint vs = compile(vertFile, GL_VERTEX_SHADER,   cfg);
    GLuint fs = compile(fragFile, GL_FRAGMENT_SHADER, cfg);
    GLuint np = glCreateProgram();
    glAttachShader(np, vs);
    glAttachShader(np, fs);
    glLinkProgram(np);
    glDeleteShader(vs);
    glDeleteShader(fs);
    checkProgram(np, cfg);
    glDeleteProgram(pass.program);
    pass.program = np;
}

void checkProgram(GLuint program, const core::RendererConfig& cfg) {
    int ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return;
    char log[1024];
    glGetProgramInfoLog(program, 1024, NULL, log);
    cfg.logMessage("[%f] PROGRAM_LINK_ERROR:\n%s\n", glfwGetTime(), log);
}

void checkGLError(const char* label, bool& success, const core::RendererConfig& cfg) {
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        cfg.logMessage("[%f] GL_ERROR at %s: %u\n", glfwGetTime(), label, (unsigned int)err);
        success = false;
    }
}

void dispatch(const core::DispatchArgs& args) {
    if (args.mode == core::DispatchMode::Direct) {
        glDispatchCompute(
            (GLuint)std::ceil((float)args.direct.globalSize.x / (float)args.direct.groupSize.x),
            (GLuint)std::ceil((float)args.direct.globalSize.y / (float)args.direct.groupSize.y),
            1u
        );
    } else {
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, args.indirect.buffer);
        glDispatchComputeIndirect(args.indirect.offset);
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, 0);
    }
}

} // namespace shader
