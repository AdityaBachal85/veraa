# 3ds Max `.max` files: layout notes

These notes describe what Veraa's importer (`src/core/io_max.cpp`, `src/core/cfb.cpp`) relies
on. They were worked out by inspecting scene files (hex dumps, comparing values that are visible
in the scene such as sizes, positions and colors, and checking that counts add up), not from any
Autodesk code. Only one real sample was available (saved by 3ds Max 9 with V-Ray 1.50), so the
importer treats every rule below as a hypothesis: reads are bounds-checked, unknown chunks are
skipped, and anything it cannot interpret becomes a warning. Newer versions (e.g. 3ds Max 2022)
may differ in details; the parts most likely to change are marked *(unverified)*.

## Container

A `.max` file is an OLE2 / Compound File Binary container (signature `D0 CF 11 E0 A1 B1 1A E1`,
512-byte sectors, FAT + mini-FAT + DIFAT as in the public MS-CFB specification). Root streams:

| Stream | Content |
| --- | --- |
| `ClassDirectory3` (older: `ClassDirectory2`) | classes used by the scene |
| `DllDirectory` | plug-in DLLs the classes come from |
| `Scene` | all scene objects |
| `Config`, `VideoPostQueue`, `ClassData` | settings (ignored) |
| `\x05SummaryInformation`, `\x05DocumentSummaryInformation` | OLE property sets; the latter contains a `3ds max Version: N` string that is reported |
| `FileAssetMetaData*` (newer files) | asset (bitmap) paths, keyed by a 16-byte id *(unverified)* |

A stream may be zlib-compressed in newer versions (starts with `78 9C` / `78 DA`); the importer
inflates it before parsing *(unverified, no compressed sample)*.

## Chunks

`ClassDirectory3`, `Scene` and others are trees of chunks:

```
uint16 id
uint32 size            high bit set = the payload is a list of child chunks
                       size == 0: a uint64 size follows (bit 63 = children)
payload (size includes the 6- or 14-byte header)
```

## Class directory

One container per class, each with:

- `0x2060`: `int32 dll index, uint32 class id A, uint32 class id B, uint32 superclass id`
- `0x2042`: class name (UTF-16)

Superclass ids seen: `0x01` node, `0x10` geometry, `0x20` camera, `0x30` light, `0x40` shape,
`0x50` helper, `0x810` modifier, `0xC00` material, `0xC10` texture map, `0x90xx` controllers
(`0x9003` float, `0x9008` transform, `0x900B` position, `0x900C` rotation, `0x900D` scale).
Class ids seen: Editable Poly `1BF8338D:192F6098`, Node `1:0`, Standard material `2:0`,
Multi/Sub-Object `200:0`, Teapot `ACAD13D3:ACAD26D9`, Plane `081F1DFC:77566F65`.

## Scene stream

The first top-level chunk (id `0x200E` in the sample, presumably a version marker) contains one
child chunk per scene object. **The child's chunk id is the object's class index** in the class
directory, and objects are referred to by their position in this list.

Common chunks inside an object:

- `0x2034`: references, an `int32` object index per reference slot (`-1` = none).
- `0x2035`: references as a leading `int32` followed by `(slot, index)` pairs.
- `0x2150`, `0x2045`, `0x2047`, `0x21B0`: custom attributes / app data (ignored).

## Nodes (class `Node`)

| Chunk | Meaning |
| --- | --- |
| `0x0962` | name (UTF-16) |
| `0x0960` | `int32 parent index` (the `RootNode` object for top-level nodes), `int32 flags` |
| `0x096A` | object offset position (pivot), 3 floats |
| `0x096B` | object offset rotation, quaternion x y z w |
| `0x096C` | object offset scale: 3 floats + scale-axis quaternion |
| `0x0974` | wire color, bytes R G B 0 |
| references | slot 0 transform controller, slot 1 object, slot 3 material, slot 6 layer |

Instances are simply several nodes referencing the same object (the sample has 16 nodes sharing
one Editable Poly); Veraa makes them instances too.

## Controllers

The node transform controller (`Position/Rotation/Scale`) references position, rotation and scale
controllers; the node's local matrix is `T * R * S` relative to the parent node.

- `Bezier Float`: current value in `0x2501` (float). Keyed animation is not read.
- `Position XYZ` / `Euler XYZ`: three float controllers; Euler angles are radians, `0x1003` is the
  axis order (0 = XYZ).
- `Bezier Scale`: `0x2505` = scale x y z + axis quaternion.
- Point3 / quaternion values in `0x2503` / `0x2504` *(unverified)*. Quaternions are stored with
  the opposite rotation sense to the usual convention *(unverified)*.
- List controllers are summed / composed; anything else is taken as identity with a warning.
- Look-at controllers (target cameras/lights): only the position is read; the node is aimed at
  the node named `<name>.Target`.

## Parameter blocks

**ParamBlock2** (class `ParamBlock2`):

- `0x0009` header: `uint32 owner class index, uint16 block id, uint16, uint16, uint16 parameter
  count, uint32 owner object index`.
