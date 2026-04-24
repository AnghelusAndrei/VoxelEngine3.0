#include "octree.hpp"
#include "iostream"

Octree::Octree(Config *config){
    depth = config->depth > maxDepth ? maxDepth : config->depth;

    delete config;

    capacity = 8;
    
    // Query the maximum texture buffer size from GPU
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTextureSize);

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
    glBufferData(GL_TEXTURE_BUFFER, capacity * 4, data.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    gpuBufferSize = capacity;  // Track GPU buffer size
}

void Octree::UpdateNode(uint32_t index){
    // Ensure GPU buffer is large enough for this slot
    if (index >= gpuBufferSize) {
        resizeDataIfNeeded(index + 1);
    }
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
        
        // Clamp capacity to GPU's maximum texture buffer size
        if (capacity > (uint32_t)maxTextureSize) {
            capacity = (uint32_t)maxTextureSize;
        }

        Octree::Node newNode;
        newNode.raw = 0;

        data.resize(capacity, newNode);
    
        // Update GPU buffer to reflect new capacity
        glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
        glBufferData(GL_TEXTURE_BUFFER, capacity * 4, data.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);
        
        gpuBufferSize = capacity;  // Track GPU buffer size
    }
}

// ---------------------------------------------------------------------------
// set() — full bulk-upload from an OctreeCPU.  Use for initial scene load or
// after large structural changes.  For incremental edits (insert/remove),
// prefer applyEdits() which is O(changed_nodes) instead of O(total_nodes).
// ---------------------------------------------------------------------------

void Octree::set(OctreeCPU* cpu) {
    if (!cpu || !cpu->root) return;

    // Reset all bookkeeping — full rebuild discards any pending incremental state.
    Node zero; zero.raw = 0;
    data.assign(capacity, zero);
    size      = 9;      // slot 0 = root, slots 1-8 = root's 8 children
    numVoxels = 0;
    while (!freeBlocks.empty()) freeBlocks.pop();
    cpu->freedGpuBlocks.clear();
    resizeDataIfNeeded(size);
    gpuBufferSize = capacity;  // Sync GPU buffer size

    // Slot 0: explicit root node.
    // The GPU shader at depth=0 always reads slot 0 (locate(ur, 2^depth) is
    // always 0 for any position inside [0,2^depth)).  By placing the root here
    // with next=1, depth=1 correctly selects one of the 8 real octants.
    cpu->root->gpuBlock = 1;
    cpu->root->dirty    = false;
    Node root_gpu; root_gpu.raw = 0;
    root_gpu.node.isNode = 1;
    root_gpu.node.next   = 1;
    root_gpu.node.count  = cpu->root->childrenCount;
    data[0] = root_gpu;

    // Slots 1-8: root's 8 children (one per octant of [0,2^depth)^3).
    for (int i = 0; i < 8; i++)
        linearizeNode(cpu, cpu->root->children[i], uint32_t(i + 1), 2);

    // Single bulk upload.
    glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
    glBufferData(GL_TEXTURE_BUFFER, capacity * 4, data.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    gpuBufferSize = capacity;  // Track GPU buffer size

    // Rebind so the sampler sees the (possibly reallocated) buffer.
    glBindTexture(GL_TEXTURE_BUFFER, texBufferID);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, gl_ID);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
}

