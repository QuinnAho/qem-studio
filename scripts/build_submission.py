"""Assemble the QEM Studio submission folder.

Usage:
    python scripts/build_submission.py
    python scripts/build_submission.py --no-verify
    python scripts/build_submission.py --out path/to/out

The script is idempotent: running it twice wipes and rebuilds the output
directory. The generated folder is meant to be handed to an evaluator as-is
or zipped for transport.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# Paths (resolved relative to this script, not the current working directory,
# so the script works no matter where it's invoked from).
# ---------------------------------------------------------------------------

SCRIPT_PATH   = Path(__file__).resolve()
REPO_ROOT     = SCRIPT_PATH.parent.parent             # repo root (qem-studio/)
APP_DIR       = REPO_ROOT                             # alias for legacy paths below

DEFAULT_OUT   = REPO_ROOT / "dist" / "glint_qem_studio"


# ---------------------------------------------------------------------------
# File lists. Explicit rather than glob-based because it's easier to audit
# what goes into the submission than to reason about what might accidentally
# get swept in by a pattern.
# ---------------------------------------------------------------------------

# Source files that end up under src/ in the submission.
SOURCE_FILES = [
    ("src/core/simplify_baseline.cpp",                  "src/core/simplify_baseline.cpp"),
    ("src/backends/mesh_preview_staging.cpp",           "src/backends/mesh_preview_staging.cpp"),
    ("src/backends/obj_mesh_io_tinyobj.cpp",            "src/backends/obj_mesh_io_tinyobj.cpp"),
    ("src/gui/main.cpp",                                "src/gui/main.cpp"),
]

# Public headers that end up under include/.
HEADER_FILES = [
    ("include/glint_qem/simplify.h",                    "include/glint_qem/simplify.h"),
    ("include/glint_qem/types.h",                       "include/glint_qem/types.h"),
    ("include/glint_qem_tool/mesh_preview_staging.h",   "include/glint_qem_tool/mesh_preview_staging.h"),
    ("include/glint_qem_tool/obj_mesh_io.h",            "include/glint_qem_tool/obj_mesh_io.h"),
]

# Model files. cornellbox is excluded per project scoping decision.
MODEL_FILES = [
    ("resources/models/cow.obj",     "resources/models/cow.obj"),
    ("resources/models/bunny.obj",   "resources/models/bunny.obj"),
    ("resources/models/dragon.obj",  "resources/models/dragon.obj"),
]

# Shader sources loaded by the GUI at runtime. LoadShaderSource() in the GUI
# looks for these relative to the exe so they have to ship alongside it.
SHADER_FILES = [
    ("resources/shaders/mesh.vert",    "resources/shaders/mesh.vert"),
    ("resources/shaders/mesh.frag",    "resources/shaders/mesh.frag"),
    ("resources/shaders/overlay.vert", "resources/shaders/overlay.vert"),
    ("resources/shaders/overlay.frag", "resources/shaders/overlay.frag"),
]

# Whole-directory model copies (e.g. sponza with its .mtl + textures).
MODEL_DIRS = [
    ("resources/models/sponza",      "resources/models/sponza"),
]

# Documentation shipped with the submission.
DOC_FILES = [
    ("docs/BACKEND_INTERFACE_DISCUSSION.md",      "docs/BACKEND_INTERFACE_DISCUSSION.md"),
    ("docs/PERFORMANCE_ANALYSIS.md",              "docs/PERFORMANCE_ANALYSIS.md"),
    ("docs/PRESERVATION_OBSERVATIONS.md",         "docs/PRESERVATION_OBSERVATIONS.md"),
    ("presentation/final_report.txt",             "docs/final_report.txt"),
    ("presentation/script.md",                    "docs/presentation_script.md"),
    ("presentation/slides.md",                    "docs/slides.md"),
    ("QEM_Studio.pptx",                           "docs/QEM_Studio.pptx"),
]


# Third-party dependencies. Sourced from this repo's flattened third_party/
# tree.
#
# Each tuple is (src_path_relative_to_repo_root, dst_path_relative_to_out_dir).
THIRD_PARTY_FILES = [
    # glad (loader impl + header)
    ("third_party/glad/glad.c",                            "ThirdParty/glad/glad.c"),
    ("third_party/glad/glad.h",                            "ThirdParty/glad/include/glad/glad.h"),
    ("third_party/KHR/khrplatform.h",                      "ThirdParty/glad/include/KHR/khrplatform.h"),

    # tinyobjloader (single-header)
    ("third_party/tinyobjloader/tiny_obj_loader.h",
        "ThirdParty/tinyobjloader/tiny_obj_loader.h"),
]

# Entire directories copied wholesale.
THIRD_PARTY_DIRS = [
    ("third_party/imgui",            "ThirdParty/imgui"),
    ("third_party/glm",              "ThirdParty/glm"),
    ("third_party/glfw",             "ThirdParty/glfw"),
]


# ---------------------------------------------------------------------------
# Generated files (CMakeLists.txt, launcher bat, README).
# ---------------------------------------------------------------------------

CMAKELISTS_TEMPLATE = r"""# glint_qem_studio -- standalone submission build
#
# This CMakeLists is self-contained: it does not reference any Glint3D engine
# code. All third-party dependencies live under ThirdParty/ in this folder.
#
# Build:
#   cmake -S . -B build
#   cmake --build build --config Release
#
# Run (Windows):
#   build\Release\glint_qem_studio_gui.exe
#   or double-click launch-gui.bat

