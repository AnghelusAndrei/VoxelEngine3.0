# Octree — Node Format & GPU Buffers

The scene's only runtime truth. A sparse 8-way tree of 64-bit nodes in one SSBO, traversed by a DDA
in `primary.comp` / `shade.comp` and edited from the CPU.

Frame flow: [PIPELINE.md](PIPELINE.md). Cache: [LBUFFER.md](LBUFFER.md). Materials: [USAGE.md](USAGE.md).

## Node format — 64 bits, AoS

Two 32-bit words (`lo`, `hi`) interleaved as `uvec2 nodes[]`. Eight nodes form one 8-slot block =
64 B = exactly one cache line; blocks are 8-slot aligned.

```
Lo: [ isNode:1 | material:10 | childmask:8 | version:8 | slotOffset:5 ]
Hi: [ next:32 ]
```

| Field | Bits | Internal node | Leaf |
|---|---|---|---|
| `isNode` | lo[0] | 1 | 0 |
| `material` | lo[1:10] | majority of children (representative) | the voxel's material |
| `childmask` | lo[11:18] | 1 bit per occupied octant | 0 |
| `version` | lo[19:26] | incarnation counter, wraps mod 256 | same |
| `slotOffset` | lo[27:31] | lBuffer probe distance, baked by `claim` | same |
| `next` | hi[0:31] | child-block base slot | 0 |

**The word is completely full.** Shifts and masks live in **one** place, `shd/constants.glsl`
(`NODE_*` / `MATERIAL_*`), injected into every shader; `Octree::Node` mirrors them and
`static_assert`s that the fields tile the word exactly with no overlap. Decode sites must use those
names — the word being full means a hardcoded shift that survives a re-layout reads a different field
with no error.

CPU and GLSL both use explicit shift/mask, never C++ bitfields (whose bit order is
implementation-defined).

### Field notes

**`material` is 10 bits (1024 materials, 0 = empty).** Its extra bits came from `slotOffset`, which
only ever holds a probe distance in `[0, LB_PROBE_LIMIT)` = `[0, 32)` — exactly 5 bits. `claim` guards
the bake with `off <= NODE_SLOTOFF_MASK`, so raising `LB_PROBE_LIMIT` past 32 degrades to a cache hole
rather than corrupting the node. Two consequences:

- The material pool is an **SSBO**, not a UBO: 1024 × 64 B = 64 KB is the whole of a typical
  `GL_MAX_UNIFORM_BLOCK_SIZE` and 4× the 16 KB the GL spec guarantees. Past 256 materials a uniform
  block is not portable.
- The virtual gbuffer's `.z` shares 32 bits between `material` and the `uniqueVoxelList` index, so 10
  material bits leave 22 — capping the virtual framebuffer at 4.19 M texels, only reachable at
  `virtualScale` 1 above ~2K. `level` in the gbuffer key is 8 bits for a depth that never exceeds 15,
  so 4 bits are recoverable there if it ever binds.

**`version`** is the third component of the lBuffer key alongside `(pos, level)`. It disambiguates
incarnations: a voxel modified while off-screen and returning at the same `(pos, level)` yields a
different hash key, so its stale cached entry is not reused and ages out via LRU. `makeLeaf` bumps it
on every insert.

**`slotOffset`** is written by `lbuffer_claim` and read by `lookupLBuffer`; it is the only field the
GPU writes. See [LBUFFER.md](LBUFFER.md) § Slot-offset baking for why it needs no validity bit, and
why any CPU node upload wipes it in bulk.

**`material` sits at the same bits for leaves and internal nodes**, so a coarse LOD hit can read a
representative material without knowing which it landed on.

---

## GPU buffers (owned by `Octree`)

| Buffer | Binding | Type | Size |
|---|---|---|---|
| node buffer | `SSBO_OCTREE_BINDING = 0` | `uvec2[]` | `capacity × 8 B` |
| claim bitfield | `SSBO_CLAIM_BINDING = 1` | `uint[]` | `⌈capacity/32⌉ × 4 B` |

Both are reallocated together in `resizeDataIfNeeded` (capacity doubles), so slot *N* always has a
backing claim bit. As SSBOs they are bounded by `GL_MAX_SHADER_STORAGE_BLOCK_SIZE` / VRAM, not by any
texture-buffer cap. The lBuffer (binding 2) is owned by `Renderer`, not `Octree`.

