// 64-bit node, AoS in octree.nodes[] (uvec2 per slot: .x = lo, .y = hi).
//   Lo: [ isNode:1 | material:7 | childmask:8 | reserved:16 ]   Hi: [ next:32 ]
// Mirrors Octree::Node on the CPU — keep both sides on these explicit masks.
const float inv_127 = 1.0 / 127.0;
#define MAXDEPTH 12
uint octreeLength; // Assume this is provided by uniform/buffer

struct Node {
    bool type;
    uint childmask, next, material, version;
};

struct leaf_t { uint size; vec3 position; };
struct hit_t { bool hit; uint id, material; uvec3 position; uint size; uint version; uint steps; };
struct ray_t { vec3 origin, direction, inverted_direction; };

Node UnpackNode(uvec2 raw) {
    uint lo = raw.x;
    return Node(
        bool(lo & 1u),            // type     (isNode, bit 0)
        (lo >> 8u) & 0xFFu,       // childmask (bits 8..15)
        raw.y,                    // next     (full 32-bit hi word)
        (lo >> 1u) & 0x7Fu,       // material (bits 1..7)
        (lo >> 16u) & 0xFFu       // version  (bits 16..23)
    );
}

bool inBounds(vec3 v, float n) { 
    return all(lessThanEqual(vec3(0.0), v)) && all(lessThanEqual(v, vec3(n)));
}

uint locate(uvec3 pos, uint p2) { 
    return (uint(bool(pos.x & p2)) << 2u) | (uint(bool(pos.y & p2)) << 1u) | uint(bool(pos.z & p2));
}

// Bounding box intersection used for initial SVO entry
vec4 intersect(ray_t r, vec3 box_min, vec3 box_max) {
    vec3 t1 = (box_min - r.origin + 0.001) * r.inverted_direction;
    vec3 t2 = (box_max - r.origin - 0.001) * r.inverted_direction;
    vec3 tmin = min(t1, t2), tmax = max(t1, t2);
    
    float t_enter = max(max(tmin.x, tmin.y), tmin.z);
    float t_exit  = min(min(tmax.x, tmax.y), tmax.z);
    
    if (t_exit < t_enter || t_exit < 0.0) return vec4(0.0, 0.0, 0.0, -1.0);
    return vec4(r.direction * t_enter + r.origin, 1.0);
}

// Stack Transposed: gs_stack[depth][threadIdx]
// This completely eliminates shared memory bank conflicts across the warp.
// Stores the raw uvec2 node so both childmask (.x) and next (.y) are available
// on the walk-up without a re-fetch.
shared uvec2 gs_stack[MAXDEPTH][64];