cmake_minimum_required(VERSION 3.15)
project(glint_qem_studio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Paths ------------------------------------------------------------------

set(QEM_ROOT         ${CMAKE_CURRENT_SOURCE_DIR})
set(QEM_INCLUDE_DIR  ${QEM_ROOT}/include)
set(QEM_THIRD_PARTY  ${QEM_ROOT}/ThirdParty)

set(QEM_IMGUI_DIR    ${QEM_THIRD_PARTY}/imgui)
set(QEM_GLAD_DIR     ${QEM_THIRD_PARTY}/glad)
set(QEM_GLFW_DIR     ${QEM_THIRD_PARTY}/glfw)

# Pick the GLFW static lib. glfw3.lib matches the default MSVC /MD runtime.
if (EXISTS "${QEM_GLFW_DIR}/lib/glfw3.lib")
    set(QEM_GLFW_LIB "${QEM_GLFW_DIR}/lib/glfw3.lib")
elseif (EXISTS "${QEM_GLFW_DIR}/lib/glfw3_mt.lib")
    set(QEM_GLFW_LIB "${QEM_GLFW_DIR}/lib/glfw3_mt.lib")
else()
    message(FATAL_ERROR "No GLFW .lib found under ${QEM_GLFW_DIR}/lib.")
endif()

# ---- Core library (pure algorithm; no graphics deps) ------------------------

add_library(qem_core STATIC
    src/core/simplify_baseline.cpp
)
target_include_directories(qem_core PUBLIC ${QEM_INCLUDE_DIR})

# ---- Backend library (OBJ IO + preview staging helpers used by the GUI) ----

add_library(qem_backends STATIC
    src/backends/mesh_preview_staging.cpp
    src/backends/obj_mesh_io_tinyobj.cpp
)
target_include_directories(qem_backends PUBLIC
    ${QEM_INCLUDE_DIR}
    ${QEM_THIRD_PARTY}/tinyobjloader
)
target_link_libraries(qem_backends PUBLIC qem_core)

# ---- ImGui (vendored; compiled into a small static lib) --------------------

add_library(qem_imgui STATIC
    ${QEM_IMGUI_DIR}/imgui.cpp
    ${QEM_IMGUI_DIR}/imgui_draw.cpp
    ${QEM_IMGUI_DIR}/imgui_tables.cpp
    ${QEM_IMGUI_DIR}/imgui_widgets.cpp
    ${QEM_IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${QEM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(qem_imgui PUBLIC
    ${QEM_IMGUI_DIR}
    ${QEM_IMGUI_DIR}/backends
    ${QEM_GLFW_DIR}/include
)
if (MSVC)
    target_compile_options(qem_imgui PRIVATE /W3 /wd4996)
endif()

# ---- glad (C) ---------------------------------------------------------------

add_library(qem_glad STATIC
    ${QEM_GLAD_DIR}/glad.c
)
target_include_directories(qem_glad PUBLIC ${QEM_GLAD_DIR}/include)

# ---- GUI executable --------------------------------------------------------

add_executable(glint_qem_studio_gui
    src/gui/main.cpp
)
target_include_directories(glint_qem_studio_gui PRIVATE
    ${QEM_INCLUDE_DIR}
    ${QEM_GLAD_DIR}/include
    ${QEM_GLFW_DIR}/include
    # glm is a header-only lib whose headers are referenced as <glm/glm.hpp>.
    # The folder layout has glm.hpp sitting directly inside ThirdParty/glm/,
    # so the include root must be ThirdParty/ (the PARENT of the glm/
    # directory) for the angle-bracket path to resolve.
    ${QEM_THIRD_PARTY}
)
target_link_libraries(glint_qem_studio_gui PRIVATE
    qem_core
    qem_backends
    qem_imgui
    qem_glad
    ${QEM_GLFW_LIB}
)
if (WIN32)
    target_link_libraries(glint_qem_studio_gui PRIVATE
        opengl32 gdi32 user32 shell32
    )
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(glint_qem_studio_gui PRIVATE OpenGL::GL dl pthread)
endif()

if (MSVC)
    target_compile_options(qem_core      PRIVATE /W4)
    target_compile_options(qem_backends  PRIVATE /W4)
    target_compile_options(glint_qem_studio_gui PRIVATE /W3)
else()
    target_compile_options(qem_core      PRIVATE -Wall -Wextra)
    target_compile_options(qem_backends  PRIVATE -Wall -Wextra)
    target_compile_options(glint_qem_studio_gui PRIVATE -Wall -Wextra)
endif()
"""


LAUNCHER_BAT_TEMPLATE = r"""@echo off
REM Double-click this file to open QEM Studio. Builds on first run, then launches.
setlocal

REM %~dp0 always ends with a backslash. Strip it so that quoted paths
REM passed to cmake don't end in \" -- cmake treats \" as an escaped
REM quote and eats the closing quote, which silently mangles the -S arg.
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "BUILD_DIR=%HERE%\build"
set "EXE=%BUILD_DIR%\Release\glint_qem_studio_gui.exe"

if not exist "%EXE%" (
  echo First-run setup: building QEM Studio ^(Release^).
  echo This usually takes under a minute.
  echo.
  cmake -S "%HERE%" -B "%BUILD_DIR%"
  if errorlevel 1 goto build_failed
  cmake --build "%BUILD_DIR%" --config Release --target glint_qem_studio_gui
  if errorlevel 1 goto build_failed
)

if not exist "%EXE%" goto build_failed

start "" "%EXE%"
exit /b 0

:build_failed
echo.
echo Build failed. See messages above. Press any key to close this window.
pause >nul
exit /b 1
"""


README_TEMPLATE = """# QEM Studio

A standalone mesh-simplification tool implementing Quadric Error Metrics with
boundary, sharp-edge, and curvature preservation. Includes a GUI for live
feature visualization, simplification parameter tuning, and before/after
side-by-side comparison.

## Quick start (Windows)

1. Double-click `launch-gui.bat`. First run builds the Release target; it
   typically takes under a minute. Subsequent runs launch immediately.
2. Click **Open OBJ...** and pick a model from `resources/models/` (cow.obj
   is a good demo mesh).
3. Drag the **Sharp angle** and **Curvature top %** sliders to see the
   feature overlays update live in the viewport.
4. Set a **Target %** in Simplification, tick the preservation flags you
   want, and click **Run Simplification**. Use the **View** radio to flip
   between the original and the simplified mesh.
5. Open the **Compare** section and check **Enable split comparison** to see
   both meshes side by side with a shared orbit camera.

## Build manually

```
cmake -S . -B build
cmake --build build --config Release
```

The executable is `build/Release/glint_qem_studio_gui.exe`.

## Folder layout

- `src/`          - application source (core simplifier, backends, GUI)
- `include/`      - public headers
- `resources/`    - sample models
- `docs/`         - design notes, performance analysis, preservation
                    observations, mid-term report
- `ThirdParty/`   - vendored dependencies (ImGui, GLFW, GLAD, GLM,
                    tinyobjloader)

## Dependencies

All third-party dependencies are vendored under `ThirdParty/`. No external
package manager is required. You need a recent CMake (3.15+) and a C++17
compiler with C support (MSVC 2019+, GCC 9+, Clang 9+).

## Documentation

See `docs/` for:

- `final_report.txt` - the final project writeup: summary of what was
  built, what worked, what didn't, and why.
- `slides.md` / `presentation_script.md` / `QEM_Studio.pptx` -
  presentation materials.
- `PRESERVATION_OBSERVATIONS.md` - empirical notes on when each
  preservation flag helps vs. hurts, the demo script, known limitations,
  and the soft-penalty / hard-rule (strict) design.
- `PERFORMANCE_ANALYSIS.md` - profiling baseline, parallelization
  feasibility analysis, and the `std::map` -> `std::unordered_map`
  optimization attempt (measured, reverted, documented).
- `BACKEND_INTERFACE_DISCUSSION.md` - rationale for the backend-interfaces
  design (historical).
"""


# ---------------------------------------------------------------------------
# Copy helpers
# ---------------------------------------------------------------------------

def _reset_output(out_dir: Path) -> None:
    """Wipe and recreate the output directory so every run is clean."""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)


def _copy_files(entries, src_root: Path, dst_root: Path) -> None:
    """Copy a list of (src_rel, dst_rel) pairs. Parents created as needed."""
    for src_rel, dst_rel in entries:
        src = src_root / src_rel
        dst = dst_root / dst_rel
        if not src.exists():
            raise FileNotFoundError(f"missing source file: {src}")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def _copy_dirs(entries, src_root: Path, dst_root: Path) -> None:
    """Copy a list of (src_rel, dst_rel) directory pairs."""
    for src_rel, dst_rel in entries:
        src = src_root / src_rel
        dst = dst_root / dst_rel
        if not src.exists():
            raise FileNotFoundError(f"missing source directory: {src}")
        # dirs_exist_ok=True matters because we may want to merge into an
        # already-existing parent (e.g. ThirdParty/) populated by another entry.
        shutil.copytree(src, dst, dirs_exist_ok=True)


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def assemble(out_dir: Path) -> None:
    print(f"[submission] target: {out_dir}")

    _reset_output(out_dir)

    print("[submission] copying source files...")
    _copy_files(SOURCE_FILES, APP_DIR, out_dir)

    print("[submission] copying headers...")
    _copy_files(HEADER_FILES, APP_DIR, out_dir)

    print("[submission] copying models (cornellbox excluded)...")
    _copy_files(MODEL_FILES, APP_DIR, out_dir)
    _copy_dirs(MODEL_DIRS, APP_DIR, out_dir)

    print("[submission] copying shader files...")
    _copy_files(SHADER_FILES, APP_DIR, out_dir)

    print("[submission] copying docs...")
    _copy_files(DOC_FILES, APP_DIR, out_dir)

    print("[submission] copying third-party deps...")
    _copy_files(THIRD_PARTY_FILES, REPO_ROOT, out_dir)
    _copy_dirs(THIRD_PARTY_DIRS, REPO_ROOT, out_dir)

    print("[submission] writing generated files (CMakeLists, launcher, README)...")
    _write_text(out_dir / "CMakeLists.txt", CMAKELISTS_TEMPLATE)
    _write_text(out_dir / "launch-gui.bat", LAUNCHER_BAT_TEMPLATE)
    _write_text(out_dir / "README.md", README_TEMPLATE)

    print(f"[submission] assembly complete: {out_dir}")


# ---------------------------------------------------------------------------
# Submission-cleanliness checks
# ---------------------------------------------------------------------------
#
# Two audit scans that run between assemble() and verify_build():
#
#   1) Agent artifacts: fail loudly if anything like .claude/, CLAUDE.md,
#      claudemap-*.json, .cursor/, .aider* ends up in the output folder.
#      These never belong in a submission, and the scan is cheap insurance
#      against future source-list edits accidentally sweeping them in.
#
#   2) Leftover Machine Summary / Human Summary banner text in any shipped
#      text file. If a source file ever reacquires the banner it'd ride
#      the next packaging step into the submission; this makes that
#      obvious at packaging time instead of at presentation time.

# Substrings / filenames that mean an AI-agent artifact landed in the
# submission. Matched case-insensitively.
AGENT_ARTIFACT_BASENAMES = {
    ".claude",
    ".cursor",
    ".aider",
    "claude.md",
    "claudemap-cache.json",
    "claudemap-maps.json",
    ".aider.conf.yml",
    ".aider.chat.history.md",
}

# File extensions we scan for leftover banner text. Binary or large-mesh
# files are skipped because they can't contain the banner anyway and
# reading them wastes time.
TEXT_FILE_EXTENSIONS = {
    ".h", ".hpp", ".cpp", ".cc", ".c",
    ".md", ".txt", ".py", ".bat", ".cmd",
    ".cmake",
}

# Phrases that only appeared in the stripped machine banners.
BANNER_PHRASES = (
    "Machine Summary Block",
    "Human Summary",  # standalone phrase; rare in prose
)


def _path_has_agent_basename(path: Path) -> bool:
    name = path.name.lower()
    return name in AGENT_ARTIFACT_BASENAMES or name.startswith(".aider")


def scan_for_agent_artifacts(out_dir: Path) -> list[Path]:
    """Return any paths inside out_dir that look like AI-agent artifacts.

    Walks the whole output tree. Empty list means the submission is clean.
    """
    hits: list[Path] = []
    for path in out_dir.rglob("*"):
        if _path_has_agent_basename(path):
            hits.append(path)
    return hits


def scan_for_banner_text(out_dir: Path) -> list[Path]:
    """Return text files under out_dir that still contain banner phrases.

    Only scans files whose extension is in TEXT_FILE_EXTENSIONS so we don't
    try to read e.g. .obj mesh files or vendored binaries.
    """
    hits: list[Path] = []
    for path in out_dir.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in TEXT_FILE_EXTENSIONS:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if any(phrase in text for phrase in BANNER_PHRASES):
            hits.append(path)
    return hits


def run_submission_cleanliness_checks(out_dir: Path) -> None:
    """Run both audit scans; raise SystemExit on any hit."""
    print("[audit] scanning for agent artifacts and leftover banner text...")

    agent_hits  = scan_for_agent_artifacts(out_dir)
    banner_hits = scan_for_banner_text(out_dir)

    if not agent_hits and not banner_hits:
        print("[audit] submission is clean.")
        return

    if agent_hits:
        print("[audit] ERROR: AI-agent artifacts found in submission:", file=sys.stderr)
        for hit in agent_hits:
            print(f"  {hit}", file=sys.stderr)
    if banner_hits:
        print("[audit] ERROR: Machine/Human Summary banner text still present in:",
              file=sys.stderr)
        for hit in banner_hits:
            print(f"  {hit}", file=sys.stderr)
    raise SystemExit("[audit] submission rejected; fix the files above and re-run.")


def verify_build(out_dir: Path) -> None:
    """Run cmake configure + Release build inside the submission folder.

    The build artifacts are discarded afterwards so the shipped folder
    contains only source (evaluators rebuild on their own machine anyway,
    and build artifacts balloon the folder size).

    Raises SystemExit on failure so callers can surface the error clearly.
    """
    build_dir = out_dir / "build"
    print(f"[verify] configuring: cmake -S {out_dir} -B {build_dir}")
    subprocess.run(
        ["cmake", "-S", str(out_dir), "-B", str(build_dir)],
        check=True,
    )

    print(f"[verify] building Release: cmake --build {build_dir} --config Release")
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--config", "Release",
         "--target", "glint_qem_studio_gui"],
        check=True,
    )

    # Existence check: the executable should now be at a known path.
    exe = build_dir / "Release" / "glint_qem_studio_gui.exe"
    if not exe.exists():
        # On single-config generators (Ninja, Make) it'd be one level up instead.
        alt_exe = build_dir / "glint_qem_studio_gui.exe"
        exe = alt_exe if alt_exe.exists() else exe
    if not exe.exists():
        raise SystemExit(f"[verify] executable not produced at {exe}")
    print(f"[verify] submission built ok: {exe}")

    print(f"[verify] removing build artifacts so the submission ships clean")
    shutil.rmtree(build_dir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Assemble QEM Studio submission folder.")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help=f"output directory (default: {DEFAULT_OUT})")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the cmake configure + build verification step")
    args = parser.parse_args()

    out_dir = args.out.resolve()

    assemble(out_dir)

    # Audit the assembled folder before anything else touches it. This
    # catches both agent artifacts (shouldn't ship) and leftover banner
    # text (looks like dev scaffolding). Runs even under --no-verify
    # because it's purely a content check, not a build step.
    run_submission_cleanliness_checks(out_dir)

    if args.no_verify:
        print("[submission] skipping build verification (--no-verify).")
    else:
        try:
            verify_build(out_dir)
        except subprocess.CalledProcessError as e:
            print(f"[verify] build step failed: {e}", file=sys.stderr)
            return 1

    print()
    print(f"Submission ready at: {out_dir}")
    print("Zip the folder or hand it over as-is.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
