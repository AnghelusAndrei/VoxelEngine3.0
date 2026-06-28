#include "lighttree.hpp"
#include "octree.hpp"
#include "material.hpp"
#include <cstring>

// ---------------------------------------------------------------------------
// Construction / GL lifecycle
// ---------------------------------------------------------------------------

LightTree::LightTree(Octree* octree_, MaterialPool* materials_)
    : octree(octree_), materials(materials_) {
    // Mirror the octree's root layout: node 0 = root internal, child block at node 8
    // (nodes 1..7 are padding so every 8-node block is 8-aligned). The tree is then
    // populated on the CPU by onLeafChanged during the scene build; GenSSBO uploads it.
    data.assign(16u * NODE_WORDS, 0u);
    size = 16u;
    setInternal(0u, 8u);
    dirtyMin = 0xFFFFFFFFu; dirtyMax = 0u;   // GenSSBO does the first (full) upload
}

LightTree::~LightTree() {
    if (ssbo) glDeleteBuffers(1, &ssbo);
}

void LightTree::attach() {
    octree->onLeafChanged = [this](glm::uvec3 pos, uint32_t level, uint32_t oldMat, uint32_t newMat) {
        onOctreeLeaf(pos, level, oldMat, newMat);
    };
}

void LightTree::GenSSBO() {
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    gpuCapacity = 16u; while (gpuCapacity < size) gpuCapacity *= 2u;            // power-of-2 headroom
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)gpuCapacity * (GLsizeiptr)(NODE_WORDS * sizeof(uint32_t)),
                 NULL, GL_DYNAMIC_DRAW);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);  // zero tail (deterministic)
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size * (GLsizeiptr)(NODE_WORDS * sizeof(uint32_t)), data.data());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_LIGHTTREE_BINDING, ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    dirtyMin = 0xFFFFFFFFu; dirtyMax = 0u;
}

void LightTree::flushEdits() {
    if (ssbo == 0 || dirtyMin >= dirtyMax) { dirtyMin = 0xFFFFFFFFu; dirtyMax = 0u; return; }
    const GLsizeiptr NB = (GLsizeiptr)(NODE_WORDS * sizeof(uint32_t));   // bytes per node
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    if (size > gpuCapacity) {
        // Grow GEOMETRICALLY so growth doesn't respecify the buffer every frame (the
        // per-frame glBufferData realloc was the emissive-edit stall). Orphan + zero + refill —
        // zeroing the tail keeps [size, gpuCapacity) DETERMINISTIC (no uninitialized-read Heisenbug).
        while (gpuCapacity < size) gpuCapacity = gpuCapacity ? gpuCapacity * 2u : 16u;
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)gpuCapacity * NB, NULL, GL_DYNAMIC_DRAW);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)size * NB, data.data());
    } else {
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                        (GLintptr)dirtyMin * NB,
                        (GLsizeiptr)(dirtyMax - dirtyMin) * NB,
                        &data[dirtyMin * NODE_WORDS]);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    dirtyMin = 0xFFFFFFFFu; dirtyMax = 0u;
}

// ---------------------------------------------------------------------------
// Node accessors / block allocator / dirty tracking (mirrors Octree)
// ---------------------------------------------------------------------------

float LightTree::getPower(uint32_t n) const { float f; std::memcpy(&f, &data[n*NODE_WORDS], 4); return f; }
void  LightTree::setPower(uint32_t n, float p) { std::memcpy(&data[n*NODE_WORDS], &p, 4); }

void LightTree::setInternal(uint32_t slot, uint32_t childBlock) {
    uint32_t b = slot * NODE_WORDS;
    data[b + 0] = 0u;                  // power (filled by propagate)
    data[b + 1] = childBlock;          // childBase
    data[b + 2] = 1u;                  // isNode=1, material=0, childMask=0
    markDirty(slot);
}

void LightTree::setLeaf(uint32_t slot, uint32_t material, float power, uint32_t openFaces) {
    uint32_t b = slot * NODE_WORDS;
    setPower(slot, power);
    data[b + 1] = 0u;                              // childBase 0 ⟹ leaf
    data[b + 2] = ((material & 0x7Fu) << 1)        // isNode=0, material[1:7], childMask=0
                | ((openFaces & 0x7u) << 16);      // surface-cull open-face count, reserved bits [16:18]
    markDirty(slot);
}

