#pragma once
#include "core.hpp"

// Free-function shader utilities. All GL operations on programs and stages
// live here; pass structs remain plain POD descriptor bundles.
namespace shader {

// Compile a single GLSL stage from a file path (recursive #include support).
// Returns 0 on failure; error is logged via cfg.
GLuint compile(const char* path, GLuint gl_type, const core::RendererConfig& cfg);

// Link a program from source files and store the program handle in the pass.
void linkCompute(core::ComputePass&  pass, const char* shaderFile,
                 const core::RendererConfig& cfg);
void linkRaster (core::RasterPass&   pass, const char* vertFile, const char* fragFile,
                 const core::RendererConfig& cfg);

// Hot-reload: compile → link → swap → destroy old program.
void relinkCompute(core::ComputePass& pass, const char* shaderFile,
                   const core::RendererConfig& cfg);
void relinkRaster (core::RasterPass&  pass, const char* vertFile, const char* fragFile,
                   const core::RendererConfig& cfg);

// Post-link diagnostic — logs on failure.
void checkProgram(GLuint program, const core::RendererConfig& cfg);

// Drain the GL error queue; sets success=false on any error found.
void checkGLError(const char* label, bool& success, const core::RendererConfig& cfg);

// Issue glDispatchCompute (Direct) or glDispatchComputeIndirect (Indirect).
void dispatch(const core::DispatchArgs& args);

} // namespace shader