// coarseOnExhaust: on step-budget exhaustion, return the last (coarse) node as a hit. TRUE for
// primary / bounce rays (a plausible far-geometry stand-in). FALSE for occlusion (shadow) rays —
// they must report a clean MISS on exhaustion, never fabricate a hit at a coarse position, which
// would otherwise let the strict `hit.position == lightVoxel` test spuriously pass/fail.
hit_t Raycast(ray_t ray, uint maxDepth, uint maxSteps, float originOffset, bool coarseOnExhaust) {
    uint effDepth = (maxDepth < octreeDepth) ? maxDepth : octreeDepth;
    uint threadIdx = gl_LocalInvocationIndex;

    // Precalculate ray stepping mask for O(1) exit plane calculation
    // 1.0 if direction is positive, 0.0 if negative
    vec3 ray_step_mask = step(vec3(0.0), ray.direction);

    // Advance the origin along the ray before traversal. Primary rays pass a few
    // units (cheap near-clip from the camera); surface-origin bounce rays pass 0 so
    // they cannot skip nearby occluders (light leak) — they pre-offset off the face.
    ray.origin += ray.direction * originOffset;

    vec3 r_pos;
    uint q = 0u;
    
    // Initial bounds test
    if (inBounds(ray.origin, float(octreeLength))) {
        r_pos = ray.origin;
    } else {
        vec4 isect = intersect(ray, vec3(0.0), vec3(float(octreeLength)));
        q++;
        if (isect.w < 0.0) return hit_t(false, 0u, 0u, uvec3(0u), 0u, 0u, 0u);
        r_pos = isect.xyz;
    }

    // Seed stack with root (slot 0); handle degenerate single-leaf root.
    {
        uvec2 root_raw = octree.nodes[0];
        Node root_node = UnpackNode(root_raw);
        if (!root_node.type) {
            if (root_node.material != 0u)
                return hit_t(true, 0u, root_node.material, uvec3(0u), octreeLength, root_node.version, q);
            return hit_t(false, 0u, 0u, uvec3(0u), 0u, 0u, q);
        }
        gs_stack[0][threadIdx] = root_raw;
    }

    uvec3 old_ur_pos = uvec3(~0u);
    vec3  target_pos = vec3(0.0);
    uint  target_size = octreeLength;

    while (inBounds(r_pos, float(octreeLength)) && q < maxSteps) {
        q++;
        uvec3 ur_pos = uvec3(r_pos);

        // Bitwise XOR to find the common ancestor and detect coordinate changes
        uint diff = (ur_pos.x ^ old_ur_pos.x) | (ur_pos.y ^ old_ur_pos.y) | (ur_pos.z ^ old_ur_pos.z);
        if (diff == 0u) {
            // Numerical stall fallback: advance by a fraction of the current voxel size
            // so it scales correctly at every octree depth, then re-enter the DDA.
            r_pos += ray.direction * (float(target_size) * 1e-3);
            continue;
        }

        // Deepest level at which old and new positions share a cell
        uint diff_bit   = uint(findMSB(diff));
        uint resume_depth = (diff_bit >= effDepth) ? 1u : (effDepth - diff_bit);

        // Extract block base (next ptr) from the parent node on the stack
        uint offset = gs_stack[resume_depth - 1u][threadIdx].y;
        Node node; uint slot;

        for (uint d = resume_depth; d <= effDepth; d++) {
            target_size = octreeLength >> d; // Replaced dynamic p2c array allocation
            uint childIdx = locate(ur_pos, target_size);

            // Skip the node fetch when parent childmask indicates an empty octant
            uint parent_mask = (gs_stack[d - 1u][threadIdx].x >> 8u) & 0xFFu;
            if ((parent_mask & (1u << childIdx)) == 0u) {
                target_pos = vec3(uvec3(ur_pos) & ~uvec3(target_size - 1u));
                break;
            }

            slot = offset + childIdx;
            uvec2 raw = octree.nodes[slot];
            node = UnpackNode(raw);
            
            target_pos = vec3(uvec3(ur_pos) & ~uvec3(target_size - 1u));

            if (!node.type) {
                if (node.material != 0u)
                    return hit_t(true, slot, node.material, uvec3(target_pos), target_size, node.version, q);
                break; // Empty leaf — advance past it
            }

            // Internal node: push to bank-conflict-free stack and descend
            gs_stack[d][threadIdx] = raw;
            offset = node.next;
        }
        if (coarseOnExhaust && q == maxSteps-1u) { // Early exit if maxSteps reached, return last coarse node since material representative is stored.
            if (node.material != 0u)
                return hit_t(true, slot, node.material, uvec3(target_pos), target_size, node.version, q);
        }

        old_ur_pos = ur_pos;

        // Optimized DDA advancement (Replaces intersect_inside)
        // Uses the precomputed ray_step_mask to calculate distance to the 3 forward exit planes
        vec3 exit_planes = target_pos + ray_step_mask * float(target_size);
        vec3 t_exit = (exit_planes - r_pos) * ray.inverted_direction;

        // The intersection t is the closest of the 3 forward planes
        float t_next = min(min(t_exit.x, t_exit.y), t_exit.z);

        // Step to the exit plane, then nudge axis-aligned past the face.
        // Axis-aligned nudge avoids the grazing-ray precision failure of
        // direction-scaled stepping: for a nearly-parallel ray (direction[i] ≈ 0),
        // direction * epsilon contributes < ULP(r_pos[i]) in the crossing axis,
        // leaving r_pos.i on the wrong side of the integer boundary.
        // Scaling by target_size keeps accuracy consistent at every octree depth.
        r_pos += ray.direction * t_next;
        bvec3 is_exit = lessThanEqual(t_exit, vec3(t_next) * (1.0 + 1e-4) + 1e-5);
        r_pos += vec3(is_exit) * (ray_step_mask * 2.0 - 1.0) * float(target_size) * 1e-4;
    }

    return hit_t(false, 0u, 0u, uvec3(0u), 0u, 0u, q);
}