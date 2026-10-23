# Interface parity with the 3ds Max 2022 reference screenshots

Veraa follows the layout and workflow of 3ds Max 2022. This file lists, for each of the 32
reference screenshots the interface was compared against, the elements shown and Veraa's state
before this pass ("Before") and after it ("Now").

Status words: **present** (same element, same place), **differs** (exists, but label, order or
place differ), **missing** (not in Veraa), **inert** (shown but did nothing or was disabled).
"Placeholder" means the item is in the same place and opens a clear "not available in Veraa yet"
message, because the feature itself does not exist yet.

Tools used for the comparison:

- `VERAA_UI_DUMP=<file> Veraa --script uiDump` writes every menu (with command ids, shortcuts and
  enabled state), the main toolbar, all commands and the command panel tabs as text.
- `VERAA_UI_SCREENS=<dir> Veraa --script uiParityScreens` saves the empty main window at
  1920x1050, every top menu, every command panel tab and every Create category with its
  subcategory list (file names `p01_...`, `p1xx_menu_...`, `p2x_panel_...`, `p3x_create_...`).
- `Veraa --script uiParity` (part of `--script selftest`) checks the structure below.

Names of other companies' products are not reproduced: third-party plug-in menus (Substance,
Civil View, Arnold), "Open Autodesk A360 Gallery", and V-Ray / Arnold entries in the Create
panel. Where the reference shows a V-Ray entry, Veraa shows its own renderer's equivalent
("Veraa" subcategory) in that slot. "MAXScript" is "Script" (Veraa scripts in Python) and
"Max Creation Graph" is "Creation Graph".

## 01 - Main window

| Element | Before | Now |
|---|---|---|
| Title "Untitled - <app>" | present | present |
| Menu bar, Workspaces selector at the right | present | present |
| Main toolbar under the menu bar | differs (modules appended duplicate buttons at the end) | present |
| Axis Constraints toolbar on the left: X, Y, Z, XY flyout, Snaps Use Axis Constraint | differs (Cycle Planes and Snaps Toggle instead of the last button) | present |
| No Scene Explorer docked on the left | differs (Scene / Layer Explorer docked left) | present (hidden by default, toolbar toggles show them) |
| 2x2 viewports, labels `[+] [Top] [Standard] [Wireframe]`, Perspective `[Default Shading]` | present | present |
| ViewCube in every viewport, axis tripod lower left | present | present |
| Time slider `< 0 / 100 >` and track bar across the full window width | differs (only under the viewports; command panel reached the status bar) | present |
| Command panel on the right, ending above the time slider | differs | present |
| Status bar and animation controls in two rows | differs (key/playback controls in a third row under the track bar) | present |

## 02 - File menu

New >, Reset, Open... (Ctrl+O), Open Recent >, View Image File... | Save (Ctrl+S), Save As...
(Shift+Ctrl+S), Save Copy As..., Save Selected..., Archive... | Import >, Export >, Share View...,
Send to >, Reference >, Project >, Summary Info..., File Properties..., Preferences... | Exit.

| Element | Before | Now |
|---|---|---|
| Items and order | differs (separator after Reset, extra separators around Reference/Project and Preferences) | present |
| Share View... | missing | placeholder |
| Send to > | missing | placeholder submenu |
| Save Selected disabled with nothing selected | present | present |

## 03 - Main toolbar

Undo, Redo | Select and Link, Unlink Selection, Bind to Space Warp | Selection Filter "All" | Select
Object, Select by Name, Selection Region flyout, Window/Crossing | Select and Move, Select and
Rotate, Scale flyout, Select and Place flyout | Reference Coordinate System "View", Use Center
flyout | Select and Manipulate, Keyboard Shortcut Override | Snaps (2D/2.5D/3D flyout), Angle
Snap, Percent Snap, Spinner Snap | Edit Named Selection Sets, Create Selection Set | Mirror, Align
flyout | Toggle Scene Explorer, Toggle Layer Explorer | Toggle Ribbon, Curve Editor, Schematic
View | Material Editor flyout | Render Setup, Rendered Frame Window, Render Production flyout.

| Element | Before | Now |
|---|---|---|
| Order and grouping | differs (snaps, link, unlink, mirror, align, bind appended a second time) | present |
| Select and Place flyout (Place / Rotate) | differs (single button) | present |
| Snaps Toggle 2D / 2.5D / 3D flyout and right-click settings | differs (only on the duplicate button) | present |
| Toggle Ribbon | missing | present (shows/hides the Modeling ribbon) |
| Material Editor flyout (Compact / Slate) | differs (single button) | present |
| Render Production flyout (Production / Iterative / ActiveShade) | differs (single button) | present (ActiveShade placeholder) |

