#include "octree.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Construction / GPU lifecycle
// ---------------------------------------------------------------------------

Octree::Octree(Config* config) {
    // NOTE: callers currently pass `&stackConfig`, so we don't own the
    // pointer — no `delete config;` here (the old code had one and was
    // technically UB). If you ever switch to a heap-allocated config,
    // free it on the call site.
    depth = config->depth > maxDepth ? maxDepth : config->depth;

    // utils_p2r[lev] = 2^(depth-lev) — bit mask used by locate() to pick the
    // child octant at level `lev` (lev = 1..depth). Matches the GPU shader's
    // p2c[] indexing: shader's depth=N corresponds to CPU's lev=N+1.
    uint32_t p2r = 1;
    for (int i = depth; i >= 1; i--) {
        utils_p2r[i] = p2r;
        p2r <<= 1;
    }
    utils_p2r[0] = p2r;  // unused, but defined for safety

    // Initial state: slot 0 is the root internal node. Its 8-child block starts
    // at slot 8 (slots 1..7 are padding) so that every 8-slot block is
    // 8-slot/64-byte aligned and lands in a single L2 cache line. allocBlock()
    // hands out further blocks at 8-aligned bases (size stays a multiple of 8).
    capacity = 32;
    data.assign(capacity, Node{});
    size = 16;

    data[0].makeInternal(8u);   // root → child block at slot 8

    // Mark the initial 16 slots dirty so the first flushEdits() syncs them.
    markDirty(0, size);
}

Octree::~Octree() { freeVRAM(); }

void Octree::setProgram(GLuint program_) { program = program_; }

void Octree::GenUBO() {
    // Octree node buffer (SSBO). uvec2 per slot (lo, hi).
    glGenBuffers(1, &gl_ID);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(capacity) * GLsizeiptr(sizeof(Node)),
                 NULL, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_OCTREE_BINDING, gl_ID);

    // Claim bitfield (SSBO) — 1 bit per slot, zero-initialised, sized to capacity.
    glGenBuffers(1, &claimBuffer_ID);
    claimCapacityWords = (capacity + 31u) / 32u;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, claimBuffer_ID);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 GLsizeiptr(claimCapacityWords) * GLsizeiptr(sizeof(GLuint)),
                 NULL, GL_DYNAMIC_DRAW);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_CLAIM_BINDING, claimBuffer_ID);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    gpuBufferSize = capacity;
    // First-time push of the initial empty tree.
    Update();
}

void Octree::freeVRAM() {
    glDeleteBuffers(1, &gl_ID);
    glDeleteBuffers(1, &claimBuffer_ID);
}

void Octree::clearClaimBitfield() {
    if (claimBuffer_ID == 0) return;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, claimBuffer_ID);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void Octree::BindUniforms(uint8_t& texturesBound) {
    // Octree and its claim bitfield are SSBOs bound to fixed binding points, not
    // texture units — texturesBound is intentionally left untouched so callers
    // that bind real textures (skybox, ...) keep their unit numbering.
    (void)texturesBound;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_OCTREE_BINDING, gl_ID);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, core::SSBO_CLAIM_BINDING, claimBuffer_ID);
    glUniform1ui(glGetUniformLocation(program, "octreeDepth"), (GLuint)depth);
}


void Octree::Update() {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(capacity) * GLsizeiptr(sizeof(Node)),
                 data.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    gpuBufferSize = capacity;
    // Whole buffer is now in sync.
    dirtyMin = UINT32_MAX;
    dirtyMax = 0;
}

