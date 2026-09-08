include(FetchContent)

macro(LinkGLFW TARGET ACCESS)
    # Build only the library — no examples, tests, docs, or install rules.
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

    # EXCLUDE_FROM_ALL: glfw is compiled only because our target depends on it,
    # never as part of a bare `make`. SYSTEM: treat its headers as system
    # headers so their warnings don't pollute our build.
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw
        GIT_TAG        3.3.2
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)
    FetchContent_MakeAvailable(glfw)

    set_target_properties(glfw PROPERTIES FOLDER ${PROJECT_NAME}/thirdparty)

    target_link_libraries(${TARGET} ${ACCESS} glfw)
endmacro()
