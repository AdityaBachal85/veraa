# Veraa architecture

Veraa is split into a UI-free core, a renderer, a command-line renderer and the Qt desktop
app. Feature modules plug into registries and extension points instead of editing shared
files, so several features can be developed in parallel.

## Libraries

| Folder | Library | Depends on |
|---|---|---|
| `src/core` | `veraa_core`: scene, objects, modifiers, materials, file IO | nlohmann/json, stb |
| `src/render` | `veraa_render`: Embree path tracer, denoiser, image output | core, Embree, OIDN (optional) |
| `src/cli` | `veraa-render` command-line renderer | render |
| `src/app` | `Veraa` desktop app (every `.cpp` in the folder is compiled) | core, render, Qt 6 |

World space is Z-up, like 3ds Max. Units are centimeters by default.

## Core concepts

- **Node**: an object placed in the scene. It has a local `transform` (relative to its parent),
  an `objectOffset` (pivot adjustment applied to the geometry only), a `parentId`, a layer,
  display/render flags, group flags, a material id, a shared `object` and a stack of shared
  `modifiers`. Sharing the object and modifiers between nodes makes them instances.
  - `worldMatrix()` is the pivot in world space; `objectWorldMatrix()` places geometry.
  - After changing object parameters or the stack, call `node->invalidate()`.
- **SceneObject** kinds: Geometry (`build()` → `PolyMesh`), Shape (`buildShape()` →
  `BezierShape`), Light, Camera, Helper, SpaceWarp.
- **Modifiers** receive a `GeomState` (mesh or shape). Mesh modifiers override
  `apply(PolyMesh&)`; shape-aware modifiers (Extrude, Lathe, Sweep) override `applyState()`.
- **ParamBlock**: typed, named parameters with UI hints. Forms, file IO and (later) animation
  work on them generically.
- **Scene**: nodes, layers, materials, render settings, time settings and an `extensions` JSON
  object where feature modules save their own data under their module name.

## Registries (core)

- Object types: `registerObjectType({type, displayName, category, subcategory, create})`.
  Categories are the Create panel tabs: Geometry, Shapes, Lights, Cameras, Helpers,
  Space Warps, Systems. The subcategory is the dropdown entry (Standard Primitives,
  Extended Primitives, AEC Objects, Splines, ...).
- Modifier types: `registerModifierType({type, displayName, category, acceptsMeshes,
  acceptsShapes, create})`. Categories are the Modifiers menu groups (Mesh Editing,
  Parametric Deformers, Free Form Deformers, UV Coordinates, ...).
- Each module has one `registerXxx()` function, declared in `src/core/registry.h` and called
  from `src/core/registry.cpp`.

The Create panel, the Create menu, the Modifiers menu and file loading all read these
registries, so a new type appears everywhere once registered.

## App extension points (`src/app/extension.h`)

- `VERAA_APP_EXTENSION(name, order, installer)`: the installer runs once after the main window
  is built. It can use:
  - `MainWindow::menuAt("Tools/Align")`: find or create menus (top level stays in 3ds Max order),
  - `mainToolBar()`, `addToolBar()`, `addDockWidget()`,
  - `addBelowViewports(widget)`: time slider, track bar,
  - `addCommandPanelTab(page, icon, title)`: Hierarchy, Motion, Display, Utilities,
  - `registerCommand(id, action)`: for the hotkey editor.
- `ViewportTool`: an interactive mode set with `EditorState::setViewportTool()`. It gets mouse
  and key events first and can draw 3D lines and a 2D overlay.
- `ViewportDecorator`: always-on viewport drawing (`EditorState::addDecorator()`).
- `registerCreationTool(type, factory)`: custom interactive creation for an object type.
- `registerModifyRollouts(type, factory)`: custom Modify panel rollouts for an object or
  modifier type (selection levels, editing buttons), shown above the generated parameters.
- Snapping: `EditorState::setPointSnapper()`, `setAngleSnap()`, `setPercentSnap()`; creation and
  transform drags already call them.
- Selection filter: `EditorState::setSelectionFilter()`.

## Undo

Every scene edit goes through `SceneDocument::beginEdit()/endEdit()` (or `softEdit()` for
spinner drags). Undo restores a scene JSON snapshot, so new data saved by a module (in
`Scene::extensions` or in object/modifier JSON) is undoable automatically.

## Tests

- `tests/core_tests.cpp`, `tests/render_tests.cpp` (CTest).
- `Veraa --script selftest` drives the real UI under Xvfb. Feature modules add their own UI
  tests in their own files with `VERAA_SELF_TEST(name, fn)` (helpers in `selftest.h`);
  `Veraa --script <name>` runs one of them.
- `Veraa --script demo --screenshot out.png` saves screenshots of the main windows.
