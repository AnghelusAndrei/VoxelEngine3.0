#pragma once

#include "renderer.hpp"

class Skybox{
    public:
        Skybox( const std::string& Directory,
                const std::string& PosXFilename,
                const std::string& NegXFilename,
                const std::string& PosYFilename,
                const std::string& NegYFilename,
                const std::string& PosZFilename,
                const std::string& NegZFilename);
        
        ~Skybox();

    private:
        std::string fileNames[6];
        GLuint gl_ID;
};