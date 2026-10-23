# Shapes and construction

This module adds splines, spline editing, the modifiers that turn splines into meshes, 3D text,
compound objects, AEC (architecture) objects, patch grids and NURBS curves.

## Create panel

- **Shapes > Splines:** Line, Rectangle, Circle, Ellipse, Arc, Donut, NGon, Star, Text, Helix,
  Egg, Section, Freehand.
- **Shapes > NURBS Curves:** Point Curve (passes through its points) and CV Curve (a cubic
  B-spline pulled by its control vertices, which show as a green lattice while selected).
- **Shapes > Extended Splines:** Walled Rectangle, Channel, Angle, Tee, Wide Flange.
- **Geometry > Standard Primitives:** TextPlus (extruded, optionally bevelled text).
- **Geometry > Compound Objects:** Scatter, Conform, Connect, BlobMesh, ShapeMerge, Terrain, Loft.
- **Geometry > Patch Grids:** Quad Patch, Tri Patch.
- **Geometry > Doors / Windows / AEC Extended / Stairs:** Pivot, Sliding and BiFold doors;
  Awning, Casement, Fixed, Pivoted, Projected and Sliding windows; Foliage, Railing, Wall;
  L-Type, Spiral, Straight and U-Type stairs.

## Interactive creation

- **Line:** click for corner vertices, click-drag for Bezier vertices, Shift keeps segments
  orthogonal, Backspace removes the last vertex, right-click or Enter finishes. Clicking the first
  vertex asks "Close spline?".
- **Wall, Point Curve, CV Curve:** click points; right-click finishes; clicking the first point
  asks to close.
- **Freehand:** drag to draw; a stroke ending on its start closes.
- **Drag to size:** Rectangle, Ellipse and the patch grids drag corner to corner. Circle, NGon
  and the spiral stair drag a radius. Donut, Star and Helix continue with mouse moves for their
  second radius or height. Arc drags its chord and then bends through a third point. The Extended
  Splines finish with a thickness move. Doors, windows and stairs drag along their width or run,
  then move for depth or width and height.
- **Click to place:** Text, TextPlus, Foliage, BlobMesh.
- **From the selection:** choosing Loft, Scatter, Connect, ShapeMerge, Terrain or Conform turns
  the selected object into the compound's first operand. Loft paths and Terrain contours are kept;
  the other objects move into the compound.

## Spline editing

Editable Spline objects (Line, Freehand, Convert to Editable Spline) and the Edit Spline
modifier show **Vertex / Segment / Spline** buttons. The keys 1, 2 and 3 switch levels while a
viewport has focus, and Delete removes the selection.

- **Selecting and moving:** click or drag a region to select, with Ctrl to add and Alt to remove.
  Drag selected items to move them. Selected Bezier vertices show handles you can drag. Bezier
  handles stay mirrored; Bezier Corner handles move independently.
- **Vertex tools:** Corner, Smooth, Bezier and Bezier Corner types; Refine; Insert (adds a vertex
  and drags it); Connect (two open ends); Cross Insert; Weld (with a threshold); Fuse; Break;
  Make First; Delete; Fillet and Chamfer.
- **Segment tools:** Divide, Detach (or Copy), Delete, Hide, Unhide All.
- **Spline tools:** Outline (with Center), Boolean (Union, Subtraction or Intersection with a
  picked spline), Mirror (Horizontal, Vertical or Both, with Copy), Reverse, Close, Explode,
  Detach, Delete, Trim, Extend.
- **Attach and Attach Mult.:** add other shapes to this one.
- Every operation is one undo step.

**Tools > Convert to Editable Spline** is also in the quad menu. Shape nodes whose stack ends in
a mesh (an Extrude, for example) can be converted to Editable Poly.

## Modifiers

These apply to shapes and are listed in the Modifier List:

- **Extrude:** amount, segments, Morph or Grid capping.
- **Lathe:** degrees, segments, X/Y/Z direction, Min/Center/Max alignment, weld core, flip
  normals, caps.
- **Sweep:** 10 built-in sections or a picked Custom section; pivot alignment, offsets, angle,
  mirrors, banking, smoothing.
- **Bevel:** start outline and up to three levels, linear or curved sides.
- **Bevel Profile:** follows a picked profile.
- **Edit Spline**, **Normalize Spl.**, **Fillet/Chamfer**.
- **Trim/Extend:** uses Pick Locations.
- **Renderable Spline:** radial or rectangular section.

Closed shapes cap into closed, consistently outward meshes, including shapes with holes and
islands. Material IDs follow the usual convention: start cap 1, end cap 2, sides 3. The code
stores them 0-based.

## Material IDs of construction objects

The UI shows each ID one higher than stored.

| Object | IDs |
|---|---|
| Doors | front 1, back 2, panels/glass 3, frame 4, leaf edges 5 |
| Windows | frame 1, sash rails 2, glass 3 |
| Wall | vertical ends 1, outside 2, inside 3, top 4, bottom 5 |
| Railing | top rail 1, lower rails 2, posts 3, fencing 4 |
| Stairs | risers 1, treads 2, stringers 3, center pole 4, handrails 5, carriage 6 |
| Foliage | trunk 1, branches 2, leaves 3, fruit 4, flowers 5 |

## Code map

| Area | Files |
|---|---|
| Core | `shape2d` (capping, booleans, offsets), `shapes` and `font` (shape types, text), `spline_ops` (editing operations), `shape_mesh` (extrude, lathe, sweep, path frames), `shape_modifiers`, `compound`, `aec_walls` / `aec_doors` / `aec_stairs`, `patch_nurbs`, `meshbuild` |
| App | `shapes_create.cpp` (creation tools), `spline_edit.cpp` (sub-object editing), `shapes_rollouts.cpp` (Modify panel rollouts, compound creation, Convert to Editable Spline), `shapes_selftest.cpp` |
| Tests | `tests/shape_tests.cpp`; the UI test runs with `Veraa --script shapes` |

Fonts are searched in `assets/fonts` (bundled DejaVu fonts), next to the executable (`fonts/`)
and in the system font folders.

## Known limits

- Hidden segments are hidden only while editing. They are excluded from selection but still
  drawn and used by modifiers, and they are not saved.
- Doors and windows don't cut openings in walls.
- Edit Spline stores the edited shape. Later changes to the parameters of the object below it
  don't flow through once the modifier has edits.
- Patch grids are flat patch-topology meshes. There is no Editable Patch.
- NURBS curves are edited by recreating them or converting to Editable Spline. There is no CV
  sub-object editing.