## 04 - Bottom of the window

Time slider, track bar with mini curve editor toggle; status bar: mini listener, selection count
and prompt, Isolate / Selection Lock / Absolute-Offset toggles, X Y Z type-in, Grid = ...,
Safe Scene Script Execution "Enabled", Add Time Tag; playback controls with key mode, frame
spinner and Time Configuration; Set Keys; Auto Key / Set Key; Selected / Key Filters; navigation
controls (4 x 2).

| Element | Before | Now |
|---|---|---|
| Mini listener, selection count, prompt, type-in, grid | present | present |
| Safe Scene Script Execution indicator | missing | present |
| Add Time Tag | missing | present |
| Playback / Set Keys / Auto Key / Set Key / Key Filters / frame / Time Configuration | differs (separate row under the track bar) | present (in the status bar, two rows) |
| Default In/Out Tangents flyout next to Set Key | missing | present |
| Mini curve editor toggle on the track bar | missing | present (opens the Curve Editor) |
| Navigation controls | present | present |

## 05 - Edit menu

Undo, Redo | Hold (Ctrl+H), Fetch (Alt+Ctrl+F) | Delete, Clone (Ctrl+V) | Move (W), Rotate (E),
Scale, Placement, Transform Type-In... (F12), Transform Toolbox... | Select All, Select None,
Select Invert | Select Similar, Select Instances, Select By > | Selection Region > | Manage
Selection Sets... | Object Properties...

| Element | Before | Now |
|---|---|---|
| Items and order | differs (Select By / Selection Region not separated; Select and Link / Unlink appended at the end) | present |

## 06 - Tools menu

Scene Explorer..., Layer Explorer..., Crease Explorer..., All Global Explorers >, Manage Local
Explorers..., Local Scene Explorers > | Containers > | Isolate Selection (Alt+Q), End Isolate |
Display Floater... | Manage Scene States..., Manage Templates... | Light Lister... | Mirror...,
Array..., Align >, Snapshot... | Rename Objects... | Assign Vertex Colors..., Color Clipboard...,
Perspective Match... | Viewport Canvas... | Preview - Grab Viewport >, Grids and Snaps >, Measure
Distance... | Channel Info..., Mesh Inspector >

| Element | Before | Now |
|---|---|---|
| Order | differs (Mirror, Array, Snapshot, Spacing, snap toggles, Collapse Stack, Convert to Editable Spline and a second Preview submenu appended at the end) | present |
| Spacing Tool | differs (top level) | present (in Align >) |
| Crease Explorer, All Global Explorers, Manage Local Explorers, Local Scene Explorers | missing | All Global Explorers lists the explorers; the others are placeholders |
| Containers >, Manage Templates..., Assign Vertex Colors..., Perspective Match..., Viewport Canvas..., Channel Info... | missing | placeholders |
| Mesh Inspector > | differs (dialog item) | present (submenu) |

## 07 - Group menu

Group..., Ungroup | Open, Open Recursively, Close | Attach, Detach | Explode | Assembly >

| Element | Before | Now |
|---|---|---|
| Order and separators | differs (all separators at the top, items below) | present |
| Assembly > | missing | placeholder submenu |

## 08 - Rendering menu

Render (Shift+Q), Render in the Cloud, Open A360 Gallery, Render Setup... (F10), Rendered Frame
Window... | State Sets..., Camera Sequencer... | Scene Converter... | Exposure Control... |
Environment... (8), Effects... | Render to Texture..., Bake to Texture..., Render Surface Map... |
Material Editor >, Material/Map Browser..., Material Explorer... | Video Post... | Panorama
Exporter..., Batch Render..., Print Size Assistant... | Gamma/LUT Setup..., Render Message
Window..., Compare Media in RAM Player...

