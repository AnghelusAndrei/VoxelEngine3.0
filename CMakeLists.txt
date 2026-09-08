cmake_minimum_required(VERSION 3.14)

# ---------------------------------------------------------------------------
# 64-bit toolchain detection (must run BEFORE project())
# ---------------------------------------------------------------------------
# On Windows the default `g++` is often MinGW.org's 32-bit GCC while a 64-bit
# mingw-w64 sits elsewhere. Autodetect it here so the common case needs no -D
# flags; set CMAKE_CXX_COMPILER yourself to override. No-op on Linux/macOS.

if(WIN32 AND NOT CMAKE_CXX_COMPILER)
    file(GLOB _mingw64_roots
        "C:/Strawberry/c"
        "C:/msys64/mingw64"
        "C:/mingw64"
        "C:/ProgramData/chocolatey/lib/mingw/tools/install/mingw64"
        "$ENV{ProgramFiles}/mingw-w64/*/mingw64")

    foreach(_root IN LISTS _mingw64_roots)
        if(NOT EXISTS "${_root}/bin/g++.exe")
            continue()
        endif()

        execute_process(
            COMMAND "${_root}/bin/g++.exe" -dumpmachine
            OUTPUT_VARIABLE _triple
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)

        if(_triple MATCHES "^x86_64")
            set(CMAKE_C_COMPILER   "${_root}/bin/gcc.exe" CACHE FILEPATH "" FORCE)
            set(CMAKE_CXX_COMPILER "${_root}/bin/g++.exe" CACHE FILEPATH "" FORCE)
            if(EXISTS "${_root}/bin/mingw32-make.exe")
                set(CMAKE_MAKE_PROGRAM "${_root}/bin/mingw32-make.exe" CACHE FILEPATH "" FORCE)
            endif()
            message(STATUS "Using 64-bit toolchain: ${_root}/bin (${_triple})")
            break()
        endif()
    endforeach()

    unset(_mingw64_roots)
    unset(_root)
    unset(_triple)
endif()

project(VoxelEngine3.0 VERSION 1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Guards
# ---------------------------------------------------------------------------
if(CMAKE_SIZEOF_VOID_P LESS 8)
    message(FATAL_ERROR
        "32-bit compiler detected (${CMAKE_CXX_COMPILER}, pointer size "
        "${CMAKE_SIZEOF_VOID_P}). This engine needs a 64-bit address space — large "
        "scenes exhaust 2 GB during import. Install mingw-w64 (or MSYS2) and point "
        "CMake at it:\n"
        "  cmake -G \"MinGW Makefiles\" -DCMAKE_CXX_COMPILER=<path>/g++.exe ..")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Create a separate build directory and "
        "run CMake from there. You may need to remove CMakeCache.txt.")
endif()

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/shd")
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")

set(CMAKE_INCLUDE_PATH           "${PROJECT_SOURCE_DIR}/include")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

# ---------------------------------------------------------------------------
# GLAD generator options (consumed by cmake/LinkGLAD.cmake)
# ---------------------------------------------------------------------------
set(GLAD_OUT_DIR        "${CMAKE_CURRENT_BINARY_DIR}" CACHE STRING "Output directory" FORCE)
set(GLAD_PROFILE        "core"  CACHE STRING "OpenGL profile" FORCE)
set(GLAD_API            "gl=4.6" CACHE STRING "API type/version pairs" FORCE)
set(GLAD_GENERATOR      "c"     CACHE STRING "Binding language" FORCE)
set(GLAD_SPEC           "gl"    CACHE STRING "Name of the spec" FORCE)
set(GLAD_EXTENSIONS     ""      CACHE STRING "Extensions file or comma-separated list" FORCE)
set(GLAD_ALL_EXTENSIONS OFF     CACHE BOOL   "Include all extensions" FORCE)
set(GLAD_NO_LOADER      OFF     CACHE BOOL   "No loader" FORCE)
set(GLAD_REPRODUCIBLE   OFF     CACHE BOOL   "Reproducible build" FORCE)

# ---------------------------------------------------------------------------
# Dear ImGui
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG        v1.90.8
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

add_executable(${PROJECT_NAME} ${ENGINE_SOURCES} ${IMGUI_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")

include("${CMAKE_DIR}/LinkGLFW.cmake")
include("${CMAKE_DIR}/LinkGLAD.cmake")
include("${CMAKE_DIR}/LinkGLM.cmake")
include("${CMAKE_DIR}/LinkSTB.cmake")
LinkGLFW(${PROJECT_NAME} PRIVATE)
LinkGLAD(${PROJECT_NAME} PRIVATE)
LinkGLM(${PROJECT_NAME} PRIVATE)
LinkSTB(${PROJECT_NAME} PRIVATE)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads OpenGL::GL)

# ---------------------------------------------------------------------------
# Assets and shaders
# ---------------------------------------------------------------------------

file(GLOB_RECURSE SHADERS CONFIGURE_DEPENDS
    "${SHADER_DIR}/*.glsl"
    "${SHADER_DIR}/*.hlsl"
    "${SHADER_DIR}/*.vert"
    "${SHADER_DIR}/*.frag"
    "${SHADER_DIR}/*.comp")

message(STATUS "Copying assets and ${CMAKE_CURRENT_SOURCE_DIR}/src/shd shaders to ${OUTPUT_DIR}")

file(COPY assets DESTINATION "${OUTPUT_DIR}")
file(COPY ${SHADERS} DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/shd")
file(COPY ${SHADERS} DESTINATION "${OUTPUT_DIR}/shd")

# Re-copy shaders on EVERY build, not just when the exe relinks.
#
# As a POST_BUILD command this only fired when the target itself was rebuilt, so a
# shader-only edit left the binary running the previous copy - silently, since the
# engine loads from ./shd at runtime and the in-app "Recompile shaders" button reads
# the same stale directory. An ALL custom target always runs.
add_custom_target(refresh_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SHADER_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/shd"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SHADER_DIR}" "${OUTPUT_DIR}/shd"
    COMMENT "Refreshing shaders from src/shd")
add_dependencies(${PROJECT_NAME} refresh_shaders)