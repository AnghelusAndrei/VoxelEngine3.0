#include "octree.hpp"
#include "iostream"

Octree::Octree(Config *config){
    depth = config->depth > maxDepth ? maxDepth : config->depth;

    delete config;

    capacity = 8;

    uint32_t p2r = 1;
    for (int i = depth; i >= 1; i--)
    {
        utils_p2r[i] = p2r;
        p2r <<= 1;
    }

    data.resize(capacity);
}

void Octree::setProgram(GLuint program_){
    program = program_;
}

void Octree::GenUBO(GLuint program_){
    program = program_;
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
    glBufferData(GL_TEXTURE_BUFFER, capacity * 4, NULL, GL_DYNAMIC_DRAW);

    // Generate and bind the texture object
    glGenTextures(1, &texBufferID);
    glBindTexture(GL_TEXTURE_BUFFER, texBufferID);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, gl_ID);  // Assuming GL_R32UI as the internal format

    // Unbind the buffer and texture
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

void Octree::freeVRAM(){
    glDeleteBuffers(1, &gl_ID);
    glDeleteTextures(1, &texBufferID);
}

void Octree::BindUniforms(uint8_t &texturesBound){
    glActiveTexture(GL_TEXTURE0 + texturesBound);
    glBindTexture(GL_TEXTURE_BUFFER, texBufferID);

    GLint texLoc = glGetUniformLocation(program, "octreeTexture");
    GLint depthLoc = glGetUniformLocation(program, "octreeDepth");

    glUniform1i(texLoc, (int)texturesBound);
    glUniform1ui(depthLoc, (int)depth);
    texturesBound++;
}

Octree::~Octree(){
    freeVRAM();
}

void Octree::Update(){
    glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
    glBufferData(GL_TEXTURE_BUFFER, size * 4, data.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void Octree::UpdateNode(uint32_t index){
    glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
    glBufferSubData(GL_TEXTURE_BUFFER, index * 4, 4, &data[index].raw);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

uint32_t  Octree::lookup(glm::uvec3 position){
    uint32_t offset = 0;
    for (int depth_ = 1; depth_ <= depth; depth_++)
    {
        offset += locate(position, depth_);
        Node node = data[offset];
        if (!node.base.isNode)
            return offset;
        offset = node.node.next;
    }
}

void Octree::resizeDataIfNeeded(uint32_t requiredCapacity) {
    if (requiredCapacity > capacity) {
        // Double the capacity until it is larger than the required capacity
        while (capacity < requiredCapacity) {
            capacity *= 2;
        }

        Octree::Node newNode;
        newNode.raw = 0;

        data.resize(capacity, newNode);
    
        // Update UBO to reflect new capacity
        glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
        glBufferData(GL_TEXTURE_BUFFER, capacity * 4, data.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);
    }
}

void Octree::insert(glm::uvec3 position, Node leaf){
    if(position.x > (2 << depth) || position.y > (2 << depth) || position.z > (2 << depth))
        return;
    uint32_t offset = 0;
    uint32_t lastNode = 0;
    leaf.base.isNode=false;
    for (int depth_ = 1; depth_ < depth; depth_++)
    {
        offset += locate(position, depth_);
        Node node = data[offset];

        if(!node.base.isNode){
            uint32_t nextOffset;
            if(freeNodes.empty()){
                resizeDataIfNeeded(size+16);
                nextOffset = size;
                size+=8;
            }else{
                nextOffset = freeNodes.top();
                freeNodes.pop();
            }
            Node node;
            node.base.isNode = 1;
            node.node.next = nextOffset;
            node.node.count = 0;
            data[offset] = node;
            if(depth_>1)data[lastNode].node.count++;
            UpdateNode(lastNode);
            UpdateNode(offset);
            lastNode = offset;
            offset = nextOffset;
        }else{
            lastNode = offset;
            offset = node.node.next;
        }
    }

    int i = offset + locate(position, depth);
    if(data[i].leaf.material == 0)data[lastNode].node.count++;
    data[i] = leaf;
    numVoxels++;
    UpdateNode(lastNode);
    UpdateNode(i);
}

void Octree::remove(glm::uvec3 position){

}

uint32_t Octree::locate(glm::uvec3 position, uint32_t depth_){
    return (((bool)(position.x & utils_p2r[depth_])) << 2) | ((bool)((position.y & utils_p2r[depth_])) << 1) |((bool)(position.z & utils_p2r[depth_]));
}

bool Octree::contained(glm::uvec3 position1, glm::uvec3 position2, uint32_t depth_){
     return ((position1.x >> depth_ == position2.x >> depth_) && (position1.y >> depth_ == position2.y >> depth_) && (position1.z >> depth_ == position2.z >> depth_));
}

uint32_t Octree::packedNormal(glm::vec3& normal){
    // Convert normal vector components to 8-bit signed integers
    glm::i8vec3 si = glm::i8vec3(
        static_cast<int8_t>((normal.x + 1) * 127.0f),
        static_cast<int8_t>((normal.y + 1) * 127.0f),
        static_cast<int8_t>((normal.z + 1) * 127.0f)
    );

    // Pack the 8-bit integers into the 24-bit normal field
    return ((uint32_t(si.x) & 0xFF) << 16) |
            ((uint32_t(si.y) & 0xFF) << 8) |
            (uint32_t(si.z) & 0xFF);
}