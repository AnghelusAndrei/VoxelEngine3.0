#include "skybox.hpp"
#include <fstream>
#include <iostream>

bool Skybox::loadBMP(const std::string& filename, unsigned char** data, uint32_t& width, uint32_t& height) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open BMP file: " << filename << std::endl;
        return false;
    }

    // BMP file header (14 bytes)
    char signature[2];
    file.read(signature, 2);
    if (signature[0] != 'B' || signature[1] != 'M') {
        std::cerr << "Not a valid BMP file: " << filename << std::endl;
        return false;
    }

    file.seekg(18); // Skip to width offset

    // Read width and height (4 bytes each, little-endian)
    uint32_t w, h;
    file.read(reinterpret_cast<char*>(&w), 4);
    file.read(reinterpret_cast<char*>(&h), 4);

    width = w;
    height = h;

    file.seekg(28); // Go to bits per pixel
    uint16_t bitsPerPixel;
    file.read(reinterpret_cast<char*>(&bitsPerPixel), 2);

    if (bitsPerPixel != 24 && bitsPerPixel != 32) {
        std::cerr << "Only 24-bit and 32-bit BMP files are supported: " << filename << std::endl;
        return false;
    }

    uint32_t bytesPerPixel = bitsPerPixel / 8;
    
    // Go to data offset
    file.seekg(10);
    uint32_t dataOffset;
    file.read(reinterpret_cast<char*>(&dataOffset), 4);

    file.seekg(dataOffset);

    uint32_t imageSize = width * height * 3; // Always convert to RGB
    *data = new unsigned char[imageSize];

    // Read pixel data (BMP is bottom-up, but we'll read as-is)
    unsigned char* tempData = new unsigned char[width * height * bytesPerPixel];
    file.read(reinterpret_cast<char*>(tempData), width * height * bytesPerPixel);

    // Convert to RGB (BMP is BGR)
    for (uint32_t i = 0; i < width * height; ++i) {
        (*data)[i * 3 + 0] = tempData[i * bytesPerPixel + 2]; // R
        (*data)[i * 3 + 1] = tempData[i * bytesPerPixel + 1]; // G
        (*data)[i * 3 + 2] = tempData[i * bytesPerPixel + 0]; // B
    }

    delete[] tempData;
    file.close();
    return true;
}

void Skybox::freeBMPData(unsigned char* data) {
    delete[] data;
}

Skybox::Skybox( const std::string& Directory,
                const std::string& PosXFilename,
                const std::string& NegXFilename,
                const std::string& PosYFilename,
                const std::string& NegYFilename,
                const std::string& PosZFilename,
                const std::string& NegZFilename) : gl_ID(0), program(0) {
    
    fileNames[0] = Directory + "/" + PosXFilename;   // +X
    fileNames[1] = Directory + "/" + NegXFilename;   // -X
    fileNames[2] = Directory + "/" + PosYFilename;   // +Y
    fileNames[3] = Directory + "/" + NegYFilename;   // -Y
    fileNames[4] = Directory + "/" + PosZFilename;   // +Z
    fileNames[5] = Directory + "/" + NegZFilename;   // -Z

    // Create cubemap texture
    glGenTextures(1, &gl_ID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gl_ID);

    // Load all 6 faces
    GLenum targets[] = {
        GL_TEXTURE_CUBE_MAP_POSITIVE_X,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
    };

    uint32_t width, height;
    bool success = true;

    for (int i = 0; i < 6; ++i) {
        unsigned char* data = nullptr;
        if (!loadBMP(fileNames[i], &data, width, height)) {
            std::cerr << "Failed to load skybox face: " << fileNames[i] << std::endl;
            success = false;
            continue;
        }

        glTexImage2D(targets[i], 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        freeBMPData(data);
    }

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

    if (!success) {
        std::cerr << "Warning: Some skybox faces failed to load" << std::endl;
    }
}

void Skybox::GenUBO(GLuint program_) {
    program = program_;
}

void Skybox::BindUniforms(GLuint program_, uint8_t &texturesBound) {
    glActiveTexture(GL_TEXTURE0 + texturesBound);
    glBindTexture(GL_TEXTURE_CUBE_MAP, gl_ID);
    
    GLint texLoc = glGetUniformLocation(program_, "skybox");
    glUniform1i(texLoc, (int)texturesBound);
    
    texturesBound++;
}

Skybox::~Skybox() {
    if (gl_ID != 0) {
        glDeleteTextures(1, &gl_ID);
    }
}