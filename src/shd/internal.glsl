// 64-bit node, AoS in octree.nodes[] (uvec2 per slot: .x = lo, .y = hi).
//   Lo: [ isNode:1 | material:10 | childmask:8 | version:8 | slotOffset:5 ]  Hi: [ next:32 ]
//   Field shifts/masks live in constants.glsl (NODE_* / MATERIAL_*).
// Mirrors Octree::Node on the CPU — keep both sides on these explicit masks.
const float inv_127 = 1.0 / 127.0;

#ifndef MAXDEPTH
#error "MAXDEPTH not defined - compile this through shader::compile with shaderDefines"
#endif
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
        bool(lo & NODE_ISNODE_BIT),
        (lo >> NODE_CM_SHIFT)  & NODE_CM_MASK,    // childmask
        raw.y,                                    // next (full 32-bit hi word)
        (lo >> NODE_MAT_SHIFT) & MATERIAL_MASK,   // material
        (lo >> NODE_VER_SHIFT) & NODE_VER_MASK    // version
    );
}

bool inBounds(vec3 v, float n) { 
    return all(lessThanEqual(vec3(0.0), v)) && all(lessThanEqual(v, vec3(n)));
}

uint locate(uvec3 pos, uint p2) { 
    return (uint(bool(pos.x & p2)) << 2u) | (uint(bool(pos.y & p2)) << 1u) | uint(bool(pos.z & p2));
}

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

hit_t Raycast(ray_t ray, uint maxDepth, uint maxSteps, float originOffset) {
    uint effDepth = (maxDepth < octreeDepth) ? maxDepth : octreeDepth;
    uint threadIdx = gl_LocalInvocationIndex;

    // Precalculate ray stepping mask for O(1) exit plane calculation
    // 1.0 if direction is positive, 0.0 if negative
    vec3 ray_step_mask = step(vec3(0.0), ray.direction);

    // cheap near-clip from the camera
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

    // Hoisted out of the loop so they are initialised ONCE, not per DDA step — the descent
    // below reads them only after assigning, so this costs the walk nothing.
    // They must exist across iterations because the maxSteps fallback reads them, and the
    // descent can `break` on an empty octant BEFORE assigning either (the parent-childmask
    // early-out skips the node fetch). Declared inside the loop they were then read
    // uninitialised, so the fallback could fabricate a hit from a garbage material and hand
    // back a garbage `id` — which callers now use to index octree.nodes[] for the baked slot
    // offset. material 0 is the canonical "no hit", so the fallback simply cannot fire until
    // a real node has been descended into.
    Node  node = Node(false, 0u, 0u, 0u, 0u);
    uint  slot = 0u;

    while (inBounds(r_pos, float(octreeLength)) && q < maxSteps) {
        q++;
        uvec3 ur_pos = uvec3(r_pos);

        // Bitwise XOR to find the common ancestor and detect coordinate changes
        uint diff = (ur_pos.x ^ old_ur_pos.x) | (ur_pos.y ^ old_ur_pos.y) | (ur_pos.z ^ old_ur_pos.z);
        if (diff == 0u) {
            // Numerical stall fallback: advance by a fraction of the current voxel size
            // so it scales correctly at every octree depth, then re-enter the DDA.
            r_pos += ray.direction * (float(target_size) * 1e-2);
            continue;
        }

        // Deepest level at which old and new positions share a cell
        uint diff_bit   = uint(findMSB(diff));
        uint resume_depth = (diff_bit >= effDepth) ? 1u : (effDepth - diff_bit);

        // Extract block base (next ptr) from the parent node on the stack
        uint offset = gs_stack[resume_depth - 1u][threadIdx].y;

        for (uint d = resume_depth; d <= effDepth; d++) {
            target_size = octreeLength >> d; // Replaced dynamic p2c array allocation
            uint childIdx = locate(ur_pos, target_size);

            // Skip the node fetch when parent childmask indicates an empty octant
            uint parent_mask = (gs_stack[d - 1u][threadIdx].x >> NODE_CM_SHIFT) & NODE_CM_MASK;
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
        if (q == maxSteps-1u) { // Early exit if maxSteps reached, return last coarse node since material representative is stored.
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
        // Scaling by target_size keeps accuracy consistent at every octree depth.
        r_pos += ray.direction * t_next;
        bvec3 is_exit = lessThanEqual(t_exit, vec3(t_next) * (1.0 + 1e-4) + 1e-5);
        r_pos += vec3(is_exit) * (ray_step_mask * 2.0 - 1.0) * float(target_size) * 1e-4;
    }

    return hit_t(false, 0u, 0u, uvec3(0u), 0u, 0u, q);
}