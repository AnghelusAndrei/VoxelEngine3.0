#pragma once

#include "core.hpp"

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
        
        void GenUBO(GLuint program_);
        void BindUniforms(GLuint program_, uint8_t &texturesBound);

    private:
        std::string fileNames[6];
        GLuint gl_ID;
        GLuint program;
        
        struct BMPHeader {
            uint32_t width;
            uint32_t height;
            uint32_t dataOffset;
        };
        
        bool loadBMP(const std::string& filename, unsigned char** data, uint32_t& width, uint32_t& height);
        void freeBMPData(unsigned char* data);
        
        friend class Renderer;
};