// Iterative DFS that both linearises nodes into the GPU buffer and stamps
// each CPU node with its gpuBlock (= where its children live in the buffer).
void Octree::linearizeNode(OctreeCPU* cpu, OctreeCPU::Node* cpuNode,
                            uint32_t gpuOffset, uint8_t level) {
    using Entry = std::tuple<OctreeCPU::Node*, uint32_t, uint8_t>;
    std::stack<Entry> stk;
    stk.push(std::make_tuple(cpuNode, gpuOffset, level));

    while (!stk.empty()) {
        OctreeCPU::Node* cn  = std::get<0>(stk.top());
        uint32_t         off = std::get<1>(stk.top());
        uint8_t          lv  = std::get<2>(stk.top());
        stk.pop();

        if (!cn) continue;  // empty slot — data[off] stays zero

        cn->dirty = false;  // this node is now in sync with the GPU

        if (lv > cpu->depth || (cn->material != 0 && cn->childrenCount == 0)) {
            // Leaf: either a true depth-limit voxel, or a compressed leaf where the
            // entire region is one material.  Writes a GPU leaf so the ray traversal
            // short-circuits here (potentially representing a large block, not just 1×1×1).
            if (cn->material == 0) continue;
            resizeDataIfNeeded(off + 1);
            Node leaf; leaf.raw = 0;
            leaf.leaf.isNode   = 0;
            leaf.leaf.material = cn->material & 0x7F;
            leaf.leaf.normal   = 0;
            data[off] = leaf;
            numVoxels++;
        } else {
            // Internal node: allocate a fresh 8-slot block for children.
            resizeDataIfNeeded(off + 1);
            uint32_t block = size;
            size += 8;
            resizeDataIfNeeded(size);

            cn->gpuBlock = block;  // record where children live for applyEdits

            Node internal; internal.raw = 0;
            internal.node.isNode = 1;
            internal.node.next   = block;
            internal.node.count  = cn->childrenCount;
            data[off] = internal;

            for (int i = 0; i < 8; i++)
                stk.push(std::make_tuple(cn->children[i], block + uint32_t(i), uint8_t(lv + 1)));
        }
    }
}

// ---------------------------------------------------------------------------
// applyEdits() — incremental GPU sync after CPU edits.
//
// Only visits nodes that have dirty=true (set by insertR/removeR).  Allocates
// new 8-slot blocks for new internal nodes, reclaims freed blocks from
// cpu->freedGpuBlocks into freeBlocks, and issues glBufferSubData for only
// the changed slots.  O(changed_nodes) per call.
// ---------------------------------------------------------------------------

void Octree::applyEdits(OctreeCPU* cpu) {
    if (!cpu || !cpu->root) return;

    // Reclaim blocks freed by cpu->remove() into our free-block pool.
    for (uint32_t b : cpu->freedGpuBlocks) freeBlocks.push(b);
    cpu->freedGpuBlocks.clear();

    if (!cpu->root->dirty) return;

    // Update root (slot 0) — its count may have changed.
    Node root_gpu; root_gpu.raw = 0;
    root_gpu.node.isNode = 1;
    root_gpu.node.next   = 1;
    root_gpu.node.count  = cpu->root->childrenCount;
    data[0] = root_gpu;
    UpdateNode(0);
    cpu->root->dirty = false;

    // Process each of root's 8 children (GPU slots 1-8).
    for (int i = 0; i < 8; i++)
        applyEditsNode(cpu, cpu->root->children[i], uint32_t(i + 1), 2);
}