void Octree::resizeDataIfNeeded(uint32_t requiredCapacity) {
    if (requiredCapacity <= capacity) return;
    while (capacity < requiredCapacity) capacity *= 2;
    // No GL_MAX_TEXTURE_BUFFER_SIZE clamp here: as an SSBO the buffer is bounded
    // by GL_MAX_SHADER_STORAGE_BLOCK_SIZE / VRAM, well above the ~8M-slot range
    // the (now full 32-bit) `next` pointer is meant to unlock. A hard cap can be
    // reintroduced if paging/streaming (Stage 6) needs one.

    data.resize(capacity, Node{});

    // Reallocate GPU storage. The buffer name is unchanged, so the binding made
    // by glBindBufferBase in GenUBO() stays valid after glBufferData.
    if (gl_ID != 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
        glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(capacity) * GLsizeiptr(sizeof(Node)),
                     data.data(), GL_DYNAMIC_DRAW);

        // Grow the claim bitfield in lockstep and re-zero it.
        claimCapacityWords = (capacity + 31u) / 32u;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, claimBuffer_ID);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     GLsizeiptr(claimCapacityWords) * GLsizeiptr(sizeof(GLuint)),
                     NULL, GL_DYNAMIC_DRAW);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

        gpuBufferSize = capacity;
        // Buffer was just fully repopulated — no pending dirty range needed.
        dirtyMin = UINT32_MAX;
        dirtyMax = 0;
    }
}

// ---------------------------------------------------------------------------
// Block allocator + dirty tracking
// ---------------------------------------------------------------------------

uint32_t Octree::allocBlock() {
    if (!freeBlocks.empty()) {
        uint32_t b = freeBlocks.top(); freeBlocks.pop();
        // Zero the recycled block so stale data doesn't leak through.
        for (int i = 0; i < 8; i++) data[b + i].clear();
        markDirty(b, 8);
        return b;
    }
    // size starts at 16 and only ever grows by 8, so every base is 8-aligned.
    uint32_t b = size;
    size += 8;
    resizeDataIfNeeded(size);
    // Fresh slots are already zero from the resize/initial assign.
    markDirty(b, 8);
    return b;
}

void Octree::freeBlock(uint32_t block) {
    for (int i = 0; i < 8; i++) data[block + i].clear();
    markDirty(block, 8);
    freeBlocks.push(block);
}

bool Octree::blockIsEmpty(uint32_t block) const {
    for (int i = 0; i < 8; i++)
        if (!data[block + i].empty()) return false;
    return true;
}

void Octree::markDirty(uint32_t slot, uint32_t count) {
    if (dirtyMin == UINT32_MAX) {
        dirtyMin = slot;
        dirtyMax = slot + count;
    } else {
        if (slot < dirtyMin) dirtyMin = slot;
        if (slot + count > dirtyMax) dirtyMax = slot + count;
    }
}

// Set bit `childIdx` (0–7) in the internal node's childmask, recording that
// octant as occupied. Called by insert each time a slot transitions empty→non-empty.
void Octree::setChildBit(uint32_t internalSlot, uint32_t childIdx) {
    Node& p = data[internalSlot];
    if (!p.isNode()) return;
    p.lo |= (1u << (Node::CM_SHIFT + childIdx));
    markDirty(internalSlot);
}

// Recompute one internal node's representative material as the majority over its
// non-empty direct children (ties broken toward the lowest material id). Leaves
// and internal children both expose their material at the same lo-word bits, so
// an internal child contributes its own already-propagated representative. The
// child set is small (≤ 8), so an O(n²) tally is cheaper than zeroing a 128-entry
// histogram. Returns true iff the stored material changed.
bool Octree::recomputeMaterial(uint32_t internalSlot) {
    Node& node = data[internalSlot];
    if (!node.isNode()) return false;          // leaf or freed slot: nothing to do
    uint32_t block = node.next();

    uint32_t mats[8];
    int n = 0;
    for (int i = 0; i < 8; i++) {
        const Node& c = data[block + i];
        if (c.empty()) continue;
        mats[n++] = c.material();
    }

    uint32_t best = 0;
    int bestCount = 0;
    for (int i = 0; i < n; i++) {
        int c = 0;
        for (int j = 0; j < n; j++) if (mats[j] == mats[i]) c++;
        if (c > bestCount || (c == bestCount && mats[i] < best)) {
            bestCount = c;
            best      = mats[i];
        }
    }

    if (node.material() == best) return false;
    node.setMaterial(best);
    markDirty(internalSlot);
    return true;
}

void Octree::propagateMaterialUp(const uint32_t* pathSlot, int fromLevel) {
    for (int up = fromLevel; up >= 0; up--) {
        if (data[pathSlot[up]].empty()) continue;       // freed ancestor — skip
        if (!recomputeMaterial(pathSlot[up])) break;    // surviving & unchanged → done
    }
}

