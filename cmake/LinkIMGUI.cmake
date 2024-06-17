include(FetchContent)

function(LinkIMGUI TARGET ACCESS IMGUI_SOURCE_DIR SRC)
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
    list (APPEND SRC ${SRC_IMGUI} PARENT_SCOPE)
    set (${IMGUI_SOURCE_DIR} ${imgui_SOURCE_DIR} PARENT_SCOPE)
endfunction()