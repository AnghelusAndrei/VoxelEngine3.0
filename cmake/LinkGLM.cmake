include(FetchContent)

macro(LinkGLM TARGET ACCESS)
    # glm is header-only; MakeAvailable exposes its INTERFACE target, which
    # propagates the include path when linked. SYSTEM silences its warnings.
    FetchContent_Declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm
        GIT_TAG        0.9.9.8
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(glm)

    target_link_libraries(${TARGET} ${ACCESS} glm)
endmacro()
