1. GENERAL SYSTEM ARCHITECTURE & CODE CONVENTIONS
A. Specification & Verification Protocol
Detail-First Implementation: Architecture discussions must prioritize memory footprints, data alignment, byte strides, cache mechanics, and execution barriers over high-level abstractions.

Visual Validation: Every complex state machine, data-stream, or GPU compute pass sequence must be explicitly mapped in a Mermaid diagram within its respective ./architecture/*.md document.

B. OOP Design & Generalization Philosophies
Open-Closed Principle: Core subsystems must be open for extension but closed for modification via abstract, interface-driven layers. Swapping a rendering backend or simulation pass must not break the engine core.

Decoupled Architecture: Keep memory management (VRAM allocations, buffer lifecycles, texture streaming) rigidly separated from execution context loops and compute dispatches.

Future-Proof Interfaces: Abstract repetitive buffer bindings, descriptor updates, and execution barriers into lightweight, modern wrappers that inherently support dynamic sizing, indirect argument parameters, and variable-length array bindings.

C. Structural Cleanliness & DOD
Data-Oriented Optimization (DOD): Utilize OOP structures for high-level module interfaces, but enforce Data-Oriented Design (SoA/AoS flat contiguous memory) along critical execution and processing paths to maximize cache efficiency.

Interface Uniformity: All pipeline execution passes must strictly inherit or conform to a uniform lifecycle (Initialize(), Update(), RecordCommands(), Destroy()).

D. Tunables Are Not Solutions
A tunable knob (uniform / UI slider) is a last resort or a debug aid — never the primary fix for a defect. Knobs are legitimate ONLY for genuine tradeoffs the user must steer per scene or per goal (convergence speed vs. performance vs. responsiveness). They are NEVER acceptable as a way to tune for *correctness*: if a result is only acceptable at a hand-found parameter value, the estimator/algorithm is wrong — fix the math, do not expose the error as a slider. A knob does not constitute a universal solution; it pushes the bug onto the user and adds tuning-surface complexity that every future change must respect. Prefer a parameter-free, provably-correct mechanism even if it is more work.

E. Complexity Must Justify Its Own Weight
Before adding any abstraction, class, buffer, pass, or code path, weigh it against the complexity it imposes on EVERY future change that must thereafter respect it. If an addition does not clearly earn that weight — a measured win, a real new capability, or a removed failure mode — the default answer is NO; think three times before committing, and expect that most speculative additions fail this test (instanced/transformed objects when a bake into the master octree suffices; a parallel structure when an existing one can be extended; a derived table when the source can be sampled directly; a knob when the math can be fixed). This is NOT a mandate for timidity: be ambitious in capability and correctness — but pay for ambition with capability, never with gratuitous structure. Prefer extending one concept over splitting it across two; prefer deleting a derived structure over keeping it in sync. Ambition is earned by what the engine can DO, not by how much scaffolding it carries.

2. LOW-LEVEL GPU COMPUTE RULES
A. Concurrency & Deadlock Avoidance
ZERO SPINLOCKS: Do not write while(locked) or spin-loops inside compute shaders. Threads sharing a wave/warp share an instruction pointer; spinlocks will cause instant Intra-Warp Deadlocks and trigger Windows TDR/GPU crashes.

TRY-CLAIM, DON'T SPIN: a contended atomic claim must resolve in a single pass — CAS on a per-slot token (the shipped claim uses the timestamp word), and on failure advance to the next candidate slot and exit; never loop waiting on a slot. The lock-free open-addressed claim does exactly this (see CLAIM.md); the earlier RetryBuffer + CPU-orchestrated DispatchIndirect scheme was abandoned as unnecessary.

B. Wave-Level Optimization
Prioritize wave-intrinsics and Wave Compaction over global atomics when aggregating data across screen tiles, wavefront queues, or active pixel slots.

Accumulate data into thread group shared memory (groupshared) or wave-level registers before issuing atomic operations to global VRAM.

C. Structure Packing & Cache Preservation
Structure layouts must align cleanly to 64-byte L2 cache lines. Keep strides tight and eliminate padding gaps.

Bit-pack data aggressively to conserve memory bandwidth (e.g., compress data into packed formats like R11G11B10_FLOAT, RGB9E5, or 10-bit octahedral normals).

3. HARDWARE & ALGORITHMIC PREREQUISITES
Solutions, optimizations, and data structures generated must reflect explicit mastery of:

Modern Hardware Topologies: GPU L1/L2 cache scaling, PCIe Gen 4/5 throughput limits, VRAM banking, register pressure, and occupancy metrics.

Advanced Ray Resampling: Spatio-Temporal Importance Resampling (ReSTIR DI/GI/PT) structures and spatial/temporal shift-mapping math.

Out-of-Core Virtualization: Virtual texturing page-tables, asynchronous asset streaming, and compute-driven page-fault feedback systems.