#pragma once

#include "core.hpp"

#include <stack>
#include <deque>
#include <tuple>
#include <functional>
#include <cstdlib>

#define maxDepth 11

class Renderer;

class Octree {
public:
    // 64-bit GPU node, stored AoS as two 32-bit words (lo, hi). Eight nodes form
    // one 8-slot block = 64 B = exactly one L2 cache line (blocks are 8-slot
    // aligned — see allocBlock / the constructor).
    //
    //   Lo word: [ isNode:1 | material:7 | childmask:8 | version:8 | reserved:8 ]
    //   Hi word: [ next:32 ]

    struct Node {
        uint32_t lo = 0;
        uint32_t hi = 0;

        static constexpr uint32_t ISNODE_BIT = 1u;       // lo bit 0
        static constexpr uint32_t MAT_SHIFT  = 1u;       // lo bits 1..7
        static constexpr uint32_t MAT_MASK   = 0x7Fu;
        static constexpr uint32_t CM_SHIFT   = 8u;       // lo bits 8..15
        static constexpr uint32_t CM_MASK    = 0xFFu;
        static constexpr uint32_t VER_SHIFT  = 16u;      // lo bits 16..23
        static constexpr uint32_t VER_MASK   = 0xFFu;

        bool     isNode()    const { return (lo & ISNODE_BIT) != 0u; }
        bool     empty()     const { return lo == 0u && hi == 0u; }
        uint32_t material()  const { return (lo >> MAT_SHIFT) & MAT_MASK; }
        uint32_t childmask() const { return (lo >> CM_SHIFT)  & CM_MASK; }
        uint32_t version()   const { return (lo >> VER_SHIFT) & VER_MASK; }
        uint32_t next()      const { return hi; }

        void makeInternal(uint32_t childBlock) { lo = ISNODE_BIT; hi = childBlock; }
        void makeLeaf(uint32_t mat, uint32_t ver) {
            lo = ((mat & MAT_MASK) << MAT_SHIFT) | ((ver & VER_MASK) << VER_SHIFT);
            hi = 0u;
        }
        void clear()                           { lo = 0u; hi = 0u; }

        void setMaterial(uint32_t mat) {
            lo = (lo & ~(MAT_MASK << MAT_SHIFT)) | ((mat & MAT_MASK) << MAT_SHIFT);
        }
    };

    struct Config { uint8_t depth; };

    // Result of raycast() — replaces OctreeCPU::RayHit.
    struct RayHit {
        bool       hit      = false;
        uint32_t   material = 0;        // 0 if no hit
        uint32_t   slot     = UINT32_MAX; // GPU slot of the leaf
        glm::uvec3 position = {};
    };

    Octree(Config* config);
    ~Octree();


    void     setProgram(GLuint program_);
    void     GenUBO();
    void     freeVRAM();
    void     BindUniforms(uint8_t& texturesBound);
    // Zero the claim bitfield — call once per frame before primary.comp's dedup TAS.
    void     clearClaimBitfield();

    // ---- Write ----
    void insert(glm::uvec3 pos, uint32_t material, uint32_t leafDepth = 0);
    void remove(glm::uvec3 pos, uint32_t leafDepth = 0);
    void insertBox(glm::uvec3 min, glm::uvec3 max, uint32_t material, uint32_t leafDepth = 0);
    void insertSphere(glm::vec3 centre, float radius, uint32_t material, uint32_t leafDepth = 0);
    void insertFunction(std::function<bool(glm::vec3)> fn,
                        glm::uvec3 min, glm::uvec3 max, uint32_t material, uint32_t leafDepth = 0);

    // ---- Read ----
    // Returns the GPU slot of the leaf at pos, or UINT32_MAX if empty.
    uint32_t lookup(glm::uvec3 pos) const;
    // CPU port of the GPU Raycast() — used for picking (insert/remove on click).
    RayHit raycast(glm::vec3 origin, glm::vec3 direction,
                   uint32_t maxSteps = 300) const;

    // Push pending CPU edits to GPU as a single coalesced glBufferSubData.
    void flushEdits();

    // Force-upload the entire data[] (used internally after a buffer realloc;
    // also exposed for the rare cases where data[] was mutated externally).
    void Update();

    std::vector<Node> data;
    uint8_t  depth;
    uint32_t capacity;
    uint32_t size      = 9;        // 0 = root, 1..8 reserved for root's children
    uint32_t numVoxels = 0;

    // Every occupancy-changing edit (insert/remove) calls logEdit(), which
    // builds a normalPrecision-expanded AABB around the touched voxel and
    // merges it into any overlapping pending region (addEditRegion). This
    // coalesces dense edits like insertSphere into a few large regions. The
    // Renderer pops ONE region per frame and dispatches edit_mark.comp over it,
    // which stamps nBuffer slot [3] (editTimestamp) for the voxels inside;
    // normal.comp then recomputes those normals on their next scheduled visit.
    struct EditRegion {
        glm::uvec3 min;            // inclusive lattice-coord corners
        glm::uvec3 max;
        uint32_t   timestamp_ms;   // most recent edit folded into this region
    };
    std::deque<EditRegion> editRegions;     // pending regions, drained 1 / frame
    bool editLoggingEnabled = false;        // off during the initial scene build
    int  editExpandRadius   = 6;            // AABB padding; kept in sync w/ normalPrecision

    // Generic per-voxel hook fired when a leaf is placed (insert) or cleared (remove):
    // onLeafChanged(pos, level, oldMaterial, newMaterial); newMaterial==0 => removed. Lets a
    // materials-aware subsystem mirror edits in O(depth) without coupling the octree to
    // materials — it only reports the structural change. (Currently unused; kept as an
    // extension point.)
    std::function<void(glm::uvec3, uint32_t, uint32_t, uint32_t)> onLeafChanged = nullptr;

    friend class Renderer;

private:
    GLuint gl_ID = 0;          // octree node buffer (SSBO, uvec2[] : lo/hi per slot)
    GLuint program = 0;
    uint32_t gpuBufferSize = 0;


    GLuint   claimBuffer_ID    = 0;
    uint32_t claimCapacityWords = 0;   // (capacity + 31) / 32

    uint32_t versionCounter = 0;

    // Free 8-slot blocks waiting to be reused (from removes).
    std::stack<uint32_t> freeBlocks;

    // Dirty range (half-open: [dirtyMin, dirtyMax)). UINT32_MAX = empty.
    uint32_t dirtyMin = UINT32_MAX;
    uint32_t dirtyMax = 0;

    uint32_t utils_p2r[maxDepth + 1];
    uint32_t locate(glm::uvec3 position, uint32_t depth_) const;

    void logEdit(glm::uvec3 pos);           // register one occupancy-changing edit
    void addEditRegion(EditRegion box);     // merge a region into editRegions

    void     resizeDataIfNeeded(uint32_t requiredCapacity);
    uint32_t allocBlock();        // returns a fresh or recycled 8-slot block
    void     freeBlock(uint32_t block);
    bool     blockIsEmpty(uint32_t block) const;
    void     freeSubtree(uint32_t blockSlot);
    void     markDirty(uint32_t slot, uint32_t count = 1);
    void     setChildBit(uint32_t internalSlot, uint32_t childIdx);

    // Recompute one internal node's representative material as the majority over
    // its non-empty direct children (ties => lowest id). Returns true iff the
    // node's stored material changed.
    bool     recomputeMaterial(uint32_t internalSlot);
    void     propagateMaterialUp(const uint32_t* pathSlot, int fromLevel);
};
