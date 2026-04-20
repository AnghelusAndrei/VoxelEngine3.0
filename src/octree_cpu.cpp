#include "octree_cpu.hpp"


OctreeCPU::OctreeCPU(uint8_t depth_) {
    depth = depth_;
    root  = nullptr;


    uint32_t p2r = 1;
    for (int i = depth; i >= 1; i--) {
        utils_p2r[i] = p2r;
        p2r <<= 1;
    }
    utils_p2r[0] = p2r; 
}

OctreeCPU::~OctreeCPU() {
    freeSubtree(root);
}

void OctreeCPU::freeSubtree(Node* node) {
    if (!node) return;
    std::stack<Node*> stk;
    stk.push(node);
    while (!stk.empty()) {
        Node* n = stk.top(); stk.pop();
        for (int i = 0; i < 8; i++)
            if (n->children[i]) stk.push(n->children[i]);
        delete n;
    }
}

uint32_t OctreeCPU::locate(glm::uvec3 pos, uint32_t level) const {
    uint32_t p2 = utils_p2r[level];
    return (uint32_t(bool(pos.x & p2)) << 2) |
           (uint32_t(bool(pos.y & p2)) << 1) |
           uint32_t(bool(pos.z & p2));
}

void OctreeCPU::insert(glm::uvec3 position, uint32_t material) {
    uint32_t bound = 1u << depth;
    if (position.x >= bound || position.y >= bound || position.z >= bound)
        return;
    if (!root) root = new Node();
    insertR(position, material, root, 1);
}

void OctreeCPU::insertR(glm::uvec3 pos, uint32_t material, Node*& node, uint8_t level) {
    if (!node) node = new Node();
    node->dirty = true;

    if (level > depth) {
        node->material = material;
        node->childrenCount = 0;
        return;
    }

    uint32_t idx = locate(pos, level);
    bool childExisted = (node->children[idx] != nullptr);
    insertR(pos, material, node->children[idx], level + 1);

    if (!childExisted && node->children[idx])
        node->childrenCount++;
}

void OctreeCPU::remove(glm::uvec3 position) {
    uint32_t bound = 1u << depth;
    if (position.x >= bound || position.y >= bound || position.z >= bound)
        return;
    if (!root) return;
    removeR(position, root, 1);
}

void OctreeCPU::removeR(glm::uvec3 pos, Node*& node, uint8_t level) {
    if (!node) return;
    node->dirty = true;

    if (level > depth) {
        delete node;
        node = nullptr;
        return;
    }

    uint32_t idx = locate(pos, level);
    bool hadChild = (node->children[idx] != nullptr);
    removeR(pos, node->children[idx], level + 1);

    if (hadChild && node->children[idx] == nullptr) {
        node->childrenCount--;
        if (node->childrenCount == 0) {
            // Capture gpuBlock before this node is deleted so Octree can reclaim it.
            if (node->gpuBlock != UINT32_MAX)
                freedGpuBlocks.push_back(node->gpuBlock);
            delete node;
            node = nullptr;
        }
    }
}