- one `0x000E` chunk per parameter: `uint16 id, uint32 type, 9 bytes of flags, value`.
  Types follow the usual ParamBlock2 numbering (0 float, 1 int, 2 color, 4 bool, 5 angle, 6
  percent, 7 world units, 8 string, 9 file name, 14 material, 15 texture map, ...; `0x800` = array).
  Scalars are 4 bytes, colors 12 bytes, strings `uint32 length` + bytes, materials/maps are
  object indices. Arrays: `uint32 count`, then per element one flag byte + value.

**ParamBlock** (older, class `ParamBlock`): `0x0001` count, then one `0x0002` container per
parameter with `0x0003` (index) and a value chunk `0x0100` float, `0x0101` int, `0x0104` bool,
12-byte chunks for colors.

## Editable Poly

The object contains a `0x08FE` container (the polygon mesh):

| Chunk | Layout |
| --- | --- |
| `0x0100` | `uint32 n`, then per vertex `uint32 flags, float x, y, z` |
| `0x010A` | edges (not needed; rebuilt by Veraa) |
| `0x011A` | `uint32 n`, then per face: `uint32 degree`, `degree × uint32 vertex`, `uint16 mask`, then optional fields in this order: mask `0x01` → `uint32 flags`, `0x08` → `uint16 material id` (0-based), `0x10` → `uint32 smoothing groups`, `0x20` → `(degree-3) × 2 uint32` triangulation diagonals |
| `0x0120` | number of map channels |
| `0x0124` | map channel index, followed by that channel's `0x0128` (`uint32 n`, `n × float3` UVW) and `0x012B` (per face: `uint32 degree`, `degree × uint32` map vertex; no count, one record per face) |

Verified on the sample: all 1,684 face records parse to exactly the end of the chunk, the map
face records match the polygon degrees, and the vertex coordinates give the expected 150-unit
glass block. Faces whose flags have bit `0x2` set are treated as deleted *(unverified)*. If a
face mask has unknown bits, the importer re-synchronises on the next valid face and warns.

## Editable Mesh *(unverified)*

No sample was available. The importer looks for a vertex array (`uint32 n` + `n × float3`,
preferably chunk `0x0914`) and a face array (`uint32 n` + `n × (3 × uint32 index + uint32
flags)`, preferably `0x0912`) whose indices are in range.

## Primitives

Primitive objects reference their parameter block in slot 0.

- Plane (ParamBlock2): 0 length, 1 width, 2/3 segments.
- Teapot (ParamBlock): 0 radius, 1 segments, 2 smooth, 4–7 body/handle/spout/lid.
- Box, Sphere, Cylinder, Cone, Tube, Torus, Pyramid, ChamferBox, GeoSphere: parameter indices
  follow the order of the parameters in 3ds Max's UI (radius/length first, then segments)
  *(unverified, no sample)*.

## Materials

Names are in `0x4000` (Standard, Multi/Sub-Object) or `0x5431` (VRayMtl) → `0x4001` (UTF-16).

- **Standard**: block 0 shader basics, block 1 extended (1 = opacity, 6 = IOR), block 3 maps
  (0 = enabled flags, 1 = map list, 2 = amounts; slot 1 diffuse, 6 opacity, 8 bump). Colors are in
  the shader object (e.g. `Blinn`) referenced by the material: 0 ambient, 1 diffuse, 2 specular,
  9 specular level, 10 glossiness.
- **VRayMtl** (V-Ray 1.5): block 1: 1 diffuse, 2 reflection, 3 reflection glossiness, 5 refraction,
  6 refraction glossiness, 8 Fresnel on, 9 IOR, 23 fog color, 24 fog multiplier, 63 Fresnel IOR.
  Block 4 maps: 100 + n map, 150 + n enabled, 200 + n amount (%), with n = 0 diffuse, 1 reflection,
  2 refraction, 3 bump. Newer V-Ray versions are expected to keep these ids *(unverified)*.
- **Multi/Sub-Object**: parameter 0 = material list, 1 = enabled flags, 3 = material ids (0-based).
- **Falloff** map: 0 front color, 2 front map, 4 side color, 6 side map, 8 type, 9 direction,
  12 IOR.
- **Bitmap**: the file name is taken from a string/file-name parameter or chunk that looks like an
  image path, or an asset id from `FileAssetMetaData*` *(unverified, no sample)*.

## Lights

- **VRayLight** (ParamBlock2): 1 type (0 plane, 1 dome, 2 sphere, 3 mesh), 2 half-length,
  3 half-width (radius for spheres), 7 color, 8 multiplier. Plane lights become Veraa Rect lights
  (width = 2 × half-length), spheres Sphere lights; dome lights are skipped with a warning.
- Standard lights (ParamBlock): 0 color, 1 multiplier *(unverified)*.
- Cameras: ParamBlock index 0 = field of view in radians *(unverified)*.

## Not imported (yet)

Modifiers (the base object is imported and a warning lists the node), splines/shapes, animation
keys, V-Ray maps other than Falloff/Bitmap (e.g. VRayDirt), render settings and environment,
layers, hidden/frozen state, unit settings (values are taken 1:1 as Veraa centimeters).
