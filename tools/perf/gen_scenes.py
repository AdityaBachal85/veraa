#!/usr/bin/env python3
"""Generates large test scenes for Veraa's import/viewport benchmarks (docs/PERFORMANCE.md).

  python3 tools/perf/gen_scenes.py obj  out.obj  --objects 1    --tris 5000000
  python3 tools/perf/gen_scenes.py fbx  out.fbx  --objects 2000 --tris 20000000
  python3 tools/perf/gen_scenes.py glb  out.glb  --objects 5000 --tris 10000000

Every object is a wavy quad grid (two triangles per quad) with UVs, laid out on a square grid of
objects. FBX files are binary FBX 7.4 with zlib-compressed arrays (like 3ds Max writes), with
per-polygon smoothing, a material per object and IndexToDirect UVs. Needs numpy.
"""
import argparse
import math
import struct
import sys
import zlib

import numpy as np


def grid_object(quads_target, seed):
    """Returns (positions Nx3 float64, quads Qx4 int, uvs Nx2) of a k x k quad grid."""
    k = max(1, int(math.sqrt(quads_target)))
    xs = np.linspace(0.0, 100.0, k + 1)
    gx, gy = np.meshgrid(xs, xs, indexing="xy")
    phase = seed * 0.37
    gz = 6.0 * np.sin(gx * 0.11 + phase) * np.cos(gy * 0.07 - phase)
    pos = np.stack([gx.ravel(), gy.ravel(), gz.ravel()], axis=1)
    uv = np.stack([(gx / 100.0).ravel(), (gy / 100.0).ravel()], axis=1)
    i = np.arange(k)
    a = (i[None, :] + i[:, None] * (k + 1)).ravel()
    quads = np.stack([a, a + 1, a + k + 2, a + k + 1], axis=1)
    return pos, quads, uv


