include(FetchContent)

macro(LinkSTB TARGET ACCESS)
    # stb is a collection of single-header public-domain libraries with no build
    # system, so there is no target to link — MakeAvailable just puts the sources
    # on disk and we expose the directory as an INTERFACE include path.
    #
    # Only stb_image.h is used: scene.cpp decodes .mtl `map_Kd` textures to derive
    # per-voxel albedo. Pinned to a commit, not a branch, because stb has no tags.
    FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb
        GIT_TAG        f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31
        GIT_SHALLOW    FALSE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(stb)

    add_library(stb_image INTERFACE)
    target_include_directories(stb_image SYSTEM INTERFACE "${stb_SOURCE_DIR}")

    target_link_libraries(${TARGET} ${ACCESS} stb_image)
endmacro()
