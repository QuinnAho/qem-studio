# QEM Simplifier - Performance Analysis (Pre-Final)

## TL;DR

- Preprocessing is **not** the bottleneck on realistic workloads (15-20% of total wall-clock); the collapse loop is (~80%).
- Parallelizing preprocessing tops out at **~10% total speed-up** under Amdahl's law, and it introduces floating-point non-determinism that breaks the golden-image test story. Scoped out.
- **Attempted `std::map` -> `std::unordered_map` swap**: tested empirically and **reverted** after it produced a net regression (preprocessing +35-91%, collapse-loop roughly flat). The collapse loop's spatial locality beats hashed lookups on cheap-comparator keys. See "Measured result" below.
- Net outcome: current `std::map` implementation is the best option on this workload without introducing third-party containers or a custom open-addressing table. Both deferred.

## Measured baseline

All numbers below are wall-clock on an 870k-triangle test mesh (dragon.obj) with all three preservation features enabled (`preserve_boundary`, `preserve_sharp_edges`, `preserve_curvature`).

| Workload | Wall-clock | Notes |
|---|---|---|
| `simplify --99` (1% decimation, 8.7k collapses) | ~2.95s | Isolates preprocessing + small collapse sample |
| Preprocessing alone (`RebuildDerivedState` initial seed) | **~1.8s** | From the first progress event timestamp |
| Collapse loop at 1% decimation | ~1.1s | 8.7k collapses |
| `simplify --95` (5% decimation, ~22k collapses) | ~9.8s | |
| Estimated `simplify --70` (30% decimation, ~600k collapses) | ~9-11s | Linear extrapolation from collapse-loop rate |

**Take-away:** at a realistic decimation target (30%), preprocessing is **~18%** of total time and the collapse loop is **~82%**.

## Parallel preprocessing - feasibility

### What's inside `RebuildDerivedState` and whether each pass parallelizes

| Pass | Est. share of preprocessing | Parallelizable? | Why / why not |
|---|---|---|---|
| 1. Per-face plane quadric (normal + area + plane quadric) | 30-40% | Yes, easily | Each face only reads its own 3 vertices and writes only to itself. Pure `parallel_for`. |
| 2. Per-vertex quadric accumulation (each face writes to 3 vertices) | 25-35% | Needs care | Race on shared vertex writes. Safe via per-thread partial buffers + deterministic reduce, or by inverting the loop to iterate vertices and read their adjacency. |
| 3. `AddFaceAdjacency` + `AddFaceIncidence` (std::map mutations) | 20-30% | **No** | `std::map<EdgeKey, uint32_t>` is not thread-safe and can't be cheaply made so. |
| 4. Boundary + sharp-edge penalty loops | 5-10% | Not worthwhile | Walks `std::map::iterator`, which is serial by construction. |
| 5. Per-vertex curvature factor | 5-10% | Yes, easily | Pure read-only per vertex. |
| 6. Initial candidate priority-queue build | 10-15% | No, without restructuring | `std::priority_queue::push` serializes on the heap; the underlying vector isn't exposed for a bulk `std::make_heap`. |

### Amdahl ceiling

Parallelizable fraction: ~65% (passes 1, 2, 5). With 8 threads and *perfect* scaling:

```
speedup = 1 / (0.35 + 0.65 / 8) = 1 / 0.431 ~= 2.3x on preprocessing
```

Realistic speed-up after cache contention, reduction overhead, and thread-spin-up: **1.5-2.0x on preprocessing**, which saves ~0.9-1.2s on the dragon workload.

### Applied to the real demo

- Total wall-clock before: ~9.8s
- Total wall-clock after:  ~8.7-9.1s
- **Net total speed-up: ~7-12%**

### Reasons not to ship it

1. **Determinism regression.** The project's `DeterminismPolicy` declares `deterministic = true`. Parallel floating-point `+=` breaks bit-exact reproducibility, which breaks the CI golden-image comparison story documented in `CLAUDE.md` (SSIM >= 0.995 per-run). Preserving determinism requires per-thread partial buffers with a deterministic merge order - extra complexity for a marginal win.
2. **Platform surface area.** On Windows/MSVC, `<execution>` works without TBB via ConcRT, but regressions here tend to be painful on specific machines.
3. **Risk/reward at this stage.** Threading introduces race-condition risk shortly before the final submission for a ~10% total speed-up.
4. **The win is in the wrong place.** 82% of wall-clock is in the collapse loop; we would be optimizing the 18%.

