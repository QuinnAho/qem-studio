# Preservation Flags: Empirical Observations

The three preservation flags (`preserve_boundary`, `preserve_sharp_edges`,
`preserve_curvature`) are not a free win. Whether they help or hurt depends
heavily on **what kind of mesh you are decimating**.

## TL;DR

| Mesh type | Without preservation | With preservation |
|---|---|---|
| Single smooth organic model (bunny, cow) | Clean, sometimes slightly nicer-looking silhouette | Can look *marginally worse* on mid-decimation; fine on aggressive decimation |
| Whole scene or architectural mesh (sponza) | Scene geometry vanishes at moderate decimation; walls collapse, ceilings dissolve | Scene shape survives; the "building" stays a "building" |

**Rule of thumb: preservation pays off more the less uniform the mesh is.**
A scanned bust doesn't benefit much; a compound architectural mesh does.

## Observation 1 -- bunny.obj with preservation can look *slightly worse*

Decimating the Stanford bunny to around 30-50% of original triangles with all
three preserve flags on produces a model that is, subjectively, slightly worse
than plain QEM at the same target.

### Why

The bunny is a single closed organic surface:

- No real **hard creases**. The dihedral-angle detector picks up soft curvature
  along the back and legs as "sharp" at a 60° threshold, then spends budget
  protecting edges that did not actually need protecting.
- Near-zero **open boundaries**. The bunny has a couple of small holes around
  the base but they are not visually meaningful.
- **Uniform curvature** across most of the surface. The curvature-factor
  amplification punishes the whole head/body roughly equally, removing one of
  plain QEM's advantages: its ability to thin out flat-ish regions aggressively.

Net effect: preservation pins triangles where plain QEM would have happily
collapsed them, so you end up with triangles distributed across the surface in
a less-optimal way for a fixed triangle budget.

### Recommendation for single-object organic meshes

- **Leave `preserve_boundary` on.** Cheap, never hurts if the mesh has no
  boundaries, and saves you if there are any.
- **Turn `preserve_sharp_edges` off or push the angle to 90°+.** The bunny has
  no edges sharper than about 80°.
- **Turn `preserve_curvature` off** or set `curvature_strength` to a low value
  (0.5-1.0). The default 2.0 is tuned for mixed meshes and is too aggressive
  for a uniform-curvature model.

## Observation 2 -- sponza with preservation is dramatically better

Decimating the Sponza atrium model to 70% with plain QEM essentially *removes
the building*. Walls lose their flatness, pillars lose their edges, ceilings
collapse into the floor geometry. The scene reads as noise.

Decimating the same model to 70% with all three preserve flags on keeps the
scene recognizable. Pillars stay vertical, walls stay planar, ceiling edges
stay crisp.

### Why

Sponza is the opposite of the bunny:

- **Real, sharp dihedral edges everywhere**: pillar corners, wall-to-ceiling
  seams, door frames. The sharp-edge preservation is protecting edges that
  carry almost all of the scene's perceived structure. Losing these costs more
  visual quality than losing the same number of triangles elsewhere.
- **Mixed boundaries**: windows and arches are open edges. `preserve_boundary`
  keeps the architectural openings from closing up.
- **Strong curvature contrast**: flat wall sections vs. ornate capitals and
  carvings. This is exactly the case `preserve_curvature` is designed for -- it
  decimates flat walls aggressively and spends the saved triangle budget on
  the decorated sections where curvature spikes.

Net effect: without preservation, QEM's cost metric treats all surface regions
as roughly equivalent, so it happily collapses architectural corners because
the geometric error is locally small. With preservation, those corners become
expensive to collapse, forcing the algorithm to pull triangles from flat wall
panels instead.

### Recommendation for scene / architectural meshes

- **All three preservation flags on.**
- Default weights are appropriate; raise them (e.g. `boundary_weight = 5000`,
  `sharp_edge_weight = 5000`) if you're decimating very aggressively (below
  30%) and want to be extra protective.
- Sharp-edge angle of 40-60° works well for CAD-style scenes.

## Demo script (three beats, cow -> bunny -> sponza)

A three-beat progression that each teaches a distinct lesson. Run them in
order; each sets up the one after.

### Beat 1: cow.obj at 10% -- "preservation is visibly helping"

Cow is a low-poly organic mesh (~5.8k triangles). Decimating hard to 10%
is aggressive, which is where preservation flags have the most to say.

1. Load **cow.obj** in the GUI
2. Target = 10%, all preservation flags **off**, Run
3. Flip View to Simplified
4. Note: horns blob out, eye sockets collapse, hooves lose their edges
5. Re-run with all preservation flags **on**
6. Flip View to Simplified
7. Note: horns stay crisp, eye sockets survive, hooves keep their edges

**Talking point**: preservation noticeably recovers detail on a moderate-size
organic mesh when decimating aggressively.

### Beat 2: bunny.obj at 10% -- "preservation isn't picking up the curvature"

