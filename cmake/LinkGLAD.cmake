include(FetchContent)

macro(LinkGLAD TARGET ACCESS)
    # GLAD generator options are set as cache variables by the top-level
    # CMakeLists before this macro is called. EXCLUDE_FROM_ALL/SYSTEM as in GLFW.
    FetchContent_Declare(glad
        GIT_REPOSITORY https://github.com/Dav1dde/glad
        GIT_TAG        v0.1.33
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)

    # glad v0.1.33's own CMakeLists calls the removed FindPythonInterp module,
    # which trips CMP0148's "policy not set" dev warning. OLD keeps the same
    # module-based lookup that already works, just declared explicitly so the
    # warning doesn't fire. Scoped to this fetch — our own code still warns
    # normally.
    set(_prev_cmp0148 ${CMAKE_POLICY_DEFAULT_CMP0148})
    set(CMAKE_POLICY_DEFAULT_CMP0148 OLD)
    FetchContent_MakeAvailable(glad)
    set(CMAKE_POLICY_DEFAULT_CMP0148 ${_prev_cmp0148})
    unset(_prev_cmp0148)

    set_target_properties(glad                PROPERTIES FOLDER ${PROJECT_NAME}/thirdparty)
    set_target_properties(glad-generate-files PROPERTIES FOLDER ${PROJECT_NAME}/thirdparty)

    target_link_libraries(${TARGET} ${ACCESS} glad)
endmacro()
