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

uint32_t Octree::lookup(glm::uvec3 position){
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
    if(position.x >= (1u << depth) || position.y >= (1u << depth) || position.z >= (1u << depth))
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

// ---------------------------------------------------------------------------
// set() — bulk-upload from an OctreeCPU in a single glBufferData call.
//
// Normal computation happens on the CPU (O(n) 6-neighbour lookups per dirty
// leaf).  For typical scene sizes this is well under 1 ms.  After that the
// pointer tree is linearised into the flat GPU layout with one iterative DFS,
// then uploaded in a single glBufferData call — no per-node GL calls at all.
// ---------------------------------------------------------------------------

void Octree::set(OctreeCPU* cpu) {
    if (!cpu || !cpu->root) return;

    // 1. CPU normal computation for any dirty leaves.
    cpu->computeNormals();

    // 2. Reset GPU-side state (keep capacity at current level to avoid
    //    needless reallocs on repeated set() calls with similar scene sizes).
    Node zero; zero.raw = 0;
    uint32_t prevCapacity = capacity;
    data.assign(prevCapacity, zero);
    size      = 8;
    numVoxels = 0;
    while (!freeNodes.empty()) freeNodes.pop();

    // 3. Linearise: root's 8 children map directly to GPU slots 0-7.
    for (int i = 0; i < 8; i++)
        linearizeNode(cpu, cpu->root->children[i], (uint32_t)i, 2);

    // 4. Single upload.
    glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
    glBufferData(GL_TEXTURE_BUFFER, size * 4, data.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    // Rebind the texture buffer so the sampler sees the new allocation.
    glBindTexture(GL_TEXTURE_BUFFER, texBufferID);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, gl_ID);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

// Iterative DFS — avoids stack-overflow on deep/dense trees.
void Octree::linearizeNode(OctreeCPU* cpu, OctreeCPU::Node* cpuNode,
                            uint32_t gpuOffset, uint8_t level) {
    using Entry = std::tuple<OctreeCPU::Node*, uint32_t, uint8_t>;
    std::stack<Entry> stk;
    stk.push(std::make_tuple(cpuNode, gpuOffset, level));

    while (!stk.empty()) {
        OctreeCPU::Node* cn = std::get<0>(stk.top());
        uint32_t         off = std::get<1>(stk.top());
        uint8_t          lv  = std::get<2>(stk.top());
        stk.pop();

        if (!cn) continue;  // empty slot — data[off] stays zero (no material)

        if (lv > cpu->depth) {
            // Leaf — this node IS the individual voxel.
            if (cn->material == 0) continue;  // explicitly cleared voxel, skip
            resizeDataIfNeeded(off + 1);
            Node leaf; leaf.raw = 0;
            leaf.leaf.isNode   = 0;
            leaf.leaf.material = cn->material & 0x7F;
            leaf.leaf.normal   = packedNormal(cn->normal);
            data[off] = leaf;
            numVoxels++;
        } else {
            // Internal — allocate a fresh block of 8 for the children.
            resizeDataIfNeeded(off + 1);
            uint32_t block = size;
            size += 8;
            resizeDataIfNeeded(size);

            Node internal; internal.raw = 0;
            internal.node.isNode = 1;
            internal.node.next   = block;
            internal.node.count  = cn->childrenCount;
            data[off] = internal;

            for (int i = 0; i < 8; i++)
                stk.push(std::make_tuple(cn->children[i], block + (uint32_t)i, (uint8_t)(lv + 1)));
        }
    }
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