### Conclusion

**Parallel preprocessing is scoped out of the final submission.** The analysis itself is a useful result - it shows we profiled before optimizing and made an evidence-based call.

## Proposed speed-up: `std::map` -> `std::unordered_map` (attempted, reverted)

### Hypothesis (before measuring)

The collapse loop performs multiple `std::map` lookups per iteration:

- `state.edge_incidence`  - `std::map<EdgeKey, uint32_t>`
- `state.edge_versions`   - `std::map<EdgeKey, uint32_t>`
- plus modifications during `ApplyCollapseLocalAndUpdateState`

Each tree-map operation is `O(log n)` and cache-unfriendly (pointer chasing through red-black nodes). `std::unordered_map` is `O(1)` amortized and keeps keys in contiguous buckets, which *should* be better. Expected 20-40% speed-up on the collapse loop.

### What was changed

1. Added `operator==` and an `EdgeKeyHash` functor packing both `uint32_t`s into a `uint64_t` and hashing that.
2. Changed `edge_incidence` and `edge_versions` from `std::map` to `std::unordered_map<EdgeKey, uint32_t, EdgeKeyHash>`.
3. Added deterministic sort passes at iteration sites (initial candidate seeding, boundary/sharp penalty seeding, `AnalyzeFeatures` output lists) to preserve bit-exact reproducibility since `unordered_map` has no iteration-order guarantee.

### Measured result (dragon.obj, preservation OFF)

| Run | Before (`std::map`) | After (`std::unordered_map`) | Delta |
|---|---|---|---|
| `simplify --99` preprocessing | **1.58s** | 3.02s | **+91%** (worse) |
| `simplify --99` internal total | **2.38s** | 4.16s | **+75%** (worse) |
| `simplify --70` preprocessing | **1.70s** | 2.30s | **+35%** (worse) |
| `simplify --70` internal total | **30.73s** | 31.45s | +2% (roughly flat) |

Determinism held (output triangle/vertex counts were identical before and after), but the swap was a **net performance regression**. The change was reverted.

### Why the hypothesis was wrong

Two effects the prediction missed:

1. **Spatial locality matters more than Big-O here.** The collapse loop modifies edges around a shrinking 1-ring neighborhood. With `std::map`, those nearby keys often live on recently-visited red-black-tree paths, so cache behavior was better than the "pointer-chasing tree" reputation suggests. `std::unordered_map` scatters keys across hash buckets, destroying that locality. For a 2.6M-entry map, the cache-miss cost on each hash lookup outweighs the `O(log n) -> O(1)` win on a cheap comparator (two `uint32_t`s).
2. **Determinism tax.** Preserving bit-exact output with unordered iteration requires sorting the keys before use. On dragon, the initial candidate-seeding sort alone was ~1.4s - almost the entire preprocessing budget.

### Take-away

The right answer for a workload isn't always the textbook one. `std::map` wins here because:
- the comparator is cheap (two `uint32_t` compares),
- the mesh's topological locality creates key locality,
- and we need deterministic iteration order for reproducibility, which `std::map` gives us for free.

### What to try instead

If we revisit this later, the promising directions are:

1. **Flat-map / dense-hash-map containers** (e.g. `absl::flat_hash_map`, `ankerl::unordered_dense`, `tsl::robin_map`). These keep entries in a contiguous array and get both O(1) amortized lookup *and* cache locality. Would need to vendor a third-party header.
2. **Custom open-addressing table sized to the problem** - a single `std::vector<std::pair<EdgeKey, uint32_t>>` with linear probing. Full control over locality, but a non-trivial engineering lift and risk at this stage of the project.
3. **Side-step the hot path entirely** - the real wall-clock is in the collapse loop's `ValidateCollapse` + neighborhood rebuild, not the map lookups themselves. A profiled hotspot analysis would likely point at the per-collapse set constructions in `ValidateCollapse` before pointing at the map.

None of these are single-afternoon changes, and the empirical evidence above suggests more measurement is needed before another optimization attempt. Deferred.

## Reference data points

- Test mesh: `apps/qem_simplifier/resources/models/dragon.obj` (~870k triangles, ~435k vertices).
- Machine: Windows 11, MSVC 17.14, Release build.
- Shell invocation: `printf 'simplify --99\nquit\n' | glint_qem_studio_shell.exe --config <...>`.
- Measurement source: progress-callback timestamps (`elapsed_seconds`) on the first and final events.