def layout(n, spacing=130.0):
    side = max(1, int(math.ceil(math.sqrt(n))))
    for o in range(n):
        yield (o % side) * spacing, (o // side) * spacing


# ---------------------------------------------------------------------------------------------
# OBJ


def write_obj(path, objects, tris):
    per = max(1, tris // objects // 2)
    with open(path, "w") as f:
        f.write("# Veraa benchmark scene\n")
        base = 1
        for o, (ox, oy) in enumerate(layout(objects)):
            pos, quads, uv = grid_object(per, o)
            pos = pos + np.array([ox, oy, 0.0])
            f.write(f"o Object{o:05d}\n")
            np.savetxt(f, pos, fmt="v %.4f %.4f %.4f")
            np.savetxt(f, uv, fmt="vt %.5f %.5f")
            q = quads + base
            fq = np.stack([q[:, 0], q[:, 0], q[:, 1], q[:, 1], q[:, 2], q[:, 2], q[:, 3], q[:, 3]], axis=1)
            np.savetxt(f, fq, fmt="f %d/%d %d/%d %d/%d %d/%d")
            base += len(pos)


# ---------------------------------------------------------------------------------------------
# Binary FBX 7.4


def _prop(v):
    if isinstance(v, bool):
        return b"C" + struct.pack("<B", 1 if v else 0)
    if isinstance(v, int):
        if -(2**31) <= v < 2**31:
            return b"I" + struct.pack("<i", v)
        return b"L" + struct.pack("<q", v)
    if isinstance(v, float):
        return b"D" + struct.pack("<d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return b"S" + struct.pack("<I", len(b)) + b
    if isinstance(v, bytes):
        return b"S" + struct.pack("<I", len(v)) + v
    if isinstance(v, np.ndarray):
        code = {np.dtype("float64"): b"d", np.dtype("float32"): b"f", np.dtype("int32"): b"i", np.dtype("int64"): b"l"}[v.dtype]
        raw = np.ascontiguousarray(v).tobytes()
        comp = zlib.compress(raw, 1)
        return code + struct.pack("<III", len(v), 1, len(comp)) + comp
    raise TypeError(type(v))


class FbxNode:
    def __init__(self, name, *props):
        self.name, self.props, self.children = name, props, []

    def add(self, name, *props):
        c = FbxNode(name, *props)
        self.children.append(c)
        return c

    def write(self, out, offset):
        props = b"".join(_prop(p) for p in self.props)
        name = self.name.encode()
        head_len = 13 + len(name)
        body = bytearray()
        cur = offset + head_len + len(props)
        for c in self.children:
            b = c.write(out, cur)
            body += b
            cur += len(b)
        if self.children or not self.props:
            body += b"\x00" * 13
        end = offset + head_len + len(props) + len(body)
        return struct.pack("<IIIB", end, len(self.props), len(props), len(name)) + name + props + bytes(body)


def p70(parent, entries):
    p = parent.add("Properties70")
    for e in entries:
        p.add("P", *e)


def write_fbx(path, objects, tris):
    per = max(1, tris // objects // 2)
    top = []
    hdr = FbxNode("FBXHeaderExtension")
    hdr.add("FBXHeaderVersion", 1003)
    hdr.add("FBXVersion", 7400)
    hdr.add("Creator", "Veraa gen_scenes.py")
    top.append(hdr)
    gs = FbxNode("GlobalSettings")
    gs.add("Version", 1000)
    p70(gs, [("UpAxis", "int", "Integer", "", 2), ("UpAxisSign", "int", "Integer", "", 1),
             ("FrontAxis", "int", "Integer", "", 1), ("FrontAxisSign", "int", "Integer", "", -1),
             ("CoordAxis", "int", "Integer", "", 0), ("CoordAxisSign", "int", "Integer", "", 1),
             ("UnitScaleFactor", "double", "Number", "", 1.0)])
    top.append(gs)
    objs = FbxNode("Objects")
    conns = FbxNode("Connections")
    uid = 1000
    for o, (ox, oy) in enumerate(layout(objects)):
        pos, quads, uv = grid_object(per, o)
        gid, mid, matid = uid + 1, uid + 2, uid + 3
        uid += 10
        name = f"Object{o:05d}"
        g = objs.add("Geometry", gid, (name + "\x00\x01Geometry").encode(), "Mesh")
        g.add("Vertices", pos.astype(np.float64).ravel())
        pvi = quads.astype(np.int32).copy()
        pvi[:, 3] = ~pvi[:, 3]
        g.add("PolygonVertexIndex", pvi.ravel())
        g.add("GeometryVersion", 124)
        sm = g.add("LayerElementSmoothing", 0)
        sm.add("Version", 102)
        sm.add("Name", "")
        sm.add("MappingInformationType", "ByPolygon")
        sm.add("ReferenceInformationType", "Direct")
        sm.add("Smoothing", np.ones(len(quads), dtype=np.int32))
        le = g.add("LayerElementUV", 0)
        le.add("Version", 101)
        le.add("Name", "UVChannel_1")
        le.add("MappingInformationType", "ByPolygonVertex")
        le.add("ReferenceInformationType", "IndexToDirect")
        le.add("UV", uv.astype(np.float64).ravel())
        le.add("UVIndex", quads.astype(np.int32).ravel())
        lm = g.add("LayerElementMaterial", 0)
        lm.add("Version", 101)
        lm.add("Name", "")
        lm.add("MappingInformationType", "AllSame")
        lm.add("ReferenceInformationType", "IndexToDirect")
        lm.add("Materials", np.zeros(1, dtype=np.int32))
        layer = g.add("Layer", 0)
        layer.add("Version", 100)
        for t in ("LayerElementSmoothing", "LayerElementUV", "LayerElementMaterial"):
            e = layer.add("LayerElement")
            e.add("Type", t)
            e.add("TypedIndex", 0)
        m = objs.add("Model", mid, (name + "\x00\x01Model").encode(), "Mesh")
        m.add("Version", 232)
        p70(m, [("Lcl Translation", "Lcl Translation", "", "A", float(ox), float(oy), 0.0)])
        mat = objs.add("Material", matid, (f"Mat{o:05d}" + "\x00\x01Material").encode(), "")
        mat.add("Version", 102)
        mat.add("ShadingModel", "phong")
        c = (o * 0.137) % 1.0
        p70(mat, [("DiffuseColor", "Color", "", "A", 0.3 + 0.6 * c, 0.5, 0.9 - 0.6 * c)])
        conns.add("C", "OO", gid, mid)
        conns.add("C", "OO", mid, 0)
        conns.add("C", "OO", matid, mid)
    top += [objs, conns]
    with open(path, "wb") as f:
        head = b"Kaydara FBX Binary  \x00\x1a\x00" + struct.pack("<I", 7400)
        f.write(head)
        cur = len(head)
        for n in top:
            b = n.write(None, cur)
            f.write(b)
            cur += len(b)
        f.write(b"\x00" * 13)


# ---------------------------------------------------------------------------------------------
# GLB


def write_glb(path, objects, tris):
    import json
    per = max(1, tris // objects // 2)
    blob = bytearray()
    views, accessors, meshes, nodes, materials = [], [], [], [], []

    def add_view(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    for o, (ox, oy) in enumerate(layout(objects)):
        pos, quads, uv = grid_object(per, o)
        # glTF is Y-up: (x, y, z) Z-up -> (x, z, -y)
        p = np.stack([pos[:, 0], pos[:, 2], -pos[:, 1]], axis=1).astype(np.float32) * 0.01
        t = np.stack([quads[:, 0], quads[:, 1], quads[:, 2], quads[:, 0], quads[:, 2], quads[:, 3]], axis=1)
        idx = t.astype(np.uint32).ravel()
        pv = add_view(p.tobytes(), 34962)
        accessors.append({"bufferView": pv, "componentType": 5126, "count": len(p), "type": "VEC3",
                          "min": p.min(axis=0).tolist(), "max": p.max(axis=0).tolist()})
        uvv = add_view(uv.astype(np.float32).tobytes(), 34962)
        accessors.append({"bufferView": uvv, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        iv = add_view(idx.tobytes(), 34963)
        accessors.append({"bufferView": iv, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
        c = (o * 0.137) % 1.0
        materials.append({"name": f"Mat{o:05d}", "pbrMetallicRoughness": {"baseColorFactor": [0.3 + 0.6 * c, 0.5, 0.9 - 0.6 * c, 1]}})
        meshes.append({"primitives": [{"attributes": {"POSITION": len(accessors) - 3, "TEXCOORD_0": len(accessors) - 2},
                                       "indices": len(accessors) - 1, "material": o}]})
        nodes.append({"name": f"Object{o:05d}", "mesh": o, "translation": [ox * 0.01, 0, -oy * 0.01]})
    doc = {"asset": {"version": "2.0", "generator": "Veraa gen_scenes.py"}, "scene": 0,
           "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": meshes,
           "materials": materials, "accessors": accessors, "bufferViews": views,
           "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(doc).encode()
    while len(js) % 4:
        js += b" "
    while len(blob) % 4:
        blob.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("format", choices=["obj", "fbx", "glb"])
    ap.add_argument("output")
    ap.add_argument("--objects", type=int, default=1)
    ap.add_argument("--tris", type=int, default=1000000)
    a = ap.parse_args()
    {"obj": write_obj, "fbx": write_fbx, "glb": write_glb}[a.format](a.output, a.objects, a.tris)
    print(f"wrote {a.output}")


if __name__ == "__main__":
    sys.exit(main())