void LightTree::clearNode(uint32_t slot) {
    uint32_t b = slot * NODE_WORDS;
    for (uint32_t i = 0; i < NODE_WORDS; i++) data[b + i] = 0u;
    markDirty(slot);
}

void LightTree::setChildBit(uint32_t slot, uint32_t octant) {
    data[slot*NODE_WORDS + 2] |= (1u << (8u + octant));    // childMask at bits 8..15
    markDirty(slot);
}
void LightTree::clearChildBit(uint32_t slot, uint32_t octant) {
    data[slot*NODE_WORDS + 2] &= ~(1u << (8u + octant));
    markDirty(slot);
}

uint32_t LightTree::octantOf(glm::uvec3 pos, uint32_t lev) const {
    uint32_t bit = 1u << ((uint32_t)octree->depth - lev);
    return ((pos.x & bit) ? 4u : 0u) | ((pos.y & bit) ? 2u : 0u) | ((pos.z & bit) ? 1u : 0u);
}

uint32_t LightTree::allocBlock() {
    if (!freeBlocks.empty()) {
        uint32_t b = freeBlocks.top(); freeBlocks.pop();
        for (uint32_t i = 0; i < 8u * NODE_WORDS; i++) data[b*NODE_WORDS + i] = 0u;
        markDirty(b, 8);
        return b;
    }
    uint32_t b = size;
    size += 8u;
    if (data.size() < size * NODE_WORDS) data.resize(size * NODE_WORDS, 0u);
    markDirty(b, 8);
    return b;
}

void LightTree::freeBlock(uint32_t block) {
    for (uint32_t i = 0; i < 8u * NODE_WORDS; i++) data[block*NODE_WORDS + i] = 0u;
    markDirty(block, 8);
    freeBlocks.push(block);
}

void LightTree::freeSubtree(uint32_t block) {
    for (uint32_t o = 0; o < 8; o++) {
        uint32_t slot = block + o;
        if (isEmpty(slot)) continue;
        if (isInternal(slot)) freeSubtree(childBase(slot));
        // (clearing happens via freeBlock below)
    }
    freeBlock(block);
}

void LightTree::markDirty(uint32_t node, uint32_t count) {
    if (dirtyMin == 0xFFFFFFFFu) { dirtyMin = node; dirtyMax = node + count; }
    else { if (node < dirtyMin) dirtyMin = node; if (node + count > dirtyMax) dirtyMax = node + count; }
}

// Returns true iff the node's stored power changed (so the caller can stop climbing).
bool LightTree::recomputePower(uint32_t slot) {
    uint32_t block = childBase(slot);
    float sum = 0.0f;
    for (uint32_t o = 0; o < 8; o++) sum += getPower(block + o);   // empty children contribute 0
    if (getPower(slot) == sum) return false;                       // unchanged → ancestors unchanged too
    setPower(slot, sum);
    markDirty(slot);
    return true;
}

void LightTree::propagatePowerUp(const uint32_t* pathNode, int fromLevel) {
    // Early-out like Octree::propagateMaterialUp: a parent's sum can only shift if a child's
    // did, so stop at the first surviving node whose power is unchanged (e.g. re-painting the
    // same light → stops at level 1, instead of walking to the root every insert).
    for (int up = fromLevel; up >= 0; up--) {
        if (isEmpty(pathNode[up])) continue;        // freed ancestor — skip
        if (!recomputePower(pathNode[up])) break;   // surviving & unchanged → done
    }
}

// ---------------------------------------------------------------------------
// Emissive policy + O(depth) insert / remove (mirrors Octree::insert / ::remove)
// ---------------------------------------------------------------------------

float LightTree::lightPower(uint32_t materialId, uint32_t voxelSize) const {
    const Material& m = materials->get(materialId);
    float lum  = 0.2126f * m.color.r + 0.7152f * m.color.g + 0.0722f * m.color.b;
    float area = float(voxelSize) * float(voxelSize);
    return m.emissiveIntensity * lum * area;
}