// Recursively free a child block and everything reachable from it. Used by
// insert when a coarse leaf is placed at a level that previously held an
// internal subtree (without this, the sub-blocks would leak — unreachable but
// never returned to freeBlocks). Recursion depth is bounded by maxDepth (11),
// so stack use is trivial. The caller is responsible for zeroing/repurposing
// the node that OWNS this block; freeSubtree only touches the block itself
// and what's below it.
void Octree::freeSubtree(uint32_t blockSlot) {
    for (uint32_t i = 0; i < 8; i++) {
        Node& child = data[blockSlot + i];
        if (child.empty()) continue;
        if (child.isNode()) {
            freeSubtree(child.next());      // tear down the subtree first
        } else {
            numVoxels--;                    // a leaf — keep the voxel tally honest
        }
        child.clear();
    }
    markDirty(blockSlot, 8);
    freeBlocks.push(blockSlot);
}

void Octree::flushEdits() {
    if (dirtyMin >= dirtyMax || gl_ID == 0) {
        dirtyMin = UINT32_MAX; dirtyMax = 0;
        return;
    }
    if (dirtyMax > capacity) dirtyMax = capacity;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, gl_ID);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER,
                    GLintptr(dirtyMin)  * GLintptr(sizeof(Node)),
                    GLsizeiptr(dirtyMax - dirtyMin) * GLsizeiptr(sizeof(Node)),
                    &data[dirtyMin]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    dirtyMin = UINT32_MAX;
    dirtyMax = 0;
}

// ---------------------------------------------------------------------------
// Phase 2 — edit-region tracking for normal recomputation
// ---------------------------------------------------------------------------

// Register one occupancy-changing edit. Builds an AABB padded by
// editExpandRadius (kept in sync with the renderer's normalPrecision) so the
// box covers every voxel whose refined normal the edit could have invalidated,
// clamps it to the octree bounds, timestamps it with the current frame time,
// and folds it into editRegions.
void Octree::logEdit(glm::uvec3 pos) {
    int r  = editExpandRadius;
    int L  = (int)(1u << depth);
    int px = (int)pos.x, py = (int)pos.y, pz = (int)pos.z;

    EditRegion box;
    box.min = glm::uvec3((uint32_t)std::max(px - r, 0),
                         (uint32_t)std::max(py - r, 0),
                         (uint32_t)std::max(pz - r, 0));
    box.max = glm::uvec3((uint32_t)std::min(px + r, L - 1),
                         (uint32_t)std::min(py + r, L - 1),
                         (uint32_t)std::min(pz + r, L - 1));
    box.timestamp_ms = (uint32_t)(glfwGetTime() * 1000.0);
    addEditRegion(box);
}

// Merge `box` into any overlapping pending region; the enlarged box is then
// re-checked against the rest, so a chain of nearby edits coalesces into one
// region. Linear scan — editRegions stays tiny because overlapping edits
// collapse together (a contiguous insertSphere ends up as a single region).
void Octree::addEditRegion(EditRegion box) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < editRegions.size(); i++) {
            const EditRegion& e = editRegions[i];
            bool overlap =
                box.min.x <= e.max.x && e.min.x <= box.max.x &&
                box.min.y <= e.max.y && e.min.y <= box.max.y &&
                box.min.z <= e.max.z && e.min.z <= box.max.z;
            if (!overlap) continue;
            box.min.x = std::min(box.min.x, e.min.x);
            box.min.y = std::min(box.min.y, e.min.y);
            box.min.z = std::min(box.min.z, e.min.z);
            box.max.x = std::max(box.max.x, e.max.x);
            box.max.y = std::max(box.max.y, e.max.y);
            box.max.z = std::max(box.max.z, e.max.z);
            box.timestamp_ms = std::max(box.timestamp_ms, e.timestamp_ms);
            editRegions.erase(editRegions.begin() + i);
            merged = true;
            break;          // restart the scan with the enlarged box
        }
    }
    editRegions.push_back(box);
}

// ---------------------------------------------------------------------------
// Locate / lookup
// ---------------------------------------------------------------------------

uint32_t Octree::locate(glm::uvec3 position, uint32_t depth_) const {
    return (((bool)(position.x & utils_p2r[depth_])) << 2)
         | (((bool)(position.y & utils_p2r[depth_])) << 1)
         |  ((bool)(position.z & utils_p2r[depth_]));
}

