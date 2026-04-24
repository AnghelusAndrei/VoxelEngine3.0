#pragma once

#include "core.hpp"
#include "../octree_cpu.hpp"

#include <stack>
#include <tuple>
#include <functional>
#include <cstdlib>

#define maxDepth 16

class Renderer;
class Octree{
    public:
        struct NodeBase {
            unsigned isNode : 1;
        };

        struct NodeData {
            unsigned isNode : 1;
            unsigned count : 3;
            unsigned next : 28;
        };

        struct LeafData {
            unsigned isNode : 1;
            unsigned material : 7;
            unsigned normal : 24;
        };

        union Node {
            NodeBase base;
            NodeData node;
            LeafData leaf;
            uint32_t raw;
        };

        std::vector<Node> data;
    public:
        struct Config{
            uint8_t depth;
        };

        Octree(Config *config);
        ~Octree();
        void Update();

        uint32_t lookup(glm::uvec3 position);
        void set(OctreeCPU *cpuOctree);

        // Incremental GPU sync — only uploads nodes that changed since the last
        // set() or applyEdits() call.  Call once after any batch of CPU edits.
        // O(changed_nodes) instead of O(total_nodes) like set().
        void applyEdits(OctreeCPU *cpu);

        // Returns the GPU buffer slot of the leaf at `pos`, or UINT32_MAX if empty.
        // Used to build the voxelID list for targeted nBuffer invalidation.
        uint32_t findGpuSlot(OctreeCPU *cpu, glm::uvec3 pos);

        // Bind the octree TBO for the accum compute pass so it can traverse
        // the tree for neighbourhood occupancy lookups during normal computation.
        // Must be called after glUseProgram(accumPassProgram).
        void BindForAccumPass(GLuint accumProgram);

        uint8_t depth;
        uint32_t capacity;
        uint32_t size = 8;
        uint32_t numVoxels = 0;

        friend class Renderer;
    private:
        GLuint gl_ID;
        GLuint program;
        GLuint texBufferID;
        GLuint depthUniformLocation;
        GLint maxTextureSize;  // max texture buffer size from GPU
        uint32_t gpuBufferSize = 0;  // actual size of GPU buffer in slots

        std::stack<uint32_t> freeBlocks;  // stack of recyclable 8-slot GPU blocks

        void applyEditsNode(OctreeCPU* cpu, OctreeCPU::Node* node,
                            uint32_t gpuSlot, uint8_t level);

        void setProgram(GLuint program_);
        void GenUBO(GLuint program_);
        void freeVRAM();
        void BindUniforms(uint8_t &texturesBound);
        void UpdateNode(uint32_t index);
        void resizeDataIfNeeded(uint32_t requiredCapacity);

        void linearizeNode(OctreeCPU* cpu, OctreeCPU::Node* cpuNode,
                           uint32_t gpuOffset, uint8_t level);

        uint32_t utils_p2r[maxDepth];
        uint32_t locate(glm::uvec3 position, uint32_t depth_);
        bool contained(glm::uvec3 position1, glm::uvec3 position2, uint32_t depth_);
};