void LightTree::onOctreeLeaf(glm::uvec3 pos, uint32_t level, uint32_t oldMat, uint32_t newMat) {
    uint32_t depth = (uint32_t)octree->depth;

    // Coarse leaves (LOD, not yet produced): keep the simple mirror, no surface-cull — their
    // neighbourhood is multi-resolution, handled when LOD lands. architecture/LIGHTTREE.md.
    if (level < depth) {
        lightRemoveSubtree(pos, level);
        if (materials->isEmissive(newMat)) lightInsert(pos, level, newMat, 6u);
        return;
    }

    // (1) The edited voxel's own membership: it is a light iff emissive AND not fully enclosed.
    if (materials->isEmissive(newMat)) {
        uint32_t of = countOpenFaces(pos);
        if (of > 0u) lightInsert(pos, level, newMat, of);   // surface emitter
        else         lightRemoveSubtree(pos, level);        // enclosed (e.g. emissive in a solid pocket) → not a light
    } else if (materials->isEmissive(oldMat)) {
        lightRemoveSubtree(pos, level);                     // emissive → non-emissive / removed
    }

    // (2) An OCCUPANCY change (air↔solid) flips one face of each neighbour. A pure material change
    //     (solid→solid) leaves neighbour enclosure untouched, so only act on occupancy changes.
    bool wasSolid = (oldMat != 0u), nowSolid = (newMat != 0u);
    if (wasSolid != nowSolid) updateNeighbourOpenFaces(pos, nowSolid);
}

void LightTree::lightInsert(glm::uvec3 pos, uint32_t level, uint32_t material, uint32_t openFaces) {
    float power = lightPower(material, 1u << ((uint32_t)octree->depth - level));

    uint32_t pathNode[maxDepth + 1];
    pathNode[0] = 0u;
    uint32_t parent = 0u;
    uint32_t block  = childBase(0u);          // root's child block (8)

    for (uint32_t lev = 1; lev <= level; lev++) {
        uint32_t oct  = octantOf(pos, lev);
        uint32_t slot = block + oct;
        pathNode[lev] = slot;

        if (lev == level) {
            bool wasEmpty = isEmpty(slot);
            if (!wasEmpty && isInternal(slot)) freeSubtree(childBase(slot));   // overwrite subtree (rare)
            setLeaf(slot, material, power, openFaces);
            if (wasEmpty) setChildBit(parent, oct);
            propagatePowerUp(pathNode, (int)lev - 1);
            return;
        }

        if (isEmpty(slot)) {
            uint32_t nb = allocBlock();
            setInternal(slot, nb);
            setChildBit(parent, oct);
            parent = slot; block = nb;
        } else if (isInternal(slot)) {
            parent = slot; block = childBase(slot);
        } else {
            // A leaf where we need an internal — only if mixed-level emissives share a path
            // (does not happen for uniform full-depth lights). Replace defensively.
            uint32_t nb = allocBlock();
            setInternal(slot, nb);
            parent = slot; block = nb;
        }
    }
}

// Remove the node at EXACTLY (pos, level) — a single leaf, or an entire finer subtree (when a
// coarse leaf replaces it) — then walk up freeing emptied blocks and propagating power.
void LightTree::lightRemoveSubtree(glm::uvec3 pos, uint32_t level) {
    uint32_t pathNode[maxDepth + 1];
    pathNode[0] = 0u;
    uint32_t block = childBase(0u);

    for (uint32_t lev = 1; lev <= level; lev++) {
        uint32_t slot = block + octantOf(pos, lev);
        pathNode[lev] = slot;
        if (isEmpty(slot)) return;                          // nothing at/under this path
        if (lev == level) {                                 // target — remove leaf or whole subtree
            if (isInternal(slot)) freeSubtree(childBase(slot));
            clearNode(slot);
            // Structural walk-up: clear child bits; free blocks that just emptied (root never freed).
            for (int up = (int)level - 1; up >= 0; up--) {
                uint32_t pSlot = pathNode[up];
                uint32_t oct   = pathNode[up + 1] - childBase(pSlot);
                clearChildBit(pSlot, oct);
                if (up == 0) break;
                if (childMask(pSlot) != 0u) break;
                uint32_t blk = childBase(pSlot);
                clearNode(pSlot);
                freeBlock(blk);
            }
            propagatePowerUp(pathNode, (int)level - 1);
            return;
        }
        if (!isInternal(slot)) return;                      // coarser leaf covers (pos,level): nothing finer
        block = childBase(slot);
    }
}

