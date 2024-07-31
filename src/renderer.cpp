#include "renderer.hpp"


Renderer::Renderer(const Config *config){
    log = new Log();

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return;

    // Decide GL+GLSL versions
#if defined(IMGUI_IMPL_OPENGL_ES2)
    // GL ES 2.0 + GLSL 100
    const char* glsl_version = "#version 430";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 430";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 430";
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

    vertex = compileShader("./shd/renderVertex.glsl", "VERTEX", GL_VERTEX_SHADER);
    fragment = compileShader("./shd/renderFragment.glsl", "FRAGMENT", GL_FRAGMENT_SHADER);
    programID = glCreateProgram();
    glAttachShader(programID, vertex);
    glAttachShader(programID, fragment);
    glLinkProgram(programID);
    checkShaderCompileErrors(programID, "PROGRAM");

    computeAccumShader = compileShader("./shd/pixelAccum.comp", "COMPUTE", GL_COMPUTE_SHADER);
    computeAvgShader = compileShader("./shd/pixelAvg.comp", "COMPUTE", GL_COMPUTE_SHADER);

    computeAccumProgramID = glCreateProgram();
    glAttachShader(computeAccumProgramID, computeAccumShader);
    glLinkProgram(computeAccumProgramID);
    checkShaderCompileErrors(computeAccumProgramID, "PROGRAM");

    computeAvgProgramID = glCreateProgram();
    glAttachShader(computeAvgProgramID, computeAvgShader);
    glLinkProgram(computeAvgProgramID);
    checkShaderCompileErrors(computeAvgProgramID, "PROGRAM");

    postVertex = compileShader("./shd/postVertex.glsl", "VERTEX", GL_VERTEX_SHADER);
    postFragment = compileShader("./shd/postFragment.glsl", "FRAGMENT", GL_FRAGMENT_SHADER);
    postProgramID = glCreateProgram();
    glAttachShader(postProgramID, postVertex);
    glAttachShader(postProgramID, postFragment);
    glLinkProgram(postProgramID);
    checkShaderCompileErrors(postProgramID, "PROGRAM");

    glfwGetFramebufferSize(window, &display_size.x, &display_size.y);
    glm::vec2 viewRatio = glm::vec2(
        aspect_ratio * (float)display_size.y / (float)display_size.x,
        ((float)display_size.x / (float)display_size.y) / aspect_ratio
    );

    float vertices[] = {
        -viewRatio.x,  viewRatio.y,  0.0f, 1.0f,
        -viewRatio.x, -viewRatio.y,  0.0f, 0.0f,
         viewRatio.x, -viewRatio.y,  1.0f, 0.0f,

        -viewRatio.x,  viewRatio.y,  0.0f, 1.0f,
         viewRatio.x, -viewRatio.y,  1.0f, 0.0f,
         viewRatio.x,  viewRatio.y,  1.0f, 1.0f
    };

    float quadVertices[] = { // vertex attributes for a quad that fills the entire screen in Normalized Device Coordinates.
        // positions   // texCoords
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,

        -1.0f,  1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f
    };

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), &vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));


    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));


    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Create a color attachment texture
    glGenTextures(1, &textureColorbuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, display_size.x, display_size.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorbuffer, 0);

    // Texture for the voxelColorAccumulationBuffer
    glGenTextures(1, &voxelColorAccumulationBuffer);
    glBindTexture(GL_TEXTURE_2D, voxelColorAccumulationBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, numVoxels, 4, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Texture for the output color buffer
    glGenTextures(1, &outputColorBuffer);
    glBindTexture(GL_TEXTURE_2D, outputColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, display_size.x, display_size.y, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Create a renderbuffer object for depth and stencil attachment (we won't be sampling these)
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, display_size.x, display_size.y); // depth and stencil buffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo); // attach it

    // Check if framebuffer is complete
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        log->AddLog("ERROR::FRAMEBUFFER:: Framebuffer is not complete!");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

    if(glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS)
    {
        vertex = compileShader("./shd/renderVertex.glsl", "VERTEX", GL_VERTEX_SHADER);
        fragment = compileShader("./shd/renderFragment.glsl", "FRAGMENT", GL_FRAGMENT_SHADER);
        programID = glCreateProgram();
        glAttachShader(programID, vertex);
        glAttachShader(programID, fragment);
        glLinkProgram(programID);
        checkShaderCompileErrors(programID, "PROGRAM");

        postVertex = compileShader("./shd/postVertex.glsl", "VERTEX", GL_VERTEX_SHADER);
        postFragment = compileShader("./shd/postFragment.glsl", "FRAGMENT", GL_FRAGMENT_SHADER);
        postProgramID = glCreateProgram();
        glAttachShader(postProgramID, postVertex);
        glAttachShader(postProgramID, postFragment);
        glLinkProgram(postProgramID);
        checkShaderCompileErrors(postProgramID, "PROGRAM");
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGUIpass();
    ImGui::Render();
    glm::ivec2 old_display = display_size;

    glfwGetFramebufferSize(window, &display_size.x, &display_size.y);

    if(old_display != display_size){
        glm::vec2 viewRatio = glm::vec2(
            aspect_ratio * (float)display_size.y / (float)display_size.x,
            ((float)display_size.x / (float)display_size.y) / aspect_ratio
        );

        float vertices[] = {
            -viewRatio.x,  viewRatio.y,  0.0f, 1.0f,
            -viewRatio.x, -viewRatio.y,  0.0f, 0.0f,
            viewRatio.x, -viewRatio.y,  1.0f, 0.0f,

            -viewRatio.x,  viewRatio.y,  0.0f, 1.0f,
            viewRatio.x, -viewRatio.y,  1.0f, 0.0f,
            viewRatio.x,  viewRatio.y,  1.0f, 1.0f
        };

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindTexture(GL_TEXTURE_2D, textureColorbuffer);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, display_size.x, display_size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, display_size.x, display_size.y);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    }
    
    glViewport(0, 0, display_size.x, display_size.y);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // make sure we clear the framebuffer's content
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    

    glUseProgram(programID);

    for(int i = 0; i < textureIDs.size(); i++){
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_BUFFER, textureIDs[i]);
    }

    // Ensure the uniform is set to the correct texture unit
    for(int i = 0; i < uniformiNames.size(); i++){
        GLuint uniformLocation = glGetUniformLocation(programID, uniformiNames[i].c_str());
        glUniform1i(uniformLocation, uniformiValues[i]);
    }
    for(int i = 0; i < uniformuiNames.size(); i++){
        GLuint uniformLocation = glGetUniformLocation(programID, uniformuiNames[i].c_str());
        glUniform1ui(uniformLocation, uniformuiValues[i]);
    }
    for(int i = 0; i < uniformfNames.size(); i++){
        GLuint uniformLocation = glGetUniformLocation(programID, uniformfNames[i].c_str());
        glUniform1f(uniformLocation, uniformfValues[i]);
    }


    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);


    // Bind and dispatch the accumulation compute shader
    glUseProgram(computeAccumProgramID);
    glBindImageTexture(0, textureColorbuffer, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(1, voxelColorAccumulationBuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute((GLuint)ceil(display_size.x / 8.0f), (GLuint)ceil(display_size.y / 8.0f), 1);

    // Ensure that writes to the image are complete before reading
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // Bind and dispatch the averaging compute shader
    glUseProgram(computeAvgProgramID);
    glBindImageTexture(0, voxelColorAccumulationBuffer, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(1, textureColorbuffer, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(2, outputColorBuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
    glDispatchCompute((GLuint)ceil(display_size.x / 8.0f), (GLuint)ceil(display_size.y / 8.0f), 1);

    // Ensure that writes to the image are complete before using the output
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);


    // now bind back to default framebuffer and draw a quad plane with the attached framebuffer color texture
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST); // disable depth test so screen-space quad isn't discarded due to depth test.

    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(postProgramID);

    {
        GLuint uniformLocation = glGetUniformLocation(postProgramID, "screenX");
        glUniform1i(uniformLocation, display_size.x);
    }
    {
        GLuint uniformLocation = glGetUniformLocation(postProgramID, "screenY");
        glUniform1i(uniformLocation, display_size.y);
    }

    glBindVertexArray(quadVAO);
    glBindTexture(GL_TEXTURE_2D, outputColorBuffer);	// use the color attachment texture as the texture of the quad plane
    glDrawArrays(GL_TRIANGLES, 0, 6);

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

    glDeleteBuffers(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);
    glDeleteTextures(1, &textureColorbuffer);
    glDeleteTextures(1, &voxelColorAccumulationBuffer);
    glDeleteTextures(1, &outputColorBuffer);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteFramebuffers(1, &framebuffer);

    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteProgram(programID);

    glDeleteShader(computeAccumShader);
    glDeleteShader(computeAvgShader);
    glDeleteProgram(computeAccumProgramID);
    glDeleteProgram(computeAvgProgramID);

    glDeleteShader(postVertex);
    glDeleteShader(postFragment);
    glDeleteProgram(postProgramID);

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