Bunny is a higher-resolution organic mesh (~70k triangles). Same aggressive
10% target. Here preservation should help even more, but it doesn't.

1. Load **bunny.obj**
2. Open Feature Detection. Yellow curvature markers cluster on ear tips and
   folds, but large curvy regions (body, face, flanks) have almost no markers
   despite visibly curving.
3. Target = 10%, all preservation flags on, Run
4. Compare Simplified with and without `preserve_curvature`: almost no
   visible difference. The other two flags do their job (ear tips stay sharp,
   no visible boundary drift), but curvature isn't distinguishing curvy body
   regions from flat-ish ones.

**Talking point**: on a dense organic mesh, curvature awareness under-detects.
The flag is doing something, but not enough to see. This is not a bug -- it's
a root-cause limitation documented below.

### Beat 3: sponza (or another architectural scene) at 70% -- "preservation is the difference between a scene and a cloud of triangles"

Same flags, completely different mesh type. Modest 70% target.

1. Load the scene mesh
2. Target = 70%, all preservation flags **off**, Run
3. Result: walls lose flatness, pillars lose their corners, scene reads as
   noise
4. Re-run with all preservation flags **on**
5. Result: walls stay planar, pillar corners stay crisp, scene reads as a
   building

**Talking point**: preservation is a correctness feature for structured
meshes. Without it, architectural geometry disintegrates. The same flags
that were "neutral" on bunny are "critical" here.

### The arc

Three beats, three lessons:

1. Cow: preservation works and you can see it.
2. Bunny: preservation partially works, and the limitation is measurable and
   explainable.
3. Sponza: preservation is the difference between "looks like a scene" and
   "doesn't look like anything."

## Known Limitations & Root Cause (from testing)

The bunny-at-10% observation in Beat 2 is not random. It points at a specific
structural limitation in how curvature is measured.

### Observation

On cow (~5.8k tris) the curvature metric produces a clear signal: the yellow
markers cluster on horns, eyes, and hooves, and `preserve_curvature` visibly
protects those regions during aggressive decimation.

On bunny (~70k tris) the same metric produces a weak signal: large obviously-
curved regions (cheeks, body, back) pick up very few markers. Turning
`preserve_curvature` on and off produces barely-visible differences at the
same target percent.

These meshes are both "organic and curvy." The difference is mesh *density*.

### Root cause: the curvature metric uses a fixed 1-ring neighborhood

`ComputeVertexCurvatureFactor` gathers the normals of every triangle touching
the vertex, finds the pair with the smallest dot product, and uses that as
the vertex's "bend amount":

```
bend = clamp01((1 - min_cos_between_any_two_incident_face_normals) / 2)
factor = 1 + strength * bend
```

The neighborhood is always the 1-ring -- every triangle that shares a
vertex with this one.

On a coarse mesh (cow), the 1-ring covers a visually meaningful patch of the
surface. Neighboring triangles really do sample different parts of the bulge
or crease, so their normals genuinely disagree when the surface is curving.

On a dense mesh (bunny), the 1-ring covers a tiny patch. Two adjacent
triangles that share a vertex are nearly coplanar *because the surface is
sampled so finely*. The normals agree (min_cos is close to 1.0) regardless
of whether the underlying surface is curving. The metric reports "flat" for
a region that is obviously curved to the eye.

**The metric is scale-variant.** Doubling the tessellation halves the bend
signal even though nothing about the underlying surface changed. That's why
cow works and bunny doesn't.

### Second failure mode (not demonstrated but worth knowing)

On a scanned or noisy mesh, adjacent triangles disagree slightly *everywhere*
-- not because the surface is curved, but because the mesh has scan noise or
tessellation irregularities. Every vertex looks "curvy" and `preserve_curvature`
degenerates into "amplify every quadric equally," which is a no-op.

The same root cause: the 1-ring measurement is too local to distinguish real
curvature from mesh noise.

### Proposed fix: scale-adaptive geodesic-radius sampling

Replace the fixed 1-ring with a neighborhood whose size scales with the
mesh's own tessellation density. Sketch:

1. At seed time, compute the mesh's median edge length
2. For each vertex, run a short BFS outward collecting face normals until the
   neighborhood spans ~`k * median_edge_length` of geodesic distance (k = 3-5
   is a reasonable starting point)
3. Use those normals for the curvature measurement

Properties:

- **Scale-aware**: on dense meshes the neighborhood grows to cover a
  meaningful patch; on coarse meshes it stays close to the 1-ring.
- **Noise-robust**: averaging over more triangles smooths out
  sub-neighborhood jitter.
- **Still cheap**: bounded neighborhood size keeps per-vertex work O(k^2)
  with a small constant.

### Why this wasn't fixed in the current deliverable

