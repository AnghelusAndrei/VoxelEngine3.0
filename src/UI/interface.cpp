#include "interface.hpp"

Interface::Interface(GLFWwindow *window_, const char* glsl_version_) : window(window_), glsl_version(glsl_version_), io(ImGui::GetIO()){
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
}

void Interface::Draw(Widget *widgets[], uint32_t widgetsNum){
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    for(int i = 0; i < widgetsNum; i++){
        widgets[i]->Render();
    }

    ImGui::Render();
}

void Interface::Render(){
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

Interface::~Interface(){
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}