### Claim bitfield

One **bit** per octree node — 32× less memory than a `uint`-per-slot scheme. `downscale.comp` claims
each visible voxel exactly once per frame:

```glsl
uint w = gb.vid >> 5u, b = 1u << (gb.vid & 31u);
bool firstThisFrame = (atomicOr(claimbuf.bits[w], b) & b) == 0u;   // append if true
```

It must be **explicitly cleared each frame**, before `primary`. That clear is also what makes
`nodeListIdx`-style side structures unnecessary: any `vid` present in this frame's virtual gbuffer
necessarily won the TAS this frame.

### CPU dirty tracking

Edits mark a `[dirtyMin, dirtyMax)` slot range; `flushEdits` uploads it with `glBufferSubData`. The
CPU mirror (`Octree::data`) never carries `slotOffset`, so any upload zeroes that field across the
whole range — benign (it degrades to a cache hole) but it is why off-screen readers use `findLBuffer`.

`remapMaterials(remap)` rewrites every node's material through a mapping and marks the nodes dirty.
It is the other half of `MaterialPool::collapse`: without it, compacting the pool silently repaints
everything already in the tree.

---

## Representative material

On every `insert`/`remove` the descent path is recorded and materials recompute **bottom-up**
(`recomputeMaterial` / `propagateMaterialUp`):

- An internal node's `material` = the **majority** over its non-empty direct children (ties → lowest
  id). An internal child contributes its own already-propagated representative — one vote, regardless
  of subtree size.
- **Early-out:** stop climbing once a node's material is unchanged; a parent's majority can only shift
  if a child's did. Common edit is O(1) amortised, worst case O(8·depth).
- `remove` propagates to the root even when no block is freed, since a removal can shift majorities
  all the way up. Freed ancestors are skipped.

Consumed by the DDA's `maxSteps` fallback, which returns the last coarse node rather than nothing.

---

## Traversal

`internal.glsl` holds the DDA shared by `primary` and `shade`. It keeps a per-thread descent stack in
**shared memory**:

```glsl
shared uvec2 gs_stack[MAXDEPTH][64];     // transposed: [depth][thread], no bank conflicts
```

512 B per level per workgroup. Shared memory is what caps occupancy in `primary`/`shade`, so
`MAXDEPTH` is injected by the host as `octreeDepth + 1` — the exact bound, since the descent indexes
`gs_stack[d]` for `d` up to `effDepth == octreeDepth`. Sizing it to the compile-time `maxDepth`
ceiling instead is a direct, invisible occupancy loss:

| Stack | Shared/workgroup | Workgroups/SM (64 KB) | Warps/SM (of 48) |
|---|---|---|---|
| `MAXDEPTH 15` (ceiling) | 7.5 KB | 8 | 16 |
| `MAXDEPTH 10` (depth 9) | 5 KB | 12 | 24 |

`internal.glsl` has **no default** for `MAXDEPTH` — it `#error`s. A silent fallback would still
compile and still render correctly, just at half occupancy.

The walk uses a bitwise XOR of successive integer positions to find the common ancestor and resume
the descent from there, rather than restarting at the root each step, and skips a node fetch entirely
when the parent's `childmask` says the octant is empty.

`Octree::raycast` mirrors it on the CPU for picking (click-to-edit).

---

## Capacity

`next` is a full 32-bit slot index (~4.29 B slots), so the pointer does not limit depth — **VRAM
does**. Each +1 depth on a surface-heavy scene is ≈ 4–8× the slots.

| Depth | Voxels | Slots | `capacity` | Node SSBO | Claim |
|---|---|---|---|---|---|
| 8 | 3.54 M | 4.19 M | 2²² = 4.19 M | 33.5 MB | 0.5 MB |
| 9 | 23.0 M | 26.9 M | 2²⁵ = 33.6 M | 268 MB | 4.2 MB |

The shipped scene is Sponza at depth 10, resolution 1000, thickness 3 — 24.6 M voxels.

`maxDepth` is a compile-time ceiling of 15; the gbuffer packs positions in 16 bits, so 2¹⁵ = 32768
fits.
