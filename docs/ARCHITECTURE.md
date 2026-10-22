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

## Interface (`src/app`)

- **Main window layout**: menu bar with the Workspaces selector in its right corner, the main
  toolbar (`mainwindow_toolbars.cpp`), the vertical Axis Constraints toolbar, Scene and Layer
  Explorers docked left, the command panel docked right, and the two-row status bar
  (`mainwindow_statusbar.cpp`): Python mini listener, selection count and prompt, isolate /
  selection lock / absolute-offset toggles, X/Y/Z type-in, grid size, adaptive degradation, the
  animation slot and the navigation controls.
- `MainWindow::addStatusBarWidget(widget)` puts animation controls (key mode, playback, frame
  spinner) between the type-in and the navigation controls.
- `MainWindow::addCommandPanelTab(page, icon, title)` keeps the order Create, Modify, Hierarchy,
  Motion, Display, Utilities (other titles go last). Tabs show icons with the title as tooltip.
- **Commands owned by other modules** (link, mirror, align, array, snaps, group, graph editors,
  render animation) appear in menus and the toolbar as placeholders made by
  `proxyCommand(id, text, icon)`. After all extensions ran, `resolveProxyCommands()` binds each
  placeholder to `command(id)`: it forwards triggers, mirrors enabled/checked state and shows the
  module's shortcut. A placeholder whose command does not exist stays disabled. Built-in fallbacks
  are registered when no module provides them: `tools.unlink`, `tools.selectAndLink` (drag from
  child to parent), `snaps.toggle` (home-grid snapping), `snaps.angle`, `snaps.percent`,
  `snaps.spinner` and `edit.mirror` (Mirror dialog).
- **`UiState`** (`uistate.h`): region shape, window/crossing, reference coordinate system and
  picked object, pivot center, scale mode, selection lock, shortcut override, absolute/offset
  type-in, adaptive degradation, ViewCube/gizmo/bracket/frozen display options, display colors and
  Display panel category filters. Viewports repaint when it changes.
- **`Units`** (`units.h`): display units (generic, mm, cm, m, km, inches, feet, feet with
  fractional inches) over system units (1 unit = N cm). `UnitSpinBox` shows and parses lengths in
  display units ("25.4mm", "1'6\"", "0.5m"); `ParamForm` uses it for length parameters
  (`ParamDef::unit = "length"` or common keys such as width, radius, height).
- **Interface scene data** (`interfacedata.h`, saved in `Scene::extensions["interface"]`, so it
  is undoable and saved with the scene): named selection sets, scene states, file properties,
  XRef scenes, transform locks and the working pivot.
- **Viewports**: `Viewport` adds the ViewCube (`viewcube.h`), the label menus
  `[+] [View] [Quality] [Shading]`, visual styles (`ShadingMode`: Default Shading, Facets, Clay,
  Bounding Box, Wireframe), lighting (default or scene lights), backgrounds, safe frames, view
  undo/redo and named views, selection regions (rectangle, circle, fence, lasso, paint;
  `pickNodesInPolygon()`, `pickNodesWithBrush()`), reference coordinate systems and pivot centers
  (`transformFrame()`), scale modes and selection lock. `ViewportPanel` has 14 layouts
  (`setViewLayout()`, `layoutCells()` for previews) and Alt+W maximize.
- **Viewport tools** in `edittools.h` (Select and Place, Measure Distance, Select and Manipulate,
  Select and Link, object picking, home-grid snapper) and `navtools.h` (Zoom, Zoom All,
  Field-of-View, Zoom Region, Pan, Orbit, Orbit Selected/SubObject).
- **Scripting** (`scripting.h`, `scriptui.h`): embedded CPython (CMake option
  `VERAA_WITH_PYTHON`, on when Python development files are found). The `veraa` module wraps
  scene access (`create`, `nodes`, `node`, `select`, `Node` properties and parameters,
  modifiers, `command(id)` for any registered command, import/export, undo). Startup scripts run
  from the user's startup scripts and plug-in folders. The macro recorder turns scene edits and
  command triggers into Python lines.

### Command ids

Every action is registered with `registerCommand(id, action)` (used by the Hotkey Editor, UI
schemes, scripts via `veraa.command(id)` and other modules via `MainWindow::command(id)`).