void Octree::applyEditsNode(OctreeCPU* cpu, OctreeCPU::Node* node,
                             uint32_t gpuSlot, uint8_t level) {
    if (!node) {
        // Child was removed — zero the GPU slot so the shader sees an empty voxel.
        if (data[gpuSlot].raw != 0) {
            data[gpuSlot].raw = 0;
            UpdateNode(gpuSlot);
        }
        return;
    }
    if (!node->dirty) return;  // subtree unchanged — skip
    node->dirty = false;

    if (level > cpu->depth || (node->material != 0 && node->childrenCount == 0)) {
        // Leaf: true depth-limit voxel or compressed leaf (entire region = one material).
        Node leaf; leaf.raw = 0;
        leaf.leaf.material = node->material & 0x7F;
        leaf.leaf.normal   = 0;
        data[gpuSlot] = leaf;
        UpdateNode(gpuSlot);
        if (node->material != 0) numVoxels++;
        // Release any GPU block that was previously allocated for this slot
        // (happens when a subtree is overwritten by a compressed leaf).
        if (node->gpuBlock != UINT32_MAX) {
            freeBlocks.push(node->gpuBlock);
            node->gpuBlock = UINT32_MAX;
        }
        return;
    }

    // Internal node: ensure a children block is allocated.
    if (node->gpuBlock == UINT32_MAX) {
        uint32_t block;
        if (!freeBlocks.empty()) {
            block = freeBlocks.top(); freeBlocks.pop();
            // Zero the recycled block so orphaned stale data doesn't leak through.
            for (int i = 0; i < 8; i++) data[block + i].raw = 0;
            glBindBuffer(GL_TEXTURE_BUFFER, gl_ID);
            glBufferSubData(GL_TEXTURE_BUFFER, GLintptr(block) * 4, 8 * 4, &data[block]);
            glBindBuffer(GL_TEXTURE_BUFFER, 0);
        } else {
            // Check if we have room to allocate a new block
            if (size + 8 > (uint32_t)maxTextureSize) {
                // Cannot allocate — texture buffer would exceed GPU limit
                return;
            }
            block = size; size += 8;
            resizeDataIfNeeded(size);
        }
        node->gpuBlock = block;
    }

    // Write this node's own slot.
    Node internal; internal.raw = 0;
    internal.node.isNode = 1;
    internal.node.next   = node->gpuBlock;
    internal.node.count  = node->childrenCount;
    data[gpuSlot] = internal;
    UpdateNode(gpuSlot);

    // Recurse into the 8 children, visiting all when the parent is dirty.
    for (int i = 0; i < 8; i++)
        applyEditsNode(cpu, node->children[i], node->gpuBlock + uint32_t(i), level + 1);
}

// ---------------------------------------------------------------------------
// findGpuSlot() — CPU-tree traversal using stored gpuBlock pointers.
// Returns the GPU buffer slot of the leaf at `pos`, or UINT32_MAX if absent.
// Used to collect voxelIDs for targeted nBuffer invalidation after edits.
// ---------------------------------------------------------------------------

uint32_t Octree::findGpuSlot(OctreeCPU* cpu, glm::uvec3 pos) {
    if (!cpu || !cpu->root) return UINT32_MAX;

    OctreeCPU::Node* node  = cpu->root;
    uint32_t         block = 1u;  // root's children are at GPU slots 1-8

    for (uint8_t level = 1; level <= depth; level++) {
        uint32_t          idx   = locate(pos, level);
        uint32_t          slot  = block + idx;
        OctreeCPU::Node*  child = node->children[idx];

        if (!child) return UINT32_MAX;
        if (level == depth) return slot;   // leaf

        if (child->gpuBlock == UINT32_MAX) return UINT32_MAX;
        block = child->gpuBlock;
        node  = child;
    }
    return UINT32_MAX;
}

uint32_t Octree::locate(glm::uvec3 position, uint32_t depth_){
    return (((bool)(position.x & utils_p2r[depth_])) << 2) | ((bool)((position.y & utils_p2r[depth_])) << 1) |((bool)(position.z & utils_p2r[depth_]));
}

bool Octree::contained(glm::uvec3 position1, glm::uvec3 position2, uint32_t depth_){
     return ((position1.x >> depth_ == position2.x >> depth_) && (position1.y >> depth_ == position2.y >> depth_) && (position1.z >> depth_ == position2.z >> depth_));
}

void Octree::BindForAccumPass(GLuint accumProgram) {
    // Bind the octree TBO to texture unit 0 and set uniforms on the
    // accumulate compute program so it can traverse the octree for
    // normal computation.  Must be called after glUseProgram(accumProgram).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_BUFFER, texBufferID);
    GLint texLoc   = glGetUniformLocation(accumProgram, "octreeTexture");
    GLint depthLoc = glGetUniformLocation(accumProgram, "octreeDepth");
    glUniform1i(texLoc, 0);
    glUniform1ui(depthLoc, (GLuint)depth);
}