uint32_t Octree::lookup(glm::uvec3 pos) const {
    uint32_t bound = 1u << depth;
    if (pos.x >= bound || pos.y >= bound || pos.z >= bound) return UINT32_MAX;

    uint32_t block = data[0].next();   // root's children
    for (uint32_t lev = 1; lev <= depth; lev++) {
        uint32_t slot = block + locate(pos, lev);
        const Node& n = data[slot];
        if (lev == depth) {
            return (!n.empty() && n.material() != 0) ? slot : UINT32_MAX;
        }
        if (n.empty() || !n.isNode()) return UINT32_MAX;
        block = n.next();
    }
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// insert / remove — direct edits on the linear data[] vector
// ---------------------------------------------------------------------------

void Octree::insert(glm::uvec3 pos, uint32_t material, uint32_t leafDepth) {
    uint32_t bound = 1u << depth;
    if (pos.x >= bound || pos.y >= bound || pos.z >= bound) return;
    if (material == 0) return;   // material 0 is the empty sentinel

    // leafDepth: 0 (default) or > depth -> place a leaf at full depth (legacy
    // behaviour). Otherwise place a *coarse leaf* at that level, covering a
    // (octreeLength >> leafDepth)^3 region.
    uint32_t targetDepth = (leafDepth == 0u || leafDepth > depth) ? depth : leafDepth;

    // Record of the slots descended through, so we can walk back up after placing
    // the leaf and propagate the representative material (pathSlot[0] = root).
    uint32_t pathSlot[maxDepth + 1];
    pathSlot[0] = 0;
    uint32_t parent_slot = 0;
    uint32_t block       = data[0].next();

    for (uint32_t lev = 1; lev <= targetDepth; lev++) {
        uint32_t childIdx = locate(pos, lev);
        uint32_t slot     = block + childIdx;
        pathSlot[lev]     = slot;

        if (lev == targetDepth) {
            Node& cur = data[slot];
            bool wasEmpty   = cur.empty();
            bool hadSubtree = !wasEmpty && cur.isNode();
            uint32_t oldMat = (!wasEmpty && !cur.isNode()) ? cur.material() : 0u;
            if (hadSubtree) freeSubtree(cur.next());

            cur.makeLeaf(material, (++versionCounter) & Node::VER_MASK);
            markDirty(slot);
            if (wasEmpty) {
                numVoxels++;
                setChildBit(parent_slot, childIdx);
            }
            if (editLoggingEnabled && (wasEmpty || hadSubtree)) logEdit(pos);

            // Push the representative material up the insertion path.
            propagateMaterialUp(pathSlot, (int)lev - 1);
            if (onLeafChanged) onLeafChanged(pos, lev, oldMat, material);   // mirror to LightTree etc.
            return;
        }

        Node cur = data[slot];
        if (cur.empty() || !cur.isNode()) {
            bool wasEmpty = cur.empty();
            uint32_t newBlock = allocBlock();

            data[slot].makeInternal(newBlock);
            markDirty(slot);
            if (wasEmpty) {
                setChildBit(parent_slot, childIdx);
            }

            parent_slot = slot;
            block       = newBlock;
            continue;
        }
        // Internal node — descend into existing subtree.
        parent_slot = slot;
        block       = cur.next();
    }
}

void Octree::remove(glm::uvec3 pos, uint32_t leafDepth) {
    uint32_t bound = 1u << depth;
    if (pos.x >= bound || pos.y >= bound || pos.z >= bound) return;

    // leafDepth: 0 (default) or > depth -> descend to full depth. Otherwise
    // cap descent at `leafDepth`. Either way, stop at the FIRST leaf found
    // along the path — coarse leaves (Phase 3+) are correctly removed even
    // when they sit above the maxdepth level.
    uint32_t maxDescent = (leafDepth == 0u || leafDepth > depth) ? depth : leafDepth;

    // Path of internal-node slots descended through (pathSlot[0] = root,
    // pathSlot[lev] = slot at level lev). Used by the walk-up to free
    // parents that just lost their last child.
    uint32_t pathSlot[maxDepth + 1];
    pathSlot[0] = 0;
    uint32_t block = data[0].next();
    uint32_t leafLev = 0;            // 0 = no leaf found yet

    for (uint32_t lev = 1; lev <= maxDescent; lev++) {
        uint32_t slot = block + locate(pos, lev);
        pathSlot[lev] = slot;

        Node n = data[slot];
        if (n.empty()) return;                   // empty along the path — nothing to remove
        if (!n.isNode()) {                       // leaf at this level — remove it
            leafLev = lev;
            uint32_t oldMat = n.material();
            data[slot].clear();
            markDirty(slot);
            numVoxels--;
            if (editLoggingEnabled) logEdit(pos);
            if (onLeafChanged) onLeafChanged(pos, lev, oldMat, 0u);   // mirror removal to LightTree etc.
            break;
        }
        if (lev == maxDescent) return;           // hit the cap with an internal node still under us
        block = n.next();
    }
    if (leafLev == 0) return;

    // Structural walk-up: clear the child bit in each parent and free blocks that
    // just emptied. Root (upLev == 0) is never freed — its block is permanent —
    // but its childmask is kept accurate for GPU culling passes. Unlike the old
    // code this `break`s (rather than returns) so the material walk-up below runs.
    for (int upLev = (int)leafLev - 1; upLev >= 0; upLev--) {
        uint32_t pSlot    = pathSlot[upLev];
        Node&    p        = data[pSlot];
        uint32_t childIdx = pathSlot[upLev + 1] - p.next();

        p.lo &= ~(1u << (Node::CM_SHIFT + childIdx));   // clear child bit

        if (upLev == 0)            { markDirty(pSlot); break; }
        if (p.childmask() != 0)    { markDirty(pSlot); break; }
        uint32_t blk = p.next();
        p.clear();
        markDirty(pSlot);
        freeBlock(blk);
    }

    // Representative-material walk-up. A removal can shift the majority at every
    // surviving ancestor up to the root, even where no block was freed, so this
    // is separate from the structural walk-up (which stops at the first parent
    // that keeps a child). propagateMaterialUp skips ancestors that were freed
    // above and stops at the first surviving node whose material is unchanged.
    propagateMaterialUp(pathSlot, (int)leafLev - 1);
}

void Octree::insertBox(glm::uvec3 min, glm::uvec3 max, uint32_t material, uint32_t leafDepth) {
    uint32_t bound = 1u << depth;
    max.x = std::min(max.x, bound);
    max.y = std::min(max.y, bound);
    max.z = std::min(max.z, bound);
    for (uint32_t x = min.x; x < max.x; x++)
    for (uint32_t y = min.y; y < max.y; y++)
    for (uint32_t z = min.z; z < max.z; z++)
        insert(glm::uvec3(x, y, z), material, leafDepth);
}

void Octree::insertSphere(glm::vec3 centre, float radius, uint32_t material, uint32_t leafDepth) {
    int32_t bound = int32_t(1u << depth);
    int32_t x0 = std::max(0,         (int32_t)std::floor(centre.x - radius));
    int32_t y0 = std::max(0,         (int32_t)std::floor(centre.y - radius));
    int32_t z0 = std::max(0,         (int32_t)std::floor(centre.z - radius));
    int32_t x1 = std::min(bound - 1, (int32_t)std::ceil (centre.x + radius));
    int32_t y1 = std::min(bound - 1, (int32_t)std::ceil (centre.y + radius));
    int32_t z1 = std::min(bound - 1, (int32_t)std::ceil (centre.z + radius));

    for (int32_t x = x0; x <= x1; x++)
    for (int32_t y = y0; y <= y1; y++)
    for (int32_t z = z0; z <= z1; z++) {
        glm::vec3 c(x + 0.5f, y + 0.5f, z + 0.5f);
        if (glm::distance(c, centre) <= radius)
            insert(glm::uvec3(x, y, z), material, leafDepth);
    }
}

void Octree::insertFunction(std::function<bool(glm::vec3)> fn,
                            glm::uvec3 min, glm::uvec3 max, uint32_t material, uint32_t leafDepth) {
    uint32_t bound = 1u << depth;
    max.x = std::min(max.x, bound);
    max.y = std::min(max.y, bound);
    max.z = std::min(max.z, bound);
    for (uint32_t x = min.x; x < max.x; x++)
    for (uint32_t y = min.y; y < max.y; y++)
    for (uint32_t z = min.z; z < max.z; z++)
        if (fn(glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f)))
            insert(glm::uvec3(x, y, z), material, leafDepth);
}

