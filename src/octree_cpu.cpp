#include "octree_cpu.hpp"
#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Level convention (matches the GPU exactly):
//
//   insertR is called with level=1 for the root node.
//   It calls locate(pos, level) at every level from 1 to depth,
//   so all 'depth' bits of each coordinate are consumed.
//   The LEAF is the child reached after the level==depth locate call,
//   i.e. the node that insertR receives when level == depth+1.
//
//   This gives 2^depth unique voxel positions per axis (e.g. 256 for depth=8),
//   matching the GPU tree that also performs 'depth' locate operations.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

OctreeCPU::OctreeCPU(uint8_t depth_) {
    depth = depth_;
    root  = nullptr;

    // utils_p2r[level] = bitmask for the coordinate bit tested at that level.
    // Level 1 = MSB of the coordinate range, level depth = LSB (bit 0).
    uint32_t p2r = 1;
    for (int i = depth; i >= 1; i--) {
        utils_p2r[i] = p2r;
        p2r <<= 1;
    }
    utils_p2r[0] = p2r; // unused but initialised to avoid UB
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

// ---------------------------------------------------------------------------
// locate — identical bit-extraction to the GPU shader
// ---------------------------------------------------------------------------

uint32_t OctreeCPU::locate(glm::uvec3 pos, uint32_t level) const {
    uint32_t p2 = utils_p2r[level];
    return (uint32_t(bool(pos.x & p2)) << 2) |
           (uint32_t(bool(pos.y & p2)) << 1) |
           uint32_t(bool(pos.z & p2));
}

// ---------------------------------------------------------------------------
// insert
// ---------------------------------------------------------------------------

void OctreeCPU::insert(glm::uvec3 position, uint32_t material, glm::vec3 normal) {
    uint32_t bound = 1u << depth;
    if (position.x >= bound || position.y >= bound || position.z >= bound)
        return;
    if (!root) root = new Node();
    insertR(position, material, normal, root, 1);

    // Mark the 6 face-neighbours dirty — their normals change when a voxel
    // is added or replaced next to them.
    const glm::ivec3 faces[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    for (auto& f : faces) {
        glm::ivec3 np = glm::ivec3(position) + f;
        if (np.x < 0 || np.y < 0 || np.z < 0) continue;
        if ((uint32_t)np.x >= bound || (uint32_t)np.y >= bound || (uint32_t)np.z >= bound) continue;
        Node* nb = lookup(glm::uvec3(np));
        if (nb) nb->normalDirty = true;
    }
}

void OctreeCPU::insertR(glm::uvec3 pos, uint32_t material, glm::vec3 normal,
                         Node*& node, uint8_t level) {
    if (!node) node = new Node();

    // level > depth means we have descended all 'depth' bits and this
    // node IS the individual voxel leaf.
    if (level > depth) {
        node->material = material;
        if (glm::length(normal) > 0.001f) {
            node->normal      = glm::normalize(normal);
            node->normalDirty = false;
        } else {
            node->normalDirty = true;
        }
        return;
    }

    uint32_t idx         = locate(pos, level);
    bool     childExisted = (node->children[idx] != nullptr);
    insertR(pos, material, normal, node->children[idx], level + 1);

    // Increment childrenCount only when a genuinely new child was created.
    if (!childExisted && node->children[idx])
        node->childrenCount++;
}

// ---------------------------------------------------------------------------
// remove
// ---------------------------------------------------------------------------

void OctreeCPU::remove(glm::uvec3 position) {
    uint32_t bound = 1u << depth;
    if (position.x >= bound || position.y >= bound || position.z >= bound)
        return;
    if (!root) return;
    removeR(position, root, 1);

    // Dirty the face-neighbours so their normals are recomputed on next set().
    const glm::ivec3 faces[6] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    for (auto& f : faces) {
        glm::ivec3 np = glm::ivec3(position) + f;
        if (np.x < 0 || np.y < 0 || np.z < 0) continue;
        if ((uint32_t)np.x >= bound || (uint32_t)np.y >= bound || (uint32_t)np.z >= bound) continue;
        Node* nb = lookup(glm::uvec3(np));
        if (nb) nb->normalDirty = true;
    }
}

void OctreeCPU::removeR(glm::uvec3 pos, Node*& node, uint8_t level) {
    if (!node) return;

    if (level > depth) {
        delete node;
        node = nullptr;
        return;
    }

    uint32_t idx      = locate(pos, level);
    bool     hadChild = (node->children[idx] != nullptr);
    removeR(pos, node->children[idx], level + 1);

    if (hadChild && !node->children[idx]) {
        node->childrenCount--;
        if (node->childrenCount == 0) {
            delete node;
            node = nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// lookup
// ---------------------------------------------------------------------------

OctreeCPU::Node* OctreeCPU::lookup(glm::uvec3 pos) const {
    uint32_t bound = 1u << depth;
    if (pos.x >= bound || pos.y >= bound || pos.z >= bound) return nullptr;

    Node* node = root;
    // Traverse all depth levels; the node that arrives at level > depth is the leaf.
    for (uint8_t level = 1; level <= depth + 1; level++) {
        if (!node) return nullptr;
        if (level > depth) return node;   // reached the leaf
        uint32_t idx = locate(pos, level);
        node = node->children[idx];
    }
    return nullptr; // unreachable but silences warnings
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

// ---------------------------------------------------------------------------
// Normal computation
// ---------------------------------------------------------------------------

void OctreeCPU::computeNormals(int radius) {
    if (!root) return;
    _normalRadius = radius;
    computeNormalsR(root, glm::uvec3(0), 1);
}

void OctreeCPU::computeNormalsR(Node* node, glm::uvec3 origin, uint8_t level) {
    if (!node) return;

    if (level > depth) {
        // This node IS the voxel leaf.
        if (!node->normalDirty || node->material == 0) return;

        glm::vec3 accum(0.0f);
        int32_t   bound = (int32_t)(1u << depth);
        int32_t   ox    = (int32_t)origin.x;
        int32_t   oy    = (int32_t)origin.y;
        int32_t   oz    = (int32_t)origin.z;

        for (int dx = -_normalRadius; dx <= _normalRadius; dx++)
        for (int dy = -_normalRadius; dy <= _normalRadius; dy++)
        for (int dz = -_normalRadius; dz <= _normalRadius; dz++) {
            if (dx == 0 && dy == 0 && dz == 0) continue;
            int32_t nx = ox + dx, ny = oy + dy, nz = oz + dz;
            bool outOfBounds = (nx < 0 || ny < 0 || nz < 0 ||
                                nx >= bound || ny >= bound || nz >= bound);
            bool empty = outOfBounds;
            if (!outOfBounds) {
                Node* nb = lookup(glm::uvec3((uint32_t)nx, (uint32_t)ny, (uint32_t)nz));
                empty = (!nb || nb->material == 0);
            }
            if (empty) accum += glm::vec3((float)dx, (float)dy, (float)dz);
        }

        node->normal      = (glm::length(accum) > 0.001f)
                                ? glm::normalize(accum)
                                : glm::vec3(0.0f, 1.0f, 0.0f);
        node->normalDirty = false;
        return;
    }

    // Internal node — recurse with computed child origins.
    uint32_t half = utils_p2r[level];
    for (int i = 0; i < 8; i++) {
        if (!node->children[i]) continue;
        glm::uvec3 childOrigin = origin;
        if (i & 4) childOrigin.x += half;
        if (i & 2) childOrigin.y += half;
        if (i & 1) childOrigin.z += half;
        computeNormalsR(node->children[i], childOrigin, level + 1);
    }
}