OctreeCPU::Node* OctreeCPU::lookup(glm::uvec3 pos) const {
    uint32_t bound = 1u << depth;
    if (pos.x >= bound || pos.y >= bound || pos.z >= bound) return nullptr;

    Node* node = root;
    for (uint8_t level = 1; level <= depth + 1; level++) {
        if (node == nullptr) return nullptr;  // empty subtree
        if (level > depth) return node;   // reached the leaf
        uint32_t idx = locate(pos, level);
        node = node->children[idx];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Raycast — CPU port of ray.frag Raycast()
// ---------------------------------------------------------------------------
// The GPU algorithm keeps the ray origin FIXED after the initial advance and
// computes all t-values relative to it.  We replicate that exactly so the
// hit position matches what the GPU shader sees.
//
// Mapping GPU → CPU:
//   texelFetch(octreeTexture, offset)  →  pointer tree traversal
//   !leaf.type (non-leaf in GPU sense) →  node->children[idx] == nullptr
//   leaf.material != 0                 →  child->material != 0  (at lev==depth)
//   p2c[d] = octreeLength >> d         →  childSz = 1 << (depth - lev)
// ---------------------------------------------------------------------------

OctreeCPU::RayHit OctreeCPU::raycast(glm::vec3 origin, glm::vec3 direction,
                                      uint32_t maxSteps) const {
    RayHit noHit;
    if (!root) return noHit;

    const uint32_t octreeLen = 1u << depth;
    const float bF = float(octreeLen);
    const glm::vec3 invDir = 1.0f / direction;

    origin += direction * 4.0f;

    auto inBounds = [&](const glm::vec3& p) -> bool {
        return p.x >= 0.0f && p.x <= bF &&
               p.y >= 0.0f && p.y <= bF &&
               p.z >= 0.0f && p.z <= bF;
    };

    glm::vec3 rpos;
    if (inBounds(origin)) {
        rpos = origin;
    } else {
        glm::vec3 t1  = (glm::vec3(0.0f) - origin + 0.001f) * invDir;
        glm::vec3 t2  = (glm::vec3(bF)   - origin - 0.001f) * invDir;
        glm::vec3 tmi = glm::min(t1, t2), tmx = glm::max(t1, t2);
        float t_enter = glm::max(glm::max(tmi.x, tmi.y), tmi.z);
        float t_exit  = glm::min(glm::min(tmx.x, tmx.y), tmx.z);
        if (t_exit < t_enter || t_exit < 0.0f) return noHit;
        rpos = direction * t_enter + origin;
    }

    for (uint32_t q = 0; inBounds(rpos) && q <= maxSteps; ++q) {
        glm::uvec3 ur(uint32_t(rpos.x), uint32_t(rpos.y), uint32_t(rpos.z));

        Node* node = root;
        uint32_t tgtSz = octreeLen;   // size of block to DDA-skip
        glm::uvec3 tgtOrig(0u, 0u, 0u);

        for (uint32_t lev = 1; lev <= depth; ++lev) {
            uint32_t   childSz = 1u << (depth - lev);
            uint32_t   idx     = locate(ur, lev);
            Node*      child   = node->children[idx];
            glm::uvec3 cOrig(
                ur.x & ~(childSz - 1u),
                ur.y & ~(childSz - 1u),
                ur.z & ~(childSz - 1u)
            );

            if (child == nullptr) {
                // Empty subtree — mirrors GPU "!leaf.type" early-exit branch.
                tgtSz  = childSz ? childSz : 1u;
                tgtOrig = cOrig;
                break;
            }
            if (lev == depth) {
                // Deepest level reached: child IS the 1×1×1 voxel leaf.
                // Mirrors GPU post-loop "if (leaf.material != 0) return hit".
                if (child->material != 0) {
                    RayHit h;
                    h.hit      = true;
                    h.node     = child;
                    h.position = ur;
                    return h;
                }
                // Present but empty material — skip this 1-unit block.
                tgtSz  = 1u;
                tgtOrig = ur;
                break;
            }
            // Internal node — descend.
            node    = child;
            tgtSz   = childSz;
            tgtOrig = cOrig;
        }

        // --- intersect_inside: advance rpos to the exit of tgtSz block ---
        // Mirrors GPU intersect_inside(ray, target.position, target.position+size).
        // Crucially uses the FIXED `origin` (not rpos) for all t computations.
        glm::vec3 bmin = glm::vec3(tgtOrig);
        glm::vec3 bmax = bmin + glm::vec3(float(tgtSz));
        glm::vec3 t1   = (bmin - origin - 0.001f) * invDir;
        glm::vec3 t2   = (bmax - origin + 0.001f) * invDir;
        glm::vec3 tmi  = glm::min(t1, t2), tmx = glm::max(t1, t2);
        float t_exit   = glm::min(glm::min(tmx.x, tmx.y), tmx.z);
        rpos = direction * t_exit + origin;
    }

    return noHit;
}

// ---------------------------------------------------------------------------
// Batch insertion helpers
// ---------------------------------------------------------------------------

void OctreeCPU::insertBox(glm::uvec3 min, glm::uvec3 max, uint32_t material) {
    uint32_t bound = 1u << depth;
    max.x = glm::min(max.x, bound);
    max.y = glm::min(max.y, bound);
    max.z = glm::min(max.z, bound);
    for (uint32_t x = min.x; x < max.x; x++)
    for (uint32_t y = min.y; y < max.y; y++)
    for (uint32_t z = min.z; z < max.z; z++)
        insert(glm::uvec3(x, y, z), material);
}

void OctreeCPU::insertSphere(glm::vec3 centre, float radius, uint32_t material) {
    uint32_t bound = 1u << depth;
    int32_t x0 = glm::max(0,              (int32_t)glm::floor(centre.x - radius));
    int32_t y0 = glm::max(0,              (int32_t)glm::floor(centre.y - radius));
    int32_t z0 = glm::max(0,              (int32_t)glm::floor(centre.z - radius));
    int32_t x1 = glm::min((int32_t)bound - 1, (int32_t)glm::ceil(centre.x + radius));
    int32_t y1 = glm::min((int32_t)bound - 1, (int32_t)glm::ceil(centre.y + radius));
    int32_t z1 = glm::min((int32_t)bound - 1, (int32_t)glm::ceil(centre.z + radius));

    for (int32_t x = x0; x <= x1; x++)
    for (int32_t y = y0; y <= y1; y++)
    for (int32_t z = z0; z <= z1; z++) {
        glm::vec3 c = glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f);
        if (glm::distance(c, centre) <= radius)
            insert(glm::uvec3(x, y, z), material);
    }
}

void OctreeCPU::insertFunction(std::function<bool(glm::vec3)> fn,
                                glm::uvec3 min, glm::uvec3 max,
                                uint32_t material) {
    uint32_t bound = 1u << depth;
    max.x = glm::min(max.x, bound);
    max.y = glm::min(max.y, bound);
    max.z = glm::min(max.z, bound);
    for (uint32_t x = min.x; x < max.x; x++)
    for (uint32_t y = min.y; y < max.y; y++)
    for (uint32_t z = min.z; z < max.z; z++)
        if (fn(glm::vec3(x + 0.5f, y + 0.5f, z + 0.5f)))
            insert(glm::uvec3(x, y, z), material);
}