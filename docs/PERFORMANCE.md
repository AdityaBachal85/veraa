# Large-scene performance

Goal: import scenes of 10–20 million triangles or thousands of objects in seconds, and keep
viewport navigation, selection, moving and undo interactive.

## How it is measured

`Veraa --script bench` imports the file named by `VERAA_BENCH_FILE`, then times the first and
second redraw of all four viewports, an orbit frame, selecting, click-picking, a 10-step move
with its undo step, undo/redo, a Scene Explorer rebuild, and saving and reopening as `.veraa`.
Every step includes the UI work it triggers (queued panel updates and repaints).

```sh
python3 tools/perf/gen_scenes.py obj obj5m.obj   --objects 1    --tris 5000000
python3 tools/perf/gen_scenes.py obj obj5000.obj --objects 5000 --tris 5000000
python3 tools/perf/gen_scenes.py fbx fbx5000.fbx --objects 5000 --tris 5000000
python3 tools/perf/gen_scenes.py glb glb20m.glb  --objects 200  --tris 20000000
xvfb-run -a -s "-screen 0 1920x1080x24" env LIBGL_ALWAYS_SOFTWARE=1 VERAA_BENCH_NODRAW=1 \
  VERAA_BENCH_FILE=glb20m.glb ./Veraa --script bench
```

The numbers below come from a 4-core cloud VM with software OpenGL (llvmpipe), Release build.
`VERAA_BENCH_NODRAW=1` skips the mesh draw calls so the viewport numbers show the application's
own CPU work per frame rather than software rasterization; uploads and GL state changes still
happen, and llvmpipe makes those much slower than a real driver. The parallel parts (parsing,
mesh building, triangulation, file encoding/decoding) use every core, so a 16-core/32-thread
desktop CPU is several times faster than this VM. `VERAA_BENCH_ORBIT_FRAMES=n` repeats the
orbit for profiling (`perf record -g` under `xvfb-run`).

"Before" is the state at the start of this work (shared undo snapshots and copy-on-write meshes
were already in); times in milliseconds.

| Step | obj5m (1 object, 5.0M tris) | obj5000 (5000 objects, 4.8M) | fbx5000 (5000 objects + 5000 materials, 4.8M) | glb20m (200 objects, 19.9M) |
|---|---|---|---|---|
| Import, until the UI is idle | 15 444 → **2 621** | 30 852 → **1 643** | 36 225 → **3 256** | 49 882 → **8 327** |
| + first redraw (uploads) | 33 → 36 | 139 → 530 | 1 334 → 599 | 65 → 4 986 ¹ |
| Redraw, 4 views (CPU) | 21 → 28 | 108 → 116 | 1 060 → 145 | 44 → 40 |
| Orbit frame, perspective (CPU) | 7 → 12 | 62 → 59 ² | 325 → 64 ² | 20 → 18 |
| Select one object | 48 → 82 | 206 → 212 | 1 299 → 210 | 254 → 171 |
| Click pick | 56 → **0.01** (first: 621 ³) | 2 → 4 | 4 → 4 | 0 → 0.1 |
| Move, 10 drag steps | 79 → 169 | 404 → 425 | 2 902 → 444 | 224 → 152 |
| Move commit (undo step) | 87 → 76 | 258 → 227 | 1 541 → 252 | 182 → 136 |
| Undo move | 86 → 70 | 301 → 227 | 1 575 → 283 | 227 → 108 |
| Scene Explorer rebuild | 120 → 132 | 3 276 → **261** | 4 500 → **288** | 427 → 133 |
| Save .veraa | 3 139 → **1 336** | 6 587 → **336** | 9 300 → **623** | 38 388 → **5 381** ⁴ |
| File size (MB) | 169 → 126 | 172 → 130 | 181 → 138 | 888 → 666 |
| Open .veraa | 8 085 → **1 675** | 13 978 → **1 432** | 20 499 → **2 115** | 39 788 → **6 480** ¹ |

Time from choosing the file to a drawn viewport (import + first redraw): 15.5 s → 2.7 s,
31.0 s → 2.2 s, 37.6 s → 3.9 s and 49.9 s → 13.3 s. During an import or open the window keeps
repainting, and after half a second a progress dialog with Cancel appears.

¹ Mostly uploading 800 MB of vertex buffers into llvmpipe; a hardware driver does this in a
fraction of a second. ² With adaptive degradation (on by default) the slow software frames
switch these 5000 objects to boxes while navigating; a GPU stays at full detail. ³ The first
pick of a mesh builds its BVH (radix-sorted LBVH), later picks take microseconds. ⁴ Bound by
this VM's disk (about 200 MB/s). Steps under about 100 ms vary from run to run by tens of
milliseconds on this shared VM (an orbit of 300 frames on obj5m averages 4 ms of CPU per frame).

## What was slow and what changed

- **Import runs on a worker thread** (`src/app/importjob.*`): the file is read into a scratch
  scene, display meshes are triangulated on all cores (`Scene::prepareMeshes`), then everything
  is moved into the document in one step (`Scene::adopt`: fresh ids, unique names, remapped
  parents/materials/layers, evaluated meshes kept). Importers report progress and stop when
  cancelled (`src/core/progress.h`; OBJ, glTF and FBX, the latter through ufbx's callback).
  Opening a `.veraa` reads and decodes the file on a worker thread the same way.
- **OBJ** (`obj_import.cpp`): the file is read at once and split into chunks of lines parsed in
  parallel with `std::from_chars` (no streams or per-token strings); groups then get their own
  vertex numbering through a dense table instead of hash maps, in parallel. Name collisions
  were an O(n²) scan.
- **glTF/FBX**: every mesh is converted once, in parallel, with bulk accessor reads; meshes used
  by several nodes are shared (copy-on-write) instead of copied.
- **Triangulation** (`mesh.cpp`) was single-threaded with a hash map per corner and a vector per
  vertex. It now uses vertex→corner adjacency arrays, per-vertex corner deduplication and writes
  triangles and edges in place, in parallel for big meshes.
- **`.veraa` files** store meshes as a binary section after the JSON (`VERAABIN`, see
  `Scene::save`) instead of base64 inside it, encoded and decoded in parallel. Old JSON files
  still open.
- **Viewport** (`viewport.cpp`, `glscene.*`): GPU buffers are cached per node by geometry
  stamp, so moving, selecting or undoing never re-uploads or re-triangulates; uniforms are sent
  as raw matrices; material colors are cached until materials change; objects outside the view
  are culled by their world bounds, and while navigating slowly, objects small on screen draw as
  boxes (the threshold adapts to the frame time; sub-pixel ones are skipped). Buffer and vertex
  array names are recycled: Mesa searches for free names linearly once names were deleted,
  which made reopening a 5000-object scene take 5 s of draw time.
- **Picking** builds a BVH per mesh on first use (`intersectTriMesh`).
- **Scene Explorer** is a model/view (`SceneTreeModel`) instead of a `QTreeWidget` rebuilt with
  painted icons and auto-sized columns on every change: rows are read on demand, name and
  display changes repaint rows, selection is synced as ranges. The Layer Explorer skips rebuilds
  while hidden. The Material Editor renders swatches only for slots in view (it used to queue a
  path-traced preview for every material of an imported scene).
- **Scene**: material lookup by id was a linear scan (called per object per frame); unique
  material names and duplicate-id repair were quadratic; undo snapshots now serialize nodes
  and compare materials in parallel.
