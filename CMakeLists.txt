cmake_minimum_required(VERSION 3.11)
project(VoxelEngine3.0 VERSION 1.0)
set(CMAKE_CXX_STANDARD 17)
include(FetchContent)

set(CMAKE_BUILD_TYPE Debug)
if ( ${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR} )
    message( FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there. You may need to remove CMakeCache.txt." )
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)


# SETUP VARIABLES:

set(CMAKE_INCLUDE_PATH ${PROJECT_SOURCE_DIR}/include)
set(OUTPUT_DIR ${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${OUTPUT_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${OUTPUT_DIR})
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/source)
set(CMAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(GLAD_OUT_DIR        "${CMAKE_CURRENT_BINARY_DIR}" CACHE STRING "Output directory" FORCE)
set(GLAD_PROFILE        "core" CACHE STRING "OpenGL profile" FORCE)
set(GLAD_API            "gl=3.3" CACHE STRING "API type/version pairs, like \"gl=3.2,gles=\", no version means latest" FORCE)
set(GLAD_GENERATOR      "c" CACHE STRING "Language to generate the binding for" FORCE)
set(GLAD_EXTENSIONS     "" CACHE STRING "Path to extensions file or comma separated list of extensions, if missing all extensions are included" FORCE)
set(GLAD_SPEC           "gl" CACHE STRING "Name of the spec" FORCE)
set(GLAD_ALL_EXTENSIONS OFF CACHE BOOL "Include all extensions instead of those specified by GLAD_EXTENSIONS" FORCE)
set(GLAD_NO_LOADER      OFF CACHE BOOL "No loader" FORCE)
set(GLAD_REPRODUCIBLE   OFF CACHE BOOL "Reproducible build" FORCE)

# INCLUDE FILES THAT SHOULD BE COMPILED:

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.hpp" "src/*.c" "src/*.h")

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.90.8
)

FetchContent_GetProperties(imgui)

if (NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

file (GLOB SRC_IMGUI "${imgui_SOURCE_DIR}/*.cpp" "${imgui_SOURCE_DIR}/*.hpp" "${imgui_SOURCE_DIR}/*.c" "${imgui_SOURCE_DIR}/*.h" "${imgui_SOURCE_DIR}/backends/*glfw.h" "${imgui_SOURCE_DIR}/backends/*glfw.cpp" "${imgui_SOURCE_DIR}/backends/*opengl3.h" "${imgui_SOURCE_DIR}/backends/*opengl3.cpp" "${imgui_SOURCE_DIR}/backends/*opengl3_loader.h")
list (APPEND SRC ${SRC_IMGUI})

message(STATUS "Add source files:")
foreach(SRC_FILE IN LISTS SRC)
    message(STATUS "${SRC_FILE}")
endforeach()
message(STATUS "")

add_executable(${PROJECT_NAME} ${SRC})

# Perform dependency linkage

include("${CMAKE_DIR}/LinkGLFW.cmake")
LinkGLFW(${PROJECT_NAME} PRIVATE)

include("${CMAKE_DIR}/LinkGLAD.cmake")
LinkGLAD(${PROJECT_NAME} PRIVATE)

include("${CMAKE_DIR}/LinkGLM.cmake")
LinkGLM(${PROJECT_NAME} PRIVATE)

target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_SOURCE_DIR})

# Setup Threads

set(CMAKE_THREAD_PREFER_PTHREAD)
find_package(Threads REQUIRED)
if(Threads_FOUND)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_THREAD_LIBS_INIT})
endif()

# Setup OpenGL

find_package(OpenGL REQUIRED)
if (OpenGL_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${OPENGL_INCLDUE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES})
else()
    message(FATAL_ERROR "OpenGL was not found on the system")
endif()

# Handle assets

message(STATUS "Copying all assets to destination folder...")
file(
    COPY assets
    DESTINATION ${OUTPUT_DIR}
)

# Handle shaders

file(GLOB_RECURSE shaders "*.glsl" "*.hlsl" "*.frag" "*.vert" "*.comp")
file(COPY ${shaders} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/shd)