1. The fix would invalidate the empirical observations already documented
   here. Those observations (cow works, bunny doesn't, sponza transforms)
   are themselves a useful result and a concrete evaluation criterion for
   a v2 metric.
2. The current behavior is a correct implementation of the baseline metric
   from the literature. The limitation is in the metric choice, not in the
   implementation of it.
3. Changing the metric this late would require re-running every demo and
   re-tuning every mesh-specific recommendation in this document.

### What to say about this in the presentation

Frame the limitation as an engineering *finding*, not a missing feature:

- "I tested on meshes of different density and found curvature awareness
  works clearly on coarse meshes and degrades on dense ones."
- "The root cause is that the curvature metric samples a fixed 1-ring
  neighborhood, which doesn't scale with mesh tessellation density."
- "The fix is a scale-adaptive neighborhood sized to the mesh's median edge
  length -- that's the v2 direction."

That's a complete engineering-process slide: observation, root cause,
proposed fix. More valuable to an evaluator than a passing result that
nobody stress-tested.

## Soft penalty vs. hard rule: the strictness split

### The bug that prompted this section

With `preserve_boundary = true`, decimating a mesh with holes still closed
the holes. The flag clearly did *something* -- boundary edges were getting a
heavy penalty -- but the holes were closing anyway.

### Root cause

The penalty is soft. `preserve_boundary` adds a penalty plane quadric
(weight ~1000) to boundary-vertex quadrics. That biases the priority queue so
boundary-adjacent edges are *costly* to collapse -- but after enough cheap
interior collapses run first, the remaining candidates include the boundary
edges themselves, and eventually the boundary edge is the lowest-cost option
left. The queue pops it, `ValidateCollapse` accepts it (manifold, no flip,
valid triangles), and the boundary disappears.

Same mechanism for `preserve_sharp_edges`: creases round off once the penalty
on a crease vertex is outbid by the accumulated cheap-collapse budget
elsewhere. A penalty can bias, but it cannot forbid.

### The fix: two-layer design

Each feature now has two knobs:

| Flag | What it does |
|---|---|
| `preserve_boundary` / `preserve_sharp_edges` | Soft quadric penalty. Grades the neighborhood so interior collapses are preferred first. |
| `boundary_strict` / `sharp_edge_strict` (default `true`) | Hard validator rules on top of the soft penalty. Make preservation a guarantee, not a bias. |

The hard rules, enforced in `ValidateCollapse`:

1. **Forbid collapsing a feature edge itself.** A boundary edge (incidence 1)
   or a crease edge (two adjacent faces exceed the sharp-angle threshold)
   cannot be the edge being collapsed -- collapsing it would remove the
   feature in one step.
2. **Forbid merging two feature vertices across a non-feature edge.** If both
   endpoints of an interior edge are on the boundary, collapsing stitches two
   boundary arcs together and closes the hole. If both are on creases, it
   braids two creases into one. Both are forbidden. The penalty can't prevent
   this because the edge being collapsed is not itself a feature edge.
3. **Pin the merged vertex to the feature vertex.** When exactly one endpoint
   is on a feature, the collapse is allowed but `new_pos` is forced to that
   endpoint's position (and `keep`/`remove` are swapped if needed so the
   feature vertex is the one kept). This prevents the feature from drifting
   inward across many collapses.

Curvature stays penalty-only. It's inherently a soft scaling factor -- there
is no "curvature edge" to forbid collapsing. The 1-ring scale issue documented
above is a separate concern.

### Why the strictness flag, not hard-coded

Most callers want guaranteed preservation. That's the default (`_strict = true`).

Some callers *want* small holes to close -- e.g. cleaning up a scanned mesh
with hairline cracks, or decimating a mesh where penalty-style grading is the
desired tradeoff. Turning `boundary_strict` off falls back to penalty-only,
which is the pre-fix behavior.

One bool per feature, not a multi-valued mode enum. If future cases require
more granularity (e.g. "allow collapsing boundary edges but not stitching
holes") we can split further, but the current two-state split covers the
observed use cases and keeps the UI to one checkbox per knob.

### Cost to output quality

Strict rules reject more candidates. On heavily-boundaried meshes the
simplifier may hit the target triangle count less tightly (overshoot by a few
percent) because many low-cost collapses are now forbidden. This is the
correct tradeoff: preservation is a correctness feature, and the penalty
system's purpose was always to prefer interior collapses -- the strict layer
just makes "prefer" into "require" at the boundary itself.

## Open question (separate from the above)

Is there a fast heuristic the tool could use to auto-detect which regime the
current mesh is in and suggest defaults? Candidates:

- Ratio of **sharp-edge count to total edge count**: high ratio suggests
  architectural/CAD; low ratio suggests organic.
- Ratio of **curvature-factor standard deviation to mean**: architectural
  meshes have a bimodal distribution (flat walls + ornate details); organic
  meshes are closer to uniform.
- Presence of **multiple large boundary loops**: architectural meshes often
  have many (windows, door frames); organic meshes usually have zero or one.

Implementing this as a "Suggest Defaults" button is out of scope for the
current deliverable but noted as a real usability improvement.
