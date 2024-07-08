#include "renderer.hpp"


Renderer::Renderer(const Config *config){
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char* glsl_version = "#version 100";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif
    
    // Create window with graphics context
    window = glfwCreateWindow(config->window_width, config->window_height, config->window_title, nullptr, nullptr);
    if (window == nullptr)
        return;
    glfwMakeContextCurrent(window);
    gladLoadGL();
    glfwSwapInterval(0); // Enable vsync

    InitImGUI(glsl_version);

    clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
    aspect_ratio = config->aspect_ratio;

    vertex = compileShader("./shd/vertex.glsl", "VERTEX", GL_VERTEX_SHADER);
    fragment = compileShader("./shd/fragment.glsl", "FRAGMENT", GL_FRAGMENT_SHADER);
    programID = glCreateProgram();
    glAttachShader(programID, vertex);
    glAttachShader(programID, fragment);
    glLinkProgram(programID);
    checkShaderCompileErrors(programID, "PROGRAM");

    glfwGetFramebufferSize(window, &display_size.x, &display_size.y);
    glm::vec2 viewRatio = glm::vec2(
        aspect_ratio * (float)display_size.y / (float)display_size.x,
        ((float)display_size.x / (float)display_size.y) / aspect_ratio
    );

    float vertices[] = {
        viewRatio.x,  viewRatio.y, 0.0f,  // top right
        viewRatio.x, -viewRatio.y, 0.0f,  // bottom right
        -viewRatio.x, -viewRatio.y, 0.0f,  // bottom left
        -viewRatio.x,  viewRatio.y, 0.0f   // top left 
    };
    unsigned int indices[] = {  
        0, 1, 3,  // first Triangle
        1, 2, 3   // second Triangle
    };
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); 
    glBindVertexArray(0); 

    log = new Log();
}

uint32_t Renderer::compileShader(const char* path, std::string type, uint32_t gl_type){
    //Read shader from disk
    std::string shaderCodeRaw;
    std::ifstream shaderFile;
    shaderFile.exceptions (std::ifstream::failbit | std::ifstream::badbit);
    try 
    {
        shaderFile.open(path);
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        shaderFile.close();
        shaderCodeRaw = shaderStream.str();
    }
    catch (std::ifstream::failure& e)
    {
        std::cout << "ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ: " << e.what() << std::endl;
    }
    const char* shaderCode = shaderCodeRaw.c_str();

    //compile shader
    GLuint shader = glCreateShader(gl_type);
    glShaderSource(shader, 1, &shaderCode, NULL);
    glCompileShader(shader);
    checkShaderCompileErrors(shader, type);
    return shader;
}

void Renderer::InitImGUI(const char* glsl_version){
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ui = new UIConfig{
        .io = ImGui::GetIO()
    };

    (void)ui->io;
    ui->io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    ui->io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
}

void Renderer::run(){

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGUIpass();
    ImGui::Render();
    glm::ivec2 old_display = display_size;
    glfwGetFramebufferSize(window, &display_size.x, &display_size.y);
    glViewport(0, 0, display_size.x, display_size.y);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    
    if(old_display != display_size){
        glm::vec2 viewRatio = glm::vec2(
            aspect_ratio * (float)display_size.y / (float)display_size.x,
            ((float)display_size.x / (float)display_size.y) / aspect_ratio
        );

        float vertices[] = {
            viewRatio.x,  viewRatio.y, 0.0f,  // top right
            viewRatio.x, -viewRatio.y, 0.0f,  // bottom right
            -viewRatio.x, -viewRatio.y, 0.0f,  // bottom left
            -viewRatio.x,  viewRatio.y, 0.0f   // top left 
        };

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glUseProgram(programID);

    for(int i = 0; i < textureIDs.size(); i++){
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_BUFFER, textureIDs[i]);
    }

    // Ensure the uniform is set to the correct texture unit
    for(int i = 0; i < uniformiNames.size(); i++){
        GLuint texBufferLocation = glGetUniformLocation(programID, uniformiNames[i].c_str());
        glUniform1i(texBufferLocation, uniformiValues[i]);
    }
    for(int i = 0; i < uniformuiNames.size(); i++){
        GLuint texBufferLocation = glGetUniformLocation(programID, uniformuiNames[i].c_str());
        glUniform1ui(texBufferLocation, uniformuiValues[i]);
    }
    for(int i = 0; i < uniformfNames.size(); i++){
        GLuint texBufferLocation = glGetUniformLocation(programID, uniformfNames[i].c_str());
        glUniform1f(texBufferLocation, uniformfValues[i]);
    }

    glBindVertexArray(VAO); 
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    // Unbind the textures after drawing
    glBindTexture(GL_TEXTURE_BUFFER, 0);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
}

void Renderer::ImGUIpass(){
    {
        static float f = 0.0f;
        static int counter = 0;

        ImGui::Begin("Debug"); 
        ImGui::Text("io.WantCaptureMouse %d, io.WantCaptureKeyboard %d", ui->io.WantCaptureMouse, ui->io.WantCaptureKeyboard);
        ImGui::Spacing();
        ImGui::SliderFloat("speed", ui->speed, 0.0f, 500.0f);
        ImGui::SliderFloat("sensitivity", ui->sensitivity, 0.0f, 2.0f);
        ImGui::Spacing();
        ImGui::Text("Position %.3f x %.3f y %.3f z", ui->position.x, ui->position.y, ui->position.z);
        ImGui::Text("Direction %.3f x %.3f y %.3f z", ui->direction.x, ui->direction.y, ui->direction.z);
        ImGui::Text("Cursor %d x %d y", ui->cursor.x, ui->cursor.y);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ui->io.Framerate, ui->io.Framerate);
        ImGui::End();
    }


    log->Draw("Logger");
}

Renderer::~Renderer(){
    delete ui;
    
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteProgram(programID);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
}

void Renderer::addTexture(GLuint texID){
    textureIDs.push_back(texID);
}

void Renderer::setUniformi(int v, std::string name){
    uniformiValues.push_back(v);
    uniformiNames.push_back(name);
}

void Renderer::setUniformui(unsigned int v, std::string name){
    uniformuiValues.push_back(v);
    uniformuiNames.push_back(name);
}

void Renderer::setUniformf(float v, std::string name){
    uniformfValues.push_back(v);
    uniformfNames.push_back(name);
}

void Renderer::checkShaderCompileErrors(unsigned int shader, std::string type)
{
    int success;
    char infoLog[1024];
    if (type != "PROGRAM")
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, NULL, infoLog);
            std::cout << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << std::endl;
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, NULL, infoLog);
            std::cout << "ERROR::PROGRAM_LINKING_ERROR of type: " << type << "\n" << infoLog << "\n -- --------------------------------------------------- -- " << std::endl;
        }
    }
}

void Renderer::glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}