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
        glm::vec3 normal = glm::vec3(0.0f);
        bool      normalDirty = true;   // recompute on next set()
    };

    OctreeCPU(uint8_t depth);
    ~OctreeCPU();

    // Insert a single voxel.
    // Pass a non-zero normal to pin it (analytical geometry) — normalDirty stays false.
    // Leave normal at vec3(0) to have computeNormals() fill it in automatically.
    void insert(glm::uvec3 position, uint32_t material,
                glm::vec3 normal = glm::vec3(0.0f));

    void remove(glm::uvec3 position);

    // Returns the leaf Node* at position, or nullptr if absent / out of bounds.
    Node* lookup(glm::uvec3 position) const;

    // Fill an axis-aligned box [min, max) with a material.
    void insertBox(glm::uvec3 min, glm::uvec3 max, uint32_t material);

    // Fill all voxels whose centre falls within `radius` of `centre`.
    void insertSphere(glm::vec3 centre, float radius, uint32_t material);

    // Fill every voxel in [min, max) for which fn(vec3 voxel_centre) returns true.
    void insertFunction(std::function<bool(glm::vec3)> fn,
                        glm::uvec3 min, glm::uvec3 max,
                        uint32_t material);

    // (Re)compute normals for every leaf whose normalDirty flag is set.
    // Samples a (2*radius+1)^3 neighbourhood: for each sample position that is
    // empty, its offset from the voxel centre is accumulated as a normal
    // contribution. radius=1 gives the 6-face result; radius=3 matches the
    // quality of the original hand-written scene code.
    void computeNormals(int radius = 3);

    Node*   root  = nullptr;
    uint8_t depth = 0;

private:
    static constexpr int maxDepth = 16;
    uint32_t utils_p2r[maxDepth + 1] = {};

    uint32_t locate(glm::uvec3 position, uint32_t level) const;

    // Recursive helpers — node passed by reference so allocations propagate up.
    void insertR(glm::uvec3 position, uint32_t material, glm::vec3 normal,
                 Node*& node, uint8_t level);
    void removeR(glm::uvec3 position, Node*& node, uint8_t level);

    // DFS normal computation pass.
    void computeNormalsR(Node* node, glm::uvec3 origin, uint8_t level);

    void freeSubtree(Node* node);

    int _normalRadius = 3;  // set by computeNormals(), used by computeNormalsR()
};