// ---------------------------------------------------------------------------
// Surface-cull (enclosed-voxel exclusion) — architecture/LIGHTTREE.md §Surface-cull.
// A voxel whose 6 face-neighbours are all solid radiates nothing externally → not a light.
// Maintained O(depth)/edit via the 6 neighbours; openFaces lives in the light leaf (word2).
// ---------------------------------------------------------------------------

static const glm::ivec3 kFaceOffset[6] = {
    { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
};

// Octree material at p (0 = air/void). Descends to the first leaf, so a coarse leaf at a shallower
// level reports its material (solid). Out-of-volume is air — boundary voxels radiate into the void.
uint32_t LightTree::materialAt(glm::uvec3 p) const {
    uint32_t depth = (uint32_t)octree->depth;
    uint32_t bound = 1u << depth;
    if (p.x >= bound || p.y >= bound || p.z >= bound) return 0u;
    uint32_t block = octree->data[0].next();
    for (uint32_t lev = 1; lev <= depth; lev++) {
        const Octree::Node& n = octree->data[block + octantOf(p, lev)];
        if (n.empty())   return 0u;
        if (!n.isNode()) return n.material();               // leaf at any level → solid material
        block = n.next();
    }
    return 0u;
}

// # of the 6 face-neighbours of the full-depth voxel at `pos` that are air (an open emitting face).
uint32_t LightTree::countOpenFaces(glm::uvec3 pos) const {
    int bound = int(1u << (uint32_t)octree->depth);
    uint32_t open = 0u;
    for (const glm::ivec3& o : kFaceOffset) {
        glm::ivec3 np = glm::ivec3(pos) + o;
        if (np.x < 0 || np.y < 0 || np.z < 0 || np.x >= bound || np.y >= bound || np.z >= bound) { open++; continue; }
        if (materialAt(glm::uvec3(np)) == 0u) open++;
    }
    return open;
}

// Full-depth light leaf at pos, or NO_NODE (absent / coarser-covered).
uint32_t LightTree::lightLeafSlot(glm::uvec3 pos) const {
    uint32_t depth = (uint32_t)octree->depth;
    uint32_t block = childBase(0u);
    for (uint32_t lev = 1; lev <= depth; lev++) {
        uint32_t slot = block + octantOf(pos, lev);
        if (isEmpty(slot))     return NO_NODE;
        if (lev == depth)      return isInternal(slot) ? NO_NODE : slot;
        if (!isInternal(slot)) return NO_NODE;
        block = childBase(slot);
    }
    return NO_NODE;
}

// An occupancy change at `pos` flipped one face of each of its 6 neighbours. For every EMISSIVE
// neighbour, adjust its stored openFaces: a surface light that loses its last face becomes enclosed
// (cull); a culled light that gains a face becomes a surface light again (re-insert).
void LightTree::updateNeighbourOpenFaces(glm::uvec3 pos, bool nowSolid) {
    uint32_t depth = (uint32_t)octree->depth;
    int bound = int(1u << depth);
    for (const glm::ivec3& o : kFaceOffset) {
        glm::ivec3 np = glm::ivec3(pos) + o;
        if (np.x < 0 || np.y < 0 || np.z < 0 || np.x >= bound || np.y >= bound || np.z >= bound) continue;
        glm::uvec3 n = glm::uvec3(np);
        if (!materials->isEmissive(materialAt(n))) continue;        // only emissive neighbours carry openFaces
        uint32_t slot = lightLeafSlot(n);
        if (nowSolid) {                                             // C became solid → close n's shared face
            if (slot == NO_NODE) continue;                          // already enclosed/culled → stays
            uint32_t of = openFaces(slot);
            of = (of > 0u) ? of - 1u : 0u;
            setOpenFaces(slot, of);
            if (of == 0u) lightRemoveSubtree(n, depth);             // just enclosed → cull
        } else {                                                   // C became air → open n's shared face
            if (slot != NO_NODE) { uint32_t of = openFaces(slot); setOpenFaces(slot, of < 6u ? of + 1u : 6u); }
            else { uint32_t of = countOpenFaces(n); if (of > 0u) lightInsert(n, depth, materialAt(n), of); }  // un-enclosed → re-insert
        }
    }
}
