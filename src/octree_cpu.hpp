#pragma once

#include <math.h>
#include <vector>
#include <stack>
#include <functional>
#include "renderer/core.hpp"

class OctreeCPU {
public:
    struct Node {
        Node*     children[8] = {};
        uint8_t   childrenCount = 0;
        uint32_t  material = 0;
        // GPU sync metadata — managed by Octree, not touched by OctreeCPU logic.
        uint32_t  gpuBlock = UINT32_MAX;  // start of this node's 8-child block in the GPU buffer
        bool      dirty    = false;        // set on any modification; cleared by Octree::applyEdits
    };

    // Return type for raycast().
    struct RayHit {
        bool       hit      = false;
        Node*      node     = nullptr;   // leaf node (material != 0) on hit
        glm::uvec3 position = {};        // 1x1x1 voxel world position
    };

    OctreeCPU(uint8_t depth);
    ~OctreeCPU();

    RayHit raycast(glm::vec3 origin, glm::vec3 direction, uint32_t maxSteps = 300) const;

    void insert(glm::uvec3 position, uint32_t material);
    void remove(glm::uvec3 position);
    Node* lookup(glm::uvec3 position) const;
    void insertBox(glm::uvec3 min, glm::uvec3 max, uint32_t material);
    void insertSphere(glm::vec3 centre, float radius, uint32_t material);
    void insertFunction(std::function<bool(glm::vec3)> fn, glm::uvec3 min, glm::uvec3 max, uint32_t material);

    Node*   root  = nullptr;
    uint8_t depth = 0;

    // GPU blocks freed by remove() since the last Octree::applyEdits() call.
    // Octree reads this to reclaim free blocks without coupling OctreeCPU to GPU code.
    std::vector<uint32_t> freedGpuBlocks;

    friend class Octree;   // lets Octree consume freedGpuBlocks and clear dirty flags

private:
    static constexpr int maxDepth = 16;
    uint32_t utils_p2r[maxDepth + 1] = {};

    uint32_t locate(glm::uvec3 position, uint32_t level) const;

    void insertR(glm::uvec3 position, uint32_t material, Node*& node, uint8_t level);
    void removeR(glm::uvec3 position, Node*& node, uint8_t level);
    void freeSubtree(Node* node);
};