| Group | Ids |
|---|---|
| File | `file.new`, `file.newKeepObjects`, `file.newKeepHierarchy`, `file.reset`, `file.open`, `file.viewImage`, `file.save`, `file.saveAs`, `file.saveCopyAs`, `file.saveSelected`, `file.archive`, `file.import`, `file.merge`, `file.replace`, `file.importObj`, `file.export`, `file.exportSelected`, `file.xrefObjects`, `file.xrefScene`, `file.reloadXRefs`, `file.projectCreate`, `file.projectSet`, `file.summaryInfo`, `file.fileProperties`, `file.exit` |
| Edit | `edit.undo`, `edit.redo`, `edit.hold`, `edit.fetch`, `edit.delete`, `edit.clone`, `edit.select`, `edit.move`, `edit.rotate`, `edit.scale`, `edit.scaleNonUniform`, `edit.scaleSquash`, `edit.placement`, `edit.transformTypeIn`, `edit.transformToolbox`, `edit.selectAll`, `edit.selectNone`, `edit.selectInvert`, `edit.selectSimilar`, `edit.selectInstances`, `edit.selectByName`, `edit.selectByColor`, `edit.selectByLayer`, `edit.selectByMaterial`, `edit.regionRectangle`, `edit.regionCircle`, `edit.regionFence`, `edit.regionLasso`, `edit.regionPaint`, `edit.windowCrossing`, `edit.selectionSets`, `edit.editNamedSets`, `edit.objectProperties`, `edit.hideSelection`, `edit.unhideAll`, `edit.lockSelection`, `edit.absoluteOffsetToggle`, `edit.pivotPointCenter`, `edit.selectionCenter`, `edit.coordinateCenter`, `edit.manipulate`, `edit.cancel`, `edit.mirror`* |
| Axis | `axis.x`, `axis.y`, `axis.z`, `axis.plane`, `axis.cycle` |
| Tools | `tools.sceneExplorer`, `tools.layerExplorer`, `tools.isolate`, `tools.endIsolate`, `tools.displayFloater`, `tools.sceneStates`, `tools.lightLister`, `tools.renameObjects`, `tools.colorClipboard`, `tools.grabViewport`, `tools.measureDistance`, `tools.meshInspector`, `tools.convertToPoly`, `tools.selectAndLink`*, `tools.unlink`*, `tools.bindSpaceWarp`*, `tools.array`*, `tools.align`*, `tools.quickAlign`*, `tools.normalAlign`*, `tools.alignToView`*, `tools.snapshot`*, `tools.spacing`*, `tools.previewGrab`* |
| Snaps | `snaps.toggle`*, `snaps.angle`*, `snaps.percent`*, `snaps.spinner`*, `snaps.settings`* |
| Group | `group.group`*, `group.ungroup`*, `group.open`*, `group.openRecursively`*, `group.close`*, `group.attach`*, `group.detach`*, `group.explode`* |
| Views | `views.top`, `views.bottom`, `views.front`, `views.left`, `views.perspective`, `views.orthographic`, `views.camera`, `views.undoView`, `views.redoView`, `views.viewportConfiguration`, `views.saveActiveView`, `views.restoreActiveView`, `views.grid`, `views.background`, `views.showGizmo`, `views.shadeSelected`, `views.showViewCube`, `views.safeFrame`, `views.cameraFromView`, `views.defaultShading`, `views.edgedFaces`, `views.redrawAll`, `views.zoomExtentsSelected`, `views.zoomExtents`, `views.zoomExtentsAll`, `views.zoomExtentsAllSelected`, `views.maximize`, `views.expertMode`, `views.adaptiveDegradation`, `views.zoom`, `views.zoomAll`, `views.fieldOfView`, `views.zoomRegion`, `views.pan`, `views.orbit`, `views.orbitSelected`, `views.orbitSubObject` |
| Graph editors | `graph.curveEditor`*, `graph.dopeSheet`*, `graph.schematic`* |
| Rendering | `render.render`, `render.setup`, `render.frameWindow`, `render.materialEditor`, `render.animation`* |
| Customize | `customize.ui`, `customize.hotkeys`, `customize.loadScheme`, `customize.saveScheme`, `customize.defaultsSwitcher`, `customize.revertUi`, `customize.lockUi`, `customize.projectPaths`, `customize.userPaths`, `customize.units`, `customize.pluginManager`, `customize.preferences`, `customize.shortcutOverride` |
| Show UI | `ui.commandPanel`, `ui.sceneExplorer`, `ui.layerExplorer`, `ui.mainToolbar`, `ui.axisToolbar`, `ui.toggleSceneExplorer`, `ui.toggleLayerExplorer` |
| Scripting | `scripting.new`, `scripting.open`, `scripting.run`, `scripting.listener`, `scripting.editor`, `scripting.macroRecorder`, `scripting.recordToEditor`, `scripting.help`, `scripting.openFolder` |
| Help | `help.shortcuts`, `help.userGuide`, `help.whatsNew`, `help.about` |

\* Provided by another module when installed (the interface shows a placeholder bound at
startup); `edit.mirror`, `tools.unlink`, `tools.selectAndLink` and the snaps toggles have
built-in fallbacks.

## File formats (`src/core/io_formats.h`)

`importScene()` / `exportScene()` pick the format from the extension. Import: OBJ, FBX (ufbx),
glTF/GLB (cgltf), STL, PLY, 3DS. Export: OBJ + MTL, FBX (ASCII 7.4), glTF/GLB, STL, PLY.
FBX and glTF keep the hierarchy, transforms, pivot offsets and basic materials; glTF and FBX
units/axes are converted to Veraa's Z-up centimeters. `tests/io_tests.cpp` checks round trips.

## Undo

Every scene edit goes through `SceneDocument::beginEdit()/endEdit()` (or `softEdit()` for
spinner drags). Undo restores a scene JSON snapshot, so new data saved by a module (in
`Scene::extensions` or in object/modifier JSON) is undoable automatically.

## Tests

- `tests/core_tests.cpp`, `tests/render_tests.cpp`, `tests/io_tests.cpp` (CTest).
- `Veraa --script selftest` drives the real UI under Xvfb. Feature modules add their own UI
  tests in their own files with `VERAA_SELF_TEST(name, fn)` (helpers in `selftest.h`);
  `Veraa --script <name>` runs one of them.
- `Veraa --script demo --screenshot out.png` saves screenshots of the main windows.
- `Veraa --script interface` runs the interface tests; with `VERAA_UI_SCREENS=<folder>`,
  `Veraa --script interfaceScreens` saves screenshots of menus, dialogs and panels.
