# QEM Studio

A standalone desktop app for mesh simplification via Quadric Error Metrics
with optional preservation of boundaries, sharp edges, and curved regions.
Renders its own 3D viewport (GLFW + OpenGL + ImGui) and ships with all
third-party dependencies vendored under `third_party/`.

## Launch (dev workflow)

From the repo root, build and run in one step:

```bat
build-and-run.bat release run
```

This configures CMake if needed, builds the `glint_qem_studio_gui` target in
Release, and launches the window. Submissions assembled by
`scripts/build_submission.py` get a standalone double-click launcher
(`launch-gui.bat`) generated into the submission folder itself.

## Build manually

```
cmake -S . -B builds/cmake
cmake --build builds/cmake --config Release --target glint_qem_studio_gui
```

The resulting binary is `builds/cmake/Release/glint_qem_studio_gui.exe`.

## Dev loop

The `build-and-run.bat` helper wraps CMake for fast iteration:

```bat
build-and-run.bat release run       REM build + launch Release
build-and-run.bat debug build       REM just build Debug
build-and-run.bat clean             REM wipe the build dir
```

## Quick tour of the GUI

When you open a mesh with `Open OBJ...`, the app auto-frames it and kicks off
a feature analysis.

- **Mesh**: file stats, display mode selector (Solid / Wireframe / Both).
- **Feature Detection**: sliders for the sharp-edge angle threshold and the
  top-N% curvature markers. Counts update live. Overlays render directly in
  the viewport: boundaries in red, crease edges in orange, curvature
  hotspots as yellow points.
- **Simplification**: target percent, preservation toggles (boundary / sharp
  / curvature), and an `Advanced tuning` tree for weights. The "Run
  Simplification" button runs the simplifier on a background thread so the
  viewport stays responsive.
- **Results**: triangle/vertex counts before and after, collapse statistics,
  elapsed time, and an OBJ export button.
- **Compare**: enables a split viewport so you can see two meshes side by
  side sharing a single orbit camera. Auto-populates the right pane with
  your latest simplification result for one-click A/B.

Mouse controls in the viewport: **left-drag** orbits, **right-drag** pans,
**scroll** zooms.

## Three-beat demo flow

1. **cow.obj at 10% target** -- preservation clearly helps. Run with all
   flags off, then all flags on; horns and hooves are visibly crisper with
   preservation on.
2. **bunny.obj at 10% target** -- preservation partially works. Useful to
   show a known limitation of the curvature metric on dense meshes
   (see `docs/PRESERVATION_OBSERVATIONS.md`).
3. **sponza at 70% target** -- preservation is the difference between
   a recognizable scene and a pile of triangles.

## Producing a submission folder

`scripts/build_submission.py` packages a self-contained folder suitable for
handing to an evaluator. It copies the GUI source + the vendored third-party
dependencies + the sample models into `dist/glint_qem_studio/`, emits a
stripped-down CMakeLists, and verifies the result actually builds.

```
python scripts/build_submission.py
python scripts/build_submission.py --no-verify
python scripts/build_submission.py --out some/other/path
```

The generated folder is fully self-contained; all dependencies it needs are
under its own `ThirdParty/`.

## Documentation

- `docs/PRESERVATION_OBSERVATIONS.md` -- when each preservation flag helps
  vs. hurts, three-beat demo script, known limitations, proposed fixes.
- `docs/PERFORMANCE_ANALYSIS.md` -- profiling baseline, parallelization
  feasibility writeup, failed `std::map -> std::unordered_map` optimization
  attempt (measured, reverted, documented).
- `docs/BACKEND_INTERFACE_DISCUSSION.md` -- design rationale from the
  earlier backend-interfaces iteration. Historical; the GUI no longer uses
  that layering.
- `Project Mid-Term Report.pdf` -- the mid-term submission.

## Decomposition preview (legacy)

Earlier iterations had a REPL shell that could render per-collapse frames and
stitch them with ffmpeg. That workflow was removed; the equivalent
capability is exposed in the GUI as the `Run Simplification` flow plus the
`Compare` view.