// ---------------------------------------------------------------------------
// Raycast — CPU port of internal.glsl::Raycast(), traversing data[] directly
// ---------------------------------------------------------------------------
// Mirrors the GPU algorithm exactly: ray origin is captured ONCE up-front and
// every t-value during DDA is computed relative to it (not the current rpos),
// so picked positions match what the shader sees pixel-for-pixel.

Octree::RayHit Octree::raycast(glm::vec3 origin, glm::vec3 direction,
                               uint32_t maxSteps) const {
    RayHit noHit;
    if (data.empty()) return noHit;

    const uint32_t L  = 1u << depth;
    const float    bF = float(L);
    const glm::vec3 invDir = 1.0f / direction;

    origin += direction * 4.0f;

    auto inBounds = [&](const glm::vec3& p) -> bool {
        return p.x >= 0.0f && p.x <= bF
            && p.y >= 0.0f && p.y <= bF
            && p.z >= 0.0f && p.z <= bF;
    };

    glm::vec3 rpos;
    if (inBounds(origin)) {
        rpos = origin;
    } else {
        glm::vec3 t1  = (glm::vec3(0.0f) - origin + 0.001f) * invDir;
        glm::vec3 t2  = (glm::vec3(bF)   - origin - 0.001f) * invDir;
        glm::vec3 tmi = glm::min(t1, t2), tmx = glm::max(t1, t2);
        float t_enter = std::max(std::max(tmi.x, tmi.y), tmi.z);
        float t_exit  = std::min(std::min(tmx.x, tmx.y), tmx.z);
        if (t_exit < t_enter || t_exit < 0.0f) return noHit;
        rpos = direction * t_enter + origin;
    }

    for (uint32_t q = 0; inBounds(rpos) && q <= maxSteps; ++q) {
        glm::uvec3 ur(uint32_t(rpos.x), uint32_t(rpos.y), uint32_t(rpos.z));

        uint32_t   block   = data[0].next();
        uint32_t   tgtSz   = L;
        glm::uvec3 tgtOrig(0u, 0u, 0u);

        for (uint32_t lev = 1; lev <= depth; ++lev) {
            uint32_t childSz = 1u << (depth - lev);
            uint32_t idx     = locate(ur, lev);
            uint32_t slot    = block + idx;
            const Node& n    = data[slot];
            glm::uvec3 cOrig(
                ur.x & ~(childSz - 1u),
                ur.y & ~(childSz - 1u),
                ur.z & ~(childSz - 1u)
            );

            if (n.empty()) {
                tgtSz = childSz; tgtOrig = cOrig; break;
            }
            if (lev == depth) {
                if (!n.isNode() && n.material() != 0) {
                    RayHit h;
                    h.hit      = true;
                    h.material = n.material();
                    h.slot     = slot;
                    h.position = ur;
                    return h;
                }
                tgtSz = childSz; tgtOrig = cOrig; break;
            }
            if (!n.isNode()) {
                // Compressed leaf at internal level (not produced by current
                // edit code, but tolerated for forward compatibility).
                if (n.material() != 0) {
                    RayHit h;
                    h.hit      = true;
                    h.material = n.material();
                    h.slot     = slot;
                    h.position = ur;
                    return h;
                }
                tgtSz = childSz; tgtOrig = cOrig; break;
            }
            block   = n.next();
            tgtSz   = childSz;
            tgtOrig = cOrig;
        }

        glm::vec3 bmin = glm::vec3(tgtOrig);
        glm::vec3 bmax = bmin + glm::vec3(float(tgtSz));
        glm::vec3 t1   = (bmin - origin - 0.001f) * invDir;
        glm::vec3 t2   = (bmax - origin + 0.001f) * invDir;
        glm::vec3 tmx  = glm::max(t1, t2);
        float t_exit   = std::min(std::min(tmx.x, tmx.y), tmx.z);
        rpos = direction * t_exit + origin;
    }
    return noHit;
}
