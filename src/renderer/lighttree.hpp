#pragma once

#include <glad/glad.h>
#include <glm/vec3.hpp>
#include <vector>
#include <stack>
#include <cstdint>

class Octree;
class MaterialPool;

// Sparse, emissive-only SVO maintained as a true MIRROR of the octree: every emissive voxel
// insert/remove is an O(depth) edit on this tree (allocBlock/freeBlock + power propagation),
// exactly like the octree's own insert/remove + majority-material propagation. The octree
// fires a generic per-voxel `onLeafChanged` hook; this class (which has the material pool)
// applies the emissive policy. Uploaded incrementally by dirty range, like
// Octree::flushEdits. Sampled by descent on the GPU (src/shd/lighttree.glsl). SSBO binding 5.
// See architecture/LIGHTTREE.md.
//
// GPU node = 3 DWORDs (12 B), mirroring the octree's lo/hi packing + a power word:
//   [0] power     (float bits)                                   — CDF weight (full precision)
//   [1] childBase (node-index of the 8-node child block; 0)      — like octree `next`/hi
//   [2] isNode:1 | material:7 | childMask:8 | reserved:16        — like octree `lo`
// Position / level / area are NOT stored — reconstructed by accumulating octant offsets
// during descent (exactly how the octree DDA derives position).
class LightTree {
public:
    static constexpr uint32_t NODE_WORDS = 3;

    LightTree(Octree* octree, MaterialPool* materials);   // inits the CPU root (no GL)
    ~LightTree();

    void attach();        // register octree->onLeafChanged → emissive insert/remove
    void GenSSBO();       // create the SSBO (binding 5) + upload the current tree
    void flushEdits();    // push the dirty node range to GPU (call after a batch of edits)
    uint32_t nodeCount() const { return size; }

    // Octree's per-voxel hook. Applies emissive policy: emissive → insert/update, ex-emissive → remove.
    void onOctreeLeaf(glm::uvec3 pos, uint32_t level, uint32_t oldMat, uint32_t newMat);

private:
    // ---- node word access (word 2 = isNode:1 | material:7 | childMask:8 | reserved:16) ----
    float    getPower(uint32_t n) const;
    void     setPower(uint32_t n, float p);
    uint32_t childBase(uint32_t n)  const { return data[n*NODE_WORDS + 1]; }
    uint32_t childMask(uint32_t n)  const { return (data[n*NODE_WORDS + 2] >> 8) & 0xFFu; }
    bool     isInternal(uint32_t n) const { return (data[n*NODE_WORDS + 2] & 1u) != 0u; }   // isNode bit
    bool     isEmpty(uint32_t n)    const {
        uint32_t b = n*NODE_WORDS; return data[b]==0u && data[b+1]==0u && data[b+2]==0u;
    }
    // openFaces (0..6): a leaf's count of air face-neighbours, in word2 reserved bits [16:18].
    // A voxel with 0 (fully enclosed) radiates nothing externally and is NOT a light (surface-cull).
    uint32_t openFaces(uint32_t n)  const { return (data[n*NODE_WORDS + 2] >> 16) & 0x7u; }
    void     setOpenFaces(uint32_t n, uint32_t of){
        uint32_t& w = data[n*NODE_WORDS + 2];
        w = (w & ~(0x7u << 16)) | ((of & 0x7u) << 16);
        markDirty(n);
    }
    static constexpr uint32_t NO_NODE = 0xFFFFFFFFu;

    void setInternal(uint32_t slot, uint32_t childBlock);
    void setLeaf(uint32_t slot, uint32_t material, float power, uint32_t openFaces);
    void clearNode(uint32_t slot);
    void setChildBit(uint32_t slot, uint32_t octant);
    void clearChildBit(uint32_t slot, uint32_t octant);

    uint32_t octantOf(glm::uvec3 pos, uint32_t lev) const;
    uint32_t allocBlock();
    void     freeBlock(uint32_t block);
    void     freeSubtree(uint32_t block);
    void     markDirty(uint32_t node, uint32_t count = 1);
    bool     recomputePower(uint32_t slot);    // returns whether the node's power changed
    void     propagatePowerUp(const uint32_t* pathNode, int fromLevel);

    void     lightInsert(glm::uvec3 pos, uint32_t level, uint32_t material, uint32_t openFaces);
    void     lightRemoveSubtree(glm::uvec3 pos, uint32_t level);   // remove the node at (pos,level) + its subtree
    float    lightPower(uint32_t materialId, uint32_t voxelSize) const;

    // ---- Surface-cull (enclosed-voxel exclusion) — O(depth) per edit, full-depth voxels ----
    // architecture/LIGHTTREE.md §Surface-cull. The octree is queried for neighbour occupancy.
    uint32_t materialAt(glm::uvec3 p) const;                 // 0 = air/void; handles coarse leaves
    uint32_t countOpenFaces(glm::uvec3 pos) const;           // # of the 6 face-neighbours that are air
    uint32_t lightLeafSlot(glm::uvec3 pos) const;            // full-depth light leaf at pos, or NO_NODE
    void     updateNeighbourOpenFaces(glm::uvec3 pos, bool nowSolid);  // ±1 a face on each emissive neighbour

    Octree*       octree;
    MaterialPool* materials;
    GLuint        ssbo = 0;
    std::vector<uint32_t> data;          // flat node array (NODE_WORDS per node), CPU mirror
    uint32_t      size     = 0;          // next free node index (multiple of 8)
    uint32_t      gpuCapacity = 0;       // GPU node capacity (grows geometrically; avoids per-frame realloc)
    std::stack<uint32_t> freeBlocks;     // recycled 8-node blocks
    uint32_t      dirtyMin = 0xFFFFFFFFu, dirtyMax = 0;   // half-open node range [min,max)
};