| Element | Before | Now |
|---|---|---|
| Order | differs (empty separators, Material Editor at top level, extras mixed in) | present |
| Exposure Control..., Environment..., Effects... | differs (one "Environment and Effects..." item) | present (open the matching tab) |
| Material Editor > Compact / Slate | differs | present |
| Render in the Cloud, State Sets, Camera Sequencer, Scene Converter, Render to Texture, Bake to Texture, Render Surface Map, Material/Map Browser, Material Explorer, Video Post, Panorama Exporter, Render Message Window, RAM Player | missing | placeholders |
| Gamma/LUT Setup... | missing | present (opens Preferences) |
| Open A360 Gallery | missing | not reproduced (other company's service) |
| Veraa extras: Render Animation, Add Sun and Sky, Export Proxy | present | kept next to the related items |

## 09 - Animation menu

Load Animation..., Save Animation... | IK Solvers > | Constraints > | Transform, Position,
Rotation, Scale, MCG Controllers >, CAT >, MassFX >, Fluids > | Offset Controllers >, Parameter
Editor..., Parameter Collector... | Wire Parameters > | Animation Layers... | Reaction Manager... |
Bone Tools... | Set as Skin Pose, Assume Skin Pose, Skin Pose Mode | Toggle Limits | Delete
Selected Animation | Populate >, Walkthrough Assistant..., Presets >

| Element | Before | Now |
|---|---|---|
| Order and separators | differs | present |
| MassFX > | differs ("Simulation - Rigid Bodies" lower down) | present (Veraa's rigid body simulation in that slot) |
| Wire Parameters > | differs (single item) | present (submenu) |
| Fluids >, Offset Controllers >, Skin Pose Mode, Toggle Limits, Populate >, Presets > | missing | placeholders |
| MCG Controllers, CAT | missing | not reproduced (product-specific names) |

## 10 - Modifiers menu

Selection Modifiers, Patch/Spline Editing, Mesh Editing, Conversion, Animation, Cloth, Hair and
Fur, UV Coordinates, Cache Tools, Subdivision Surfaces, Free Form Deformers, Parametric Deformers,
Surface, NURBS Editing, Radiosity, Cameras, V-Ray (all submenus).

| Element | Before | Now |
|---|---|---|
| Groups with modifiers | present, same order | present |
| Cloth, Hair and Fur, Cache Tools, NURBS Editing, Radiosity, Cameras | missing (empty groups were hidden) | placeholder submenus |
| V-Ray group | differs ("Rendering" group with VeraaDisplacementMod) | present (named "Veraa", in the V-Ray slot) |

## 11 - Create menu

Standard Primitives, Extended Primitives, AEC Objects, Compound, Particles, Patch Grids, NURBS,
Point Cloud, Dynamics, V-Ray | Shapes, Extended Shapes | Lights, Cameras | Helpers, SpaceWarps,
Systems | Fluids.

| Element | Before | Now |
|---|---|---|
| Structure | differs (one submenu per registry subcategory, e.g. "Doors", "Lights - Standard") | present (3ds Max grouping; AEC Objects holds doors, windows, stairs, walls, railings, foliage) |
| Point Cloud, Dynamics, Fluids | missing | placeholders |

## 12 - Customize menu

Customize User Interface..., Hotkey Editor... | Load / Save Custom UI Scheme... | Custom Defaults
Switcher... | Revert to Startup UI Layout, Lock UI Layout | Show UI >, Show Editors >, Show
Explorers > | Configure Project Paths..., Configure User and System Paths... | Units Setup... |
Plug-in Manager... | Preferences...

| Element | Before | Now |
|---|---|---|
| Items and order | present | present |

## 13 - Scripting menu

Install Creation Graph Package... | New / Open Creation Graph, Creation Graph Editor | Create
Graph Geometry..., Create and Apply Graph Modifier..., Create Compound From Mesh... | New Script,
Open Script..., Run Script... | Script Listener... (F11), Script Editor... | Macro Recorder |
Visual Script Editor... | Debugger Dialog... | Script Help... | Python Help...

| Element | Before | Now |
|---|---|---|
| New / Open / Run Script, Listener (F11), Editor, Macro Recorder | present | present |
| Creation Graph items, Visual Script Editor, Debugger Dialog | missing | placeholders |
| Script Help..., Python Help... | differs ("Python API Help") | present |

## 14, 15, 16 - Substance, Civil View, Arnold menus

Third-party plug-in menus. Not reproduced (other products' names); Veraa's own renderer is in the
Rendering menu and Render Setup. A **Content** menu (Asset Library...) takes the slot of the
reference's Content menu.

## 17 - Workspaces

Default, Alt Menu and Toolbar, Design Standard, Main Toolbar - modular, Modular-mini | Manage
Workspaces, Reset To Default State.

| Element | Before | Now |
|---|---|---|
| Default, Manage Workspaces, Reset to Default State | present | present |
| Alt Menu and Toolbar, Design Standard, Main Toolbar - modular, Modular-mini | missing | present (built-in layouts) |

## 18 - Modify panel

Name and color, Modifier List, modifier set buttons (Extrude, UVW Map, Edit Spline, Cap Holes,
Shell, FFD 2x2x2, Slice, Sweep, Edit Poly, FFD 4x4x4), modifier stack.

| Element | Before | Now |
|---|---|---|
| Name, color, Modifier List, stack | present | present |
| Modifier set buttons | missing | present (disabled without a selection) |
| Pin Stack, Show End Result, Make Unique, Remove, Configure Modifier Sets under the stack | differs (delete / up / down only) | present |

## 19 - Hierarchy panel

Name and color; Pivot | IK | Link Info; Adjust Pivot (Move/Rotate/Scale: Affect Pivot Only,
Affect Object Only, Affect Hierarchy Only; Alignment: Center to Object, Align to Object, Align to
World; Pivot: Reset Pivot); Working Pivot (Edit Working Pivot, Use Working Pivot, ..., Align To
View, Reset, Place Pivot To: View, Surface, Align To View, Pin Working Pivot); Adjust Transform
(Don't Affect Children; Reset: Transform, Scale); Skin Pose.

| Element | Before | Now |
|---|---|---|
| Name and color field | missing | present |
| IK button | missing | present (IK page: solver commands) |
| Grouped Adjust Pivot | differs (flat list) | present |
| Working Pivot buttons | differs | present |
| Don't Affect Children | missing | present |
| Skin Pose rollout | missing | present |

## 20 - Motion panel

Name and color; Selection Level: Sub-Object + list; Parameters | Motion Paths; Assign Controller.

| Element | Before | Now |
|---|---|---|
| Name and color, Selection Level row | missing | present |
| Motion Paths button | differs ("Trajectories") | present |

## 21 - Display panel

Name and color; Display Color; Hide by Category (Geometry, Shapes, Lights, Cameras, Helpers, Space
Warps, Particle Systems, Bone Objects; All / None / Invert; filter list with Add / Remove / None);
Hide; Freeze (closed); Display Properties.

| Element | Before | Now |
|---|---|---|
| Name and color field | missing | present |
| Particle Systems, Bone Objects categories | missing | present |
| Category list with Add / Remove / None | missing | present |
| Hide, Freeze, Display Properties | present | present (Freeze starts closed) |

## 22 - Utilities panel

More..., Sets, configure button; Perspective Match, Collapse, Color Clipboard, Measure, Motion
Capture, Reset XForm, MAXScript, Flight Studio.

| Element | Before | Now |
|---|---|---|
| More..., Sets | present | present (+ configure button) |
| Default button set, one column | differs (two columns, other utilities) | present (Script in the MAXScript slot; Perspective Match, Motion Capture, Flight Studio are placeholders; Veraa's other utilities stay under More...) |

## 23 - 32 Create panel

Category buttons Geometry, Shapes, Lights, Cameras, Helpers, Space Warps, Systems; subcategory
list; Object Type rollout with AutoGrid; Name and Color rollout.

| Element | Before | Now |
|---|---|---|
| AutoGrid check box | missing | present |
| Name and Color rollout always shown | differs (only while creating) | present |
| 23 Geometry list: Standard Primitives, Extended Primitives, Compound Objects, Particle Systems, Patch Grids, Body Objects, Doors, NURBS Surfaces, Windows, AEC Extended, Point Cloud Objects, Dynamics Objects, Stairs, VRay, Alembic, Arnold, Fluids | differs (registry order, no placeholders) | present (Veraa in the VRay slot; Body Objects, NURBS Surfaces, Point Cloud Objects, Dynamics Objects, Alembic, Fluids show a "not available yet" note) |
| 01 Standard Primitives buttons: Box, Cone, Sphere, GeoSphere, Cylinder, Tube, Torus, Pyramid, Teapot, Plane, TextPlus | differs (Plane before Teapot) | present |
| 24 Shapes: Splines, NURBS Curves, Compound Shapes, Extended Splines, Creation Graph | differs | present (Compound Shapes, Creation Graph placeholders) |
| 25 Lights: Photometric (Target Light, Free Light, Sun Positioner), Standard, VRay | differs (Standard first) | present (Photometric first; Target / Free Light create Veraa IES lights) |
| 26 Cameras: Standard (Physical, Target, Free), VRay | differs (one "Camera" button) | present |
| 27 / 28 Helpers: Standard (Dummy, Container, Crowd, Delegate, ExposeTm, Grid, Point, Tape, Protractor, Influence, Arrow, Volume, Compass) and other lists | missing (empty category) | present (Dummy, Point, Tape, Protractor, Compass, Arrow work; the others are placeholders) |
| 29 / 30 Space Warps: Forces (Push, Motor, Vortex, Drag, PBomb, Path Follow, Gravity, Wind, Displace, Motion Field), Deflectors, Geometric/Deformable, Modifier-Based, Particles & Dynamics | differs (order; Path Follow, Displace, Motion Field and two lists missing) | present (missing ones are placeholders) |
| 31 / 32 Systems: Standard (Bones, Ring Array, Biped, Sunlight, Daylight), VRay | differs (Bones only) | present (Sunlight and Daylight create Veraa's sun; Ring Array, Biped placeholders) |
