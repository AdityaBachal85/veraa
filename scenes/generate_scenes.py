#!/usr/bin/env python3
"""Generates the sample .veraa scenes in this folder.

Run from anywhere:  python3 scenes/generate_scenes.py
Units are centimeters, Z is up (like 3ds Max). Cameras and lights look down their local -Z.
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def norm(v):
    l = math.sqrt(sum(c * c for c in v))
    return [c / l for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def euler_look(frm, to):
    """Euler XYZ degrees that point local -Z from `frm` toward `to`, keeping local +Y up."""
    d = norm([to[i] - frm[i] for i in range(3)])
    right = cross(d, [0, 0, 1])
    if sum(c * c for c in right) < 1e-10:
        right = [1, 0, 0]
    right = norm(right)
    up = cross(right, d)
    back = [-c for c in d]
    # Columns of the rotation matrix are (right, up, back). R = Rz * Ry * Rx.
    r00, r10, r20 = right
    r21, r22 = up[2], back[2]
    y = math.asin(max(-1.0, min(1.0, -r20)))
    x = math.atan2(r21, r22)
    z = math.atan2(r10, r00)
    return [math.degrees(x), math.degrees(y), math.degrees(z)]


def texmap(mtype, maps=None, **params):
    m = {"type": mtype, "name": mtype, "params": params}
    if maps:
        m["maps"] = {k: {"map": v} for k, v in maps.items()}
    return m


def triplanar_bitmap(path, size):
    """A bitmap projected along the three world axes (no UVs needed)."""
    return texmap("Triplanar", maps={"texture": texmap("Bitmap", file=path)}, size=size, blend=0.15, space=1)


def write_marble_texture(path, n=1024, seed=7):
    """White marble with warm grey veins: domain-warped, tileable value noise (needs numpy, Pillow)."""
    import numpy as np
    from PIL import Image

    rng = np.random.default_rng(seed)

    def noise(freq):
        g = rng.random((freq, freq))
        x = np.arange(n) * freq / n
        i0 = np.floor(x).astype(int)
        t = x - i0
        t = t * t * (3 - 2 * t)
        i1 = (i0 + 1) % freq
        a = g[i0][:, i0] * (1 - t)[None, :] + g[i0][:, i1] * t[None, :]
        b = g[i1][:, i0] * (1 - t)[None, :] + g[i1][:, i1] * t[None, :]
        return a * (1 - t)[:, None] + b * t[:, None]

    def fbm(base, octaves):
        s, amp, tot = np.zeros((n, n)), 1.0, 0.0
        for o in range(octaves):
            s += amp * noise(base * 2 ** o)
            tot += amp
            amp *= 0.5
        return s / tot

    y, x = np.mgrid[0:n, 0:n] / n
    warp1, warp2, cloud = fbm(4, 6), fbm(8, 5), fbm(6, 6)
    v = np.abs(np.sin(2 * np.pi * (x + 2 * y) + 9.0 * warp1 + 3.0 * warp2))
    veins = np.exp(-v * 9.0) + 0.18 * np.exp(-v * 2.5)
    fine = np.exp(-np.abs(np.sin(2 * np.pi * (3 * x - y) + 14 * warp2)) * 22.0) * 0.45
    base = np.array([0.93, 0.915, 0.89]) * (0.94 + 0.08 * cloud)[..., None]
    w = np.clip(veins * (0.55 + 0.6 * warp2) + fine * 0.6, 0, 1)[..., None]
    c = np.clip(base * (1 - w) + np.array([0.52, 0.47, 0.40]) * w, 0, 1)
    srgb = np.where(c <= 0.0031308, 12.92 * c, 1.055 * c ** (1 / 2.4) - 0.055)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray((srgb * 255 + 0.5).astype(np.uint8)).save(path, quality=90)
    print("wrote", os.path.relpath(path, HERE))


class SceneBuilder:
    def __init__(self):
        self.nodes = []
        self.materials = []
        self.settings = {}
        self.names = {}

    def material(self, name, mtype="VeraaMtl", maps=None, **params):
        """maps: {param key: texmap dict (see texmap())}."""
        mid = len(self.materials) + 1
        m = {"id": mid, "name": name, "type": mtype, "params": params}
        if maps:
            m["maps"] = {k: {"map": v} for k, v in maps.items()}
        self.materials.append(m)
        return mid

    def node(self, otype, name=None, pos=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1), material=0,
             params=None, modifiers=None, wire=(0.6, 0.6, 0.6)):
        base = name or otype
        self.names[base] = self.names.get(base, 0) + 1
        nid = len(self.nodes) + 1
        full = name if name else "%s%03d" % (base, self.names[base])
        node = {
            "id": nid,
            "name": full,
            "transform": {"position": list(pos), "rotation": list(rot), "scale": list(scale)},
            "wireColor": list(wire),
            "hidden": False,
            "renderable": True,
            "material": material,
            "object": {"type": otype, "params": params or {}},
            "modifiers": [{"type": t, "enabled": True, "params": p} for t, p in (modifiers or [])],
        }
        self.nodes.append(node)
        return node

    def box(self, name, center_xy, z0, size, material, rot_z=0.0, otype="Box", extra=None):
        """Box from its footprint center and base height. size = (width X, length Y, height Z)."""
        p = {"width": size[0], "length": size[1], "height": size[2]}
        p.update(extra or {})
        return self.node(otype, name, pos=(center_xy[0], center_xy[1], z0), rot=(0, 0, rot_z),
                         material=material, params=p)

    def camera(self, name, pos, target, fov, **extra):
        p = {"fov": fov}
        p.update(extra)
        return self.node("Camera", name, pos=pos, rot=euler_look(pos, target), params=p, wire=(0.3, 0.6, 1.0))

    def light(self, otype, name, pos, target=None, rot=(0, 0, 0), **params):
        r = euler_look(pos, target) if target else rot
        return self.node(otype, name, pos=pos, rot=r, params=params, wire=(0.95, 0.85, 0.2))

    def save(self, filename):
        data = {
            "format": "veraa-scene",
            "version": 1,
            "renderSettings": self.settings,
            "materials": self.materials,
            "nodes": self.nodes,
            "nextNodeId": len(self.nodes) + 1,
            "nextMaterialId": len(self.materials) + 1,
        }
        with open(os.path.join(HERE, filename), "w") as f:
            json.dump(data, f, indent=1)
        print("wrote", filename)


def cornell_box():
    s = SceneBuilder()
    s.settings = {"width": 800, "height": 800, "samples": 256, "maxDepth": 12, "envMode": 0,
                  "envColor": [0, 0, 0], "camera": "Camera", "exposure": 0.6}
    white = s.material("White", diffuse=[0.73, 0.73, 0.73])
    red = s.material("Red", diffuse=[0.63, 0.065, 0.05])
    green = s.material("Green", diffuse=[0.14, 0.45, 0.091])
    s.box("Floor", (100, 100), -2, (204, 204, 2), white)
    s.box("Ceiling", (100, 100), 200, (204, 204, 2), white)
    s.box("BackWall", (100, 201), 0, (204, 2, 200), white)
    s.box("LeftWall", (-1, 100), 0, (2, 204, 200), red)
    s.box("RightWall", (201, 100), 0, (2, 204, 200), green)
    s.box("TallBox", (68, 125), 0, (60, 60, 120), white, rot_z=17)
    s.box("ShortBox", (135, 68), 0, (60, 60, 60), white, rot_z=-17)
    s.light("RectLight", "CeilingLight", (100, 100, 199.5), width=50, length=50, intensity=15,
            color=[1.0, 0.85, 0.65])
    s.camera("Camera", (100, -262, 100), (100, 100, 100), 42)
    s.save("cornell_box.veraa")


def living_room():
    s = SceneBuilder()
    s.settings = {"width": 1280, "height": 720, "samples": 128, "maxDepth": 8, "clampIndirect": 10,
                  "envMode": 3, "skyTurbidity": 3.0, "exposure": 1.4, "camera": "Camera",
                  "groundColor": [0.25, 0.27, 0.22]}
    wall = s.material("Wall paint", diffuse=[0.82, 0.8, 0.76], roughness=0.3)
    ceiling = s.material("Ceiling", diffuse=[0.9, 0.9, 0.9])
    floor = s.material("Oak floor", diffuse=[0.42, 0.26, 0.14], reflect=[0.6, 0.6, 0.6],
                       reflectGlossiness=0.78, fresnelIor=1.5)
    glass = s.material("Window glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1], ior=1.52,
                       fresnelIor=1.52, thinWalled=True, affectShadows=True)
    frame = s.material("Black metal", diffuse=[0.03, 0.03, 0.03], reflect=[0.5, 0.5, 0.5],
                       reflectGlossiness=0.7)
    fabric = s.material("Sofa fabric", diffuse=[0.26, 0.33, 0.42], roughness=0.8)
    cushion = s.material("Cushion fabric", diffuse=[0.62, 0.58, 0.5], roughness=0.8)
    accent = s.material("Mustard pillow", diffuse=[0.72, 0.5, 0.1], roughness=0.8)
    marble = s.material("White marble", diffuse=[0.86, 0.85, 0.83], reflect=[1, 1, 1],
                        reflectGlossiness=0.9, fresnelIor=1.55)
    chrome = s.material("Chrome", diffuse=[0.95, 0.95, 0.95], reflect=[1, 1, 1], metalness=1.0,
                        reflectGlossiness=0.96)
    brass = s.material("Brushed brass", diffuse=[0.95, 0.72, 0.38], reflect=[1, 1, 1], metalness=1.0,
                       reflectGlossiness=0.82)
    rug = s.material("Wool rug", diffuse=[0.55, 0.5, 0.44], roughness=1.0)
    lacquer = s.material("White lacquer", diffuse=[0.88, 0.88, 0.86], reflect=[1, 1, 1],
                         reflectGlossiness=0.85, fresnelIor=1.5)
    vase_glass = s.material("Green glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1],
                            ior=1.5, fresnelIor=1.5, fogColor=[0.45, 0.8, 0.55], fogMultiplier=1.0)
    terracotta = s.material("Terracotta", diffuse=[0.55, 0.27, 0.16], roughness=0.6)
    leaves = s.material("Leaves", diffuse=[0.09, 0.25, 0.06], reflect=[0.4, 0.4, 0.4],
                        reflectGlossiness=0.6)
    shade = s.material("Lamp shade", diffuse=[0.92, 0.86, 0.74], roughness=0.5)
    art = s.material("Canvas print", diffuse=[0.15, 0.32, 0.45])
    art2 = s.material("Canvas print 2", diffuse=[0.75, 0.42, 0.25])

    # Room shell: interior 600 (X) x 500 (Y) x 300 (Z). Window opening in the left (-X) wall.
    s.box("Floor", (300, 250), -10, (640, 540, 10), floor)
    s.box("Ceiling", (300, 250), 300, (640, 540, 10), ceiling)
    s.box("BackWall", (300, 510), 0, (640, 20, 300), wall)
    s.box("FrontWall", (300, -10), 0, (640, 20, 300), wall)
    s.box("RightWall", (610, 250), 0, (20, 540, 300), wall)
    s.box("LeftWallBottom", (-10, 250), 0, (20, 540, 60), wall)
    s.box("LeftWallTop", (-10, 250), 250, (20, 540, 50), wall)
    s.box("LeftWallA", (-10, 65), 60, (20, 170, 190), wall)
    s.box("LeftWallB", (-10, 460), 60, (20, 120, 190), wall)
    s.box("WindowGlass", (-10, 275), 60, (1, 250, 190), glass)
    s.box("WindowMullion", (-10, 275), 60, (8, 6, 190), frame)
    s.box("WindowSill", (-2, 275), 57, (24, 262, 3), lacquer)

    # Sofa along the back wall.
    s.box("SofaBase", (330, 448), 0, (230, 95, 42), fabric, otype="ChamferBox", extra={"fillet": 4, "filletSegs": 4})
    s.box("SofaBack", (330, 486), 30, (230, 26, 58), fabric, otype="ChamferBox", extra={"fillet": 8, "filletSegs": 5})
    s.box("SofaArmL", (205, 448), 0, (22, 95, 64), fabric, otype="ChamferBox", extra={"fillet": 7, "filletSegs": 5})
    s.box("SofaArmR", (455, 448), 0, (22, 95, 64), fabric, otype="ChamferBox", extra={"fillet": 7, "filletSegs": 5})
    s.box("SeatCushionL", (275, 440), 42, (104, 80, 16), cushion, otype="ChamferBox", extra={"fillet": 6, "filletSegs": 5})
    s.box("SeatCushionR", (385, 440), 42, (104, 80, 16), cushion, otype="ChamferBox", extra={"fillet": 6, "filletSegs": 5})
    pillow = s.box("Pillow", (240, 462), 58, (45, 14, 40), accent, rot_z=8, otype="ChamferBox",
                   extra={"fillet": 6, "filletSegs": 5, "heightSegs": 3, "widthSegs": 3})
    pillow["transform"]["rotation"] = [-12, 0, 8]

    # Coffee table: marble top on chrome legs, with a glass vase and a brass sphere.
    s.box("TableTop", (330, 315), 38, (140, 75, 4), marble, otype="ChamferBox", extra={"fillet": 1.5, "filletSegs": 3})
    for i, (x, y) in enumerate([(268, 285), (392, 285), (268, 345), (392, 345)]):
        s.node("Cylinder", "TableLeg%d" % (i + 1), pos=(x, y, 0), material=chrome,
               params={"radius": 1.6, "height": 38, "heightSegs": 1, "sides": 24})
    s.node("Tube", "Vase", pos=(300, 320, 42), material=vase_glass,
           params={"radius1": 7, "radius2": 6.4, "height": 26, "heightSegs": 8, "sides": 48},
           modifiers=[("Taper", {"amount": -0.25, "curve": 0.9})])
    s.node("Sphere", "BrassBall", pos=(360, 305, 42), material=brass, params={"radius": 6, "segments": 48, "basePivot": True})
    s.node("Sphere", "BrassBall2", pos=(375, 330, 42), material=brass, params={"radius": 4, "segments": 48, "basePivot": True})
    s.box("Book", (338, 333), 42, (24, 17, 3), art2, rot_z=-14)
    s.box("Rug", (330, 330), 0, (290, 210, 1), rug)

    # Cabinet on the right wall with decor.
    s.box("Cabinet", (578, 230), 0, (42, 170, 62), lacquer, otype="ChamferBox", extra={"fillet": 1, "filletSegs": 3})
    s.node("Cylinder", "CabinetBowl", pos=(578, 195, 62), material=brass,
           params={"radius": 14, "height": 6, "heightSegs": 1, "sides": 48},
           modifiers=[("Taper", {"amount": 0.4, "curve": 0.0})])
    s.box("Frame1", (330, 498), 120, (150, 3, 90), art)
    s.box("Frame2", (598, 230), 110, (3, 70, 90), art2)

    # Floor lamp in the back-right corner with a warm sphere light in its shade.
    s.node("Cylinder", "LampBase", pos=(555, 470, 0), material=chrome, params={"radius": 16, "height": 2, "sides": 48})
    s.node("Cylinder", "LampPole", pos=(555, 470, 2), material=chrome, params={"radius": 1.2, "height": 150, "sides": 16})
    s.node("Tube", "LampShade", pos=(555, 470, 140), material=shade,
           params={"radius1": 24, "radius2": 23.4, "height": 32, "heightSegs": 4, "sides": 64},
           modifiers=[("Taper", {"amount": -0.35, "curve": 0.0})])
    s.light("SphereLight", "LampBulb", (555, 470, 156), radius=4, intensity=60, color=[1.0, 0.78, 0.5])

    # Plant near the window.
    s.node("Cylinder", "PlantPot", pos=(60, 455, 0), material=terracotta,
           params={"radius": 20, "height": 42, "heightSegs": 1, "sides": 48},
           modifiers=[("Taper", {"amount": 0.25, "curve": 0.0})])
    s.node("GeoSphere", "PlantFoliage", pos=(60, 455, 82), scale=(1, 1, 1.3), material=leaves,
           params={"radius": 34, "segments": 8},
           modifiers=[("Noise", {"scale": 25, "strengthX": 10, "strengthY": 10, "strengthZ": 10, "fractal": True})])

    # Sun from the left, low enough to throw a patch of light across the floor.
    elev = math.radians(28)
    h = norm([-1.0, -0.55, 0.0])
    to_sun = [h[0] * math.cos(elev), h[1] * math.cos(elev), math.sin(elev)]
    s.light("SunLight", "Sun", [c * 2000 for c in to_sun], target=(0, 0, 0), intensity=3.0)

    s.camera("Camera", (520, 30, 135), (170, 380, 85), 68)
    s.save("living_room.veraa")


def product_shot():
    s = SceneBuilder()
    s.settings = {"width": 1280, "height": 720, "samples": 256, "maxDepth": 12, "envMode": 2,
                  "hdriPath": "../assets/hdri/royal_esplanade_1k.hdr", "hdriIntensity": 0.8,
                  "hdriRotation": 0.0, "envVisible": False, "backgroundColor": [0.04, 0.042, 0.048],
                  "exposure": 0.0, "camera": "Camera"}
    floor = s.material("Studio floor", diffuse=[0.08, 0.08, 0.09], reflect=[1, 1, 1],
                       reflectGlossiness=0.82, fresnelIor=1.5)
    plastic = s.material("White plastic", diffuse=[0.85, 0.85, 0.85], reflect=[1, 1, 1],
                         reflectGlossiness=0.88, fresnelIor=1.5)
    glass = s.material("Glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1], ior=1.5,
                       fresnelIor=1.5, fogColor=[0.82, 0.92, 1.0], fogMultiplier=0.6)
    gold = s.material("Gold", diffuse=[1.0, 0.77, 0.34], reflect=[1, 1, 1], metalness=1.0,
                      reflectGlossiness=0.93)
    chrome = s.material("Chrome", diffuse=[0.96, 0.96, 0.96], reflect=[1, 1, 1], metalness=1.0,
                        reflectGlossiness=1.0)
    red = s.material("Red lacquer", diffuse=[0.65, 0.03, 0.03], reflect=[1, 1, 1],
                     reflectGlossiness=0.9, fresnelIor=1.5)
    frosted = s.material("Frosted glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[0.95, 0.97, 1.0],
                         ior=1.5, fresnelIor=1.5, refractGlossiness=0.75, reflectGlossiness=0.75)

    s.node("Plane", "Floor", pos=(0, 0, 0), material=floor,
           params={"length": 4000, "width": 4000, "lengthSegs": 1, "widthSegs": 1})
    s.node("Cylinder", "Pedestal", pos=(0, 0, 0), material=plastic,
           params={"radius": 75, "height": 28, "heightSegs": 1, "sides": 96, "capSegs": 1})
    s.node("Sphere", "GlassSphere", pos=(-36, -8, 28), material=glass,
           params={"radius": 22, "segments": 96, "basePivot": True})
    s.node("Torus", "GoldRing", pos=(28, 18, 28 + 27), rot=(72, 0, -20), material=gold,
           params={"radius1": 21, "radius2": 6, "segments": 96, "sides": 48})
    s.node("GeoSphere", "ChromeBall", pos=(14, -40, 28 + 9), material=chrome, params={"radius": 9, "segments": 16})
    s.node("ChamferBox", "RedCube", pos=(52, -22, 28), rot=(0, 0, 28), material=red,
           params={"length": 26, "width": 26, "height": 26, "fillet": 3, "filletSegs": 5})
    s.node("Cylinder", "FrostedVase", pos=(-12, 38, 28), material=frosted,
           params={"radius": 10, "height": 44, "heightSegs": 12, "sides": 64},
           modifiers=[("Taper", {"amount": -0.4, "curve": 1.2}), ("Twist", {"angle": 0.0})])

    s.light("RectLight", "KeyLight", (-170, -140, 190), target=(0, 0, 40), width=120, length=80, intensity=7,
            invisible=True)
    s.light("RectLight", "RimLight", (150, 170, 130), target=(0, 0, 40), width=60, length=140, intensity=6,
            invisible=True)
    s.camera("Camera", (12, -345, 150), (4, 0, 32), 40)
    s.save("product_shot.veraa")


def kitchen():
    """Archviz kitchen: glossy lacquer run along the back wall, marble counter, backsplash and floor,
    fluted-glass display units with lit shelves, LED strips, IES downlights and a frosted glass door."""
    marble_path = os.path.join(HERE, "..", "assets", "textures", "marble_white.jpg")
    if not os.path.exists(marble_path):
        write_marble_texture(marble_path)
    marble_map = "../assets/textures/marble_white.jpg"
    s = SceneBuilder()
    s.settings = {"width": 1160, "height": 1000, "samples": 1024, "maxDepth": 10, "clampIndirect": 8,
                  "envMode": 0, "envColor": [0, 0, 0], "camera": "Camera", "denoise": True,
                  "exposureControl": 1, "ev100": 8.2, "whiteBalanceK": 4700,
                  # "Interior (archviz)" look: soft highlight roll-off, gentle contrast.
                  "tonemap": 1, "cmBurn": 0.35, "cmBrightMultiplier": 1.0, "cmDarkMultiplier": 1.0,
                  "contrast": 1.05, "saturation": 1.0}
    gloss = dict(reflect=[1, 1, 1], fresnelIor=1.5)
    wall = s.material("Wall paint", diffuse=[0.85, 0.82, 0.77], roughness=0.4)
    ceiling = s.material("Ceiling", diffuse=[0.86, 0.86, 0.85])
    floor = s.material("Marble floor", diffuse=[0.9, 0.9, 0.9], reflectGlossiness=0.95,
                       maps={"diffuse": triplanar_bitmap(marble_map, 260)}, **gloss)
    stone = s.material("Marble counter", diffuse=[0.9, 0.9, 0.9], reflectGlossiness=0.95,
                       maps={"diffuse": triplanar_bitmap(marble_map, 90)}, **gloss)
    lacquer = s.material("Beige lacquer", diffuse=[0.72, 0.66, 0.57], reflect=[1, 1, 1], fresnelIor=1.55,
                         reflectGlossiness=0.96)
    white_lacquer = s.material("White lacquer", diffuse=[0.84, 0.83, 0.8], reflectGlossiness=0.94, **gloss)
    black = s.material("Black metal", diffuse=[0.025, 0.025, 0.025], reflect=[0.6, 0.6, 0.6],
                       reflectGlossiness=0.7)
    plinth = s.material("Plinth", diffuse=[0.08, 0.08, 0.08], reflect=[0.3, 0.3, 0.3], reflectGlossiness=0.6)
    light_oak = s.material("Light oak", diffuse=[0.72, 0.56, 0.38], roughness=0.3)
    oak = s.material("Oak veneer", diffuse=[0.55, 0.38, 0.22], reflect=[0.4, 0.4, 0.4], reflectGlossiness=0.6)
    fluted = s.material("Fluted glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[0.97, 0.96, 0.93],
                        ior=1.5, fresnelIor=1.5, refractGlossiness=0.97, fogColor=[0.9, 0.92, 0.88],
                        fogMultiplier=0.2)
    frosted = s.material("Frosted glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[0.95, 0.95, 0.95],
                         ior=1.5, fresnelIor=1.5, refractGlossiness=0.55, reflectGlossiness=0.9)
    black_glass = s.material("Black glass", diffuse=[0.01, 0.01, 0.01], reflectGlossiness=0.98, **gloss)
    steel = s.material("Stainless steel", diffuse=[0.75, 0.75, 0.74], reflect=[1, 1, 1], metalness=1.0,
                       reflectGlossiness=0.8, anisotropy=0.4)
    chrome = s.material("Chrome", diffuse=[0.95, 0.95, 0.95], reflect=[1, 1, 1], metalness=1.0,
                        reflectGlossiness=0.97)
    ceramic = s.material("Ceramic", diffuse=[0.88, 0.87, 0.84], reflectGlossiness=0.96, **gloss)
    bottle = s.material("Bottle glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1], ior=1.5,
                        fresnelIor=1.5, fogColor=[0.35, 0.55, 0.3], fogMultiplier=1.0)
    jar = s.material("Jar glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1], ior=1.5,
                     fresnelIor=1.5)
    fruit = s.material("Lemons", diffuse=[0.75, 0.55, 0.05], reflect=[0.5, 0.5, 0.5], reflectGlossiness=0.6)
    greens = s.material("Herbs", diffuse=[0.08, 0.22, 0.05], roughness=0.6)
    # Seen by the camera only: the IES lights do the lighting (no fireflies from tiny emitters).
    fixture = s.material("Downlight lens", diffuse=[0, 0, 0], selfIllum=[1.0, 0.86, 0.68], selfIllumMultiplier=30000,
                         selfIllumGI=False)

    # Room shell: inside x -210..210, y -220..300, z 0..285.
    s.box("Floor", (0, 40), -6, (440, 540, 6), floor)
    s.box("CeilingSlab", (0, 40), 285, (440, 540, 8), ceiling)
    s.box("BackWall", (0, 305), 0, (440, 10, 285), wall)
    s.box("FrontWall", (0, -225), 0, (440, 10, 285), wall)
    s.box("LeftWall", (-215, 40), 0, (10, 540, 285), wall)
    # Right wall with a tall frosted glass door (opening y 40..220, up to z 230).
    s.box("RightWallA", (215, -90), 0, (10, 260, 285), wall)
    s.box("RightWallB", (215, 260), 0, (10, 80, 285), wall)
    s.box("RightWallTop", (215, 130), 230, (10, 180, 55), wall)
    s.box("DoorGlass", (213, 130), 0, (2, 180, 230), frosted)
    for name, (cy, w) in {"DoorFrameL": (41, 3), "DoorFrameR": (219, 3), "DoorFrameMid": (130, 3)}.items():
        s.box(name, (211, cy), 0, (4, w, 230), black)
    s.box("DoorFrameTop", (211, 130), 227, (4, 180, 3), black)
    s.light("VeraaLight", "DaylightBehindDoor", (240, 130, 115), target=(0, 130, 115), lightType=0,
            width=180, length=230, units=2, intensity=900, colorMode=1, temperature=6000, invisible=False)

    # ---- Base cabinets (x -200..200) ----
    s.box("BaseCarcass", (0, 272), 10, (400, 56, 76), lacquer)
    s.box("Plinth", (0, 278), 0, (400, 44, 10), plinth)
    cols = [(-200, -133), (-133, -66), (-66, 0), (0, 66), (66, 133), (133, 200)]
    for i, (x0, x1) in enumerate(cols):
        cx, w = (x0 + x1) / 2, (x1 - x0) - 0.5
        rows = [(10, 36), (37, 61), (62, 83)] if i % 3 != 1 else [(10, 83)]
        for j, (z0, z1) in enumerate(rows):
            s.box("Front%d_%d" % (i, j), (cx, 242.5), z0 + 0.25, (w, 3, z1 - z0 - 0.5), lacquer, otype="ChamferBox",
                  extra={"fillet": 0.3, "filletSegs": 2})
    # Black handle grooves (gola profiles) under the counter and between drawers.
    for z in (83.0, 61.0, 36.0):
        s.box("Gola%d" % int(z), (0, 243.4), z, (400, 1.2, 1.0 if z < 80 else 3.0), black)
    s.box("Counter", (0, 268), 86, (404, 64, 4), stone, otype="ChamferBox", extra={"fillet": 0.4, "filletSegs": 2})
    s.box("Backsplash", (0, 299), 90, (400, 2, 60), stone)

    # ---- Wall cabinets (z 150..245) ----
    s.box("Bulkhead", (0, 282), 245, (420, 36, 40), white_lacquer)
    # Middle: white lacquer doors over a black glass hood.
    for i, (x0, x1) in enumerate([(-110, -40), (-40, 40), (40, 110)]):
        z0 = 178 if i == 1 else 150
        s.box("WallDoor%d" % i, ((x0 + x1) / 2, 266), z0 + 0.3, (x1 - x0 - 0.5, 3, 245 - z0 - 0.6), white_lacquer,
              otype="ChamferBox", extra={"fillet": 0.3, "filletSegs": 2})
    s.box("WallCarcassMid", (0, 283), 150, (220, 33, 95), white_lacquer)
    s.box("Hood", (0, 270), 150, (78, 30, 28), black_glass, otype="ChamferBox", extra={"fillet": 0.5, "filletSegs": 2})

    # Display units with fluted glass doors, black frames and lit shelves.
    for side, (x0, x1) in (("L", (-200, -110)), ("R", (110, 200))):
        cx, w = (x0 + x1) / 2, x1 - x0
        s.box("DispBack" + side, (cx, 298), 150, (w, 4, 95), light_oak)
        s.box("DispBottom" + side, (cx, 283), 150, (w, 34, 2), black)
        s.box("DispTop" + side, (cx, 283), 243, (w, 34, 2), black)
        for k, x in enumerate((x0 + 1, x1 - 1)):
            s.box("DispSide%s%d" % (side, k), (x, 283), 150, (2, 34, 95), black)
        for k, z in enumerate((181, 212)):
            s.box("Shelf%s%d" % (side, k), (cx, 283), z, (w - 4, 30, 1.2), oak)
        for k, z in enumerate((180.6, 211.6, 242.6)):
            s.light("VeraaLight", "ShelfLed%s%d" % (side, k), (cx, 272, z), lightType=0, width=w - 8, length=1.5,
                    units=2, intensity=30000, colorMode=1, temperature=2700, invisible=True)
        # Props on the shelves.
        for k, z in enumerate((152, 182.2, 213.2)):
            for m in range(3):
                px = x0 + 15 + m * (w - 30) / 2
                if (m + k) % 2 == 0:
                    s.node("Cylinder", "Jar%s%d%d" % (side, k, m), pos=(px, 285, z), material=jar,
                           params={"radius": 5, "height": 16, "sides": 32})
                else:
                    s.node("Cylinder", "Plate%s%d%d" % (side, k, m), pos=(px, 292, z), rot=(80, 0, 0),
                           material=ceramic, params={"radius": 11, "height": 1.2, "sides": 48})
        # Fluted glass fronts (two doors of vertical ribs) in black frames.
        for d in range(2):
            dx0 = x0 + d * w / 2
            s.box("DoorFrame%s%dL" % (side, d), (dx0 + 1.2, 265), 150, (2.4, 3, 95), black)
            s.box("DoorFrame%s%dR" % (side, d), (dx0 + w / 2 - 1.2, 265), 150, (2.4, 3, 95), black)
            s.box("DoorFrame%s%dB" % (side, d), (dx0 + w / 4, 265), 150, (w / 2, 3, 2.4), black)
            s.box("DoorFrame%s%dT" % (side, d), (dx0 + w / 4, 265), 242.6, (w / 2, 3, 2.4), black)
            ribs = int((w / 2 - 4.8) / 2.0)
            for r in range(ribs):
                rx = dx0 + 2.4 + 1.0 + r * 2.0
                s.node("Cylinder", "Rib%s%d_%d" % (side, d, r), pos=(rx, 265, 152.4), material=fluted,
                       params={"radius": 1.05, "height": 90.2, "sides": 12, "heightSegs": 1})

    # Light pelmet: a lip under the wall cabinets that hides the LED strip from low viewpoints.
    s.box("LightPelmet", (0, 265.5), 146.5, (400, 2, 3.5), white_lacquer)
    # LED strips: under the wall cabinets (lighting the backsplash) and in the plinth recess.
    s.light("VeraaLight", "UnderCabinetLed", (0, 290, 149.8), lightType=0, width=396, length=2, units=2,
            intensity=7000, colorMode=1, temperature=3000, invisible=False)
    s.light("VeraaLight", "PlinthLed", (0, 251, 9.5), lightType=0, width=396, length=1.5, units=2,
            intensity=9000, colorMode=1, temperature=3000, invisible=True)

    # ---- Downlights: IES distribution plus a glowing lens ----
    for i, (x, y) in enumerate([(-120, 215), (0, 215), (120, 215), (-90, 40), (90, 40)]):
        s.light("VeraaIES", "Downlight%d" % (i + 1), (x, y, 284), iesFile="../assets/ies/downlight_wide.ies",
                powerMode=1, power=750, colorMode=1, temperature=3000, shapeRadius=3)
        s.node("Cylinder", "DownlightLens%d" % (i + 1), pos=(x, y, 284.4), material=fixture,
               params={"radius": 3.5, "height": 0.6, "sides": 32})

    # ---- Props on the counter ----
    s.box("Hob", (-15, 268), 90, (60, 50, 0.6), black_glass)
    s.box("Microwave", (165, 278), 90, (48, 36, 28), black_glass, otype="ChamferBox",
          extra={"fillet": 0.8, "filletSegs": 2})
    s.box("Sink", (95, 268), 89.8, (54, 42, 0.5), steel)
    s.node("Cylinder", "FaucetBody", pos=(95, 292, 90), material=chrome, params={"radius": 1.6, "height": 30, "sides": 24})
    s.node("Cylinder", "FaucetSpout", pos=(95, 292, 119), rot=(90, 0, 0), material=chrome,
           params={"radius": 1.1, "height": 16, "sides": 24})
    s.node("Cylinder", "Bowl", pos=(-120, 262, 90), material=ceramic,
           params={"radius": 13, "height": 7, "sides": 64}, modifiers=[("Taper", {"amount": 0.5, "curve": 0.6})])
    for i, (x, y) in enumerate([(-124, 259), (-114, 265), (-120, 270)]):
        s.node("Sphere", "Lemon%d" % i, pos=(x, y, 95), material=fruit, params={"radius": 4, "segments": 32})
    for i, x in enumerate((-178, -170, -162)):
        s.node("Cylinder", "Bottle%d" % i, pos=(x, 288, 90), material=bottle,
               params={"radius": 3.2, "height": 26 + 4 * i, "sides": 32})
    s.node("Cylinder", "HerbPot", pos=(55, 288, 90), material=ceramic, params={"radius": 6, "height": 10, "sides": 48})
    s.node("GeoSphere", "Herbs", pos=(55, 288, 104), scale=(1, 1, 0.8), material=greens,
           params={"radius": 8, "segments": 6},
           modifiers=[("Noise", {"scale": 4, "strengthX": 2, "strengthY": 2, "strengthZ": 2, "fractal": True})])
    s.box("Board", (30, 286), 90, (30, 2, 40), oak, rot_z=0)

    s.camera("Camera", (0, -95, 125), (0, 300, 125), 74)
    s.save("kitchen.veraa")


def exterior_daylight():
    """A modern house on a lawn under the physical sky and sun (photometric units, EV 14.7)."""
    s = SceneBuilder()
    s.settings = {"width": 1280, "height": 720, "samples": 256, "maxDepth": 8, "clampIndirect": 8,
                  "envMode": 3, "camera": "Camera", "denoise": True,
                  # "Exterior daylight" look.
                  "exposureControl": 1, "ev100": 14.7, "whiteBalanceK": 6000, "tonemap": 8,
                  "contrast": 1.08, "saturation": 1.05, "skyModel": 1}
    render = s.material("White render", diffuse=[0.78, 0.77, 0.74], roughness=0.6)
    wood = s.material("Larch cladding", diffuse=[0.42, 0.27, 0.15], reflect=[0.4, 0.4, 0.4],
                      reflectGlossiness=0.55,
                      maps={"diffuse": texmap("Noise", color1=[0.36, 0.22, 0.12], color2=[0.5, 0.33, 0.19],
                                              noiseType=1, size=6.0, tilingZ=0.08)})
    # Glazing seen from outside in daylight: dark, reflecting the sky.
    glass = s.material("Glazing", diffuse=[0.015, 0.017, 0.018], reflect=[1, 1, 1], fresnelIor=1.52,
                       reflectGlossiness=0.995)
    clear = s.material("Railing glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[0.92, 0.95, 0.94],
                       ior=1.52, fresnelIor=1.52, thinWalled=True)
    frame = s.material("Anthracite aluminium", diffuse=[0.08, 0.085, 0.09], reflect=[1, 1, 1], metalness=1.0,
                       metalFresnel=5, reflectGlossiness=0.6)
    interior = s.material("Interior", diffuse=[0.35, 0.33, 0.3])
    lawn = s.material("Lawn", diffuse=[0.1, 0.17, 0.05], roughness=1.0,
                      maps={"diffuse": texmap("Noise", color1=[0.07, 0.13, 0.03], color2=[0.15, 0.22, 0.07],
                                              noiseType=1, size=40.0)})
    concrete = s.material("Concrete paving", diffuse=[0.5, 0.49, 0.46], roughness=0.8)
    foliage = s.material("Foliage", diffuse=[0.07, 0.16, 0.04], reflect=[0.3, 0.3, 0.3], reflectGlossiness=0.5,
                         translucency=1, translucencyColor=[0.5, 0.8, 0.3])
    bark = s.material("Bark", diffuse=[0.2, 0.15, 0.11], roughness=1.0)
    steel = s.material("Brushed steel", diffuse=[0.9, 0.9, 0.9], reflect=[1, 1, 1], metalness=1.0,
                       metalFresnel=7, reflectGlossiness=0.72, anisotropy=0.5)

    s.node("Plane", "Lawn", material=lawn, params={"length": 300000, "width": 300000})
    s.box("Terrace", (0, -320), 0, (1600, 260, 12), concrete)
    s.box("Path", (-200, -900), 0, (180, 1100, 4), concrete)
    # Two volumes: a white ground floor and a timber-clad upper floor cantilevering over the terrace.
    s.box("GroundFloor", (0, 0), 0, (1400, 800, 300), render)
    s.box("UpperFloor", (150, -80), 300, (1100, 900, 290), wood)
    s.box("Roof", (150, -80), 590, (1140, 940, 18), render)
    s.box("Interior", (0, 0), 1, (1380, 780, 298), interior)
    # Glazing: ground floor window wall, upper floor ribbon window (glass in front of dark interior).
    s.box("GlassGround", (-150, -401), 15, (900, 2, 270), glass)
    s.box("GlassUpper", (150, -531), 360, (800, 2, 170), glass)
    for i, x in enumerate([-600, -375, -150, 75, 300]):
        s.box("Mullion%d" % i, (x, -402), 12, (8, 6, 276), frame)
    s.box("WindowHead", (-150, -402), 285, (910, 6, 8), frame)
    for i, x in enumerate([-250, 150, 550]):
        s.box("UpperMullion%d" % i, (x, -532), 355, (8, 6, 180), frame)
    s.box("Railing", (550, -440), 12, (500, 2, 100), clear)
    s.node("Cylinder", "Downpipe", pos=(-690, -395, 0), material=steel, params={"radius": 5, "height": 300, "sides": 24})
    # Trees: trunks with fractal foliage.
    for i, (x, y, r) in enumerate([(-1250, -500, 230), (1150, -800, 190), (-1700, 600, 300), (1700, 300, 260)]):
        s.node("Cylinder", "Trunk%d" % i, pos=(x, y, 0), material=bark,
               params={"radius": r * 0.07, "height": r * 1.6, "sides": 16})
        s.node("GeoSphere", "Crown%d" % i, pos=(x, y, r * 1.9), scale=(1, 1, 1.2), material=foliage,
               params={"radius": r, "segments": 10},
               modifiers=[("Noise", {"scale": r * 0.5, "strengthX": r * 0.3, "strengthY": r * 0.3,
                                     "strengthZ": r * 0.3, "fractal": True})])
    # Late morning sun from the front left.
    e = math.radians(38.0)
    az = math.radians(-35.0)
    to_sun = [math.sin(az) * math.cos(e), -math.cos(az) * math.cos(e), math.sin(e)]
    s.light("VeraaSun", "Sun", [c * 10000 for c in to_sun], target=(0, 0, 0), turbidity=2.2, skyModel=1,
            groundAlbedo=[0.15, 0.18, 0.1])
    s.camera("Camera", (650, -1750, 160), (-120, 0, 290), 64)
    s.save("exterior_daylight.veraa")


def studio_product():
    """Studio product shot: softboxes over a seamless backdrop, measured metals, thin film, sheen."""
    s = SceneBuilder()
    s.settings = {"width": 1280, "height": 720, "samples": 256, "maxDepth": 12, "clampIndirect": 8,
                  "envMode": 0, "envColor": [0.0, 0.0, 0.0], "camera": "Camera", "denoise": True,
                  # "Product studio" look.
                  "tonemap": 0, "exposure": 0.7, "contrast": 1.0, "saturation": 1.0}
    backdrop = s.material("Backdrop paper", diffuse=[0.42, 0.43, 0.45], roughness=0.5)
    plinth = s.material("Satin plinth", diffuse=[0.08, 0.08, 0.085], reflect=[1, 1, 1], fresnelIor=1.5,
                        reflectGlossiness=0.75)
    gold = s.material("Gold", diffuse=[0.9, 0.9, 0.9], reflect=[1, 1, 1], metalness=1.0, metalFresnel=2,
                      reflectGlossiness=0.93)
    silver = s.material("Silver", diffuse=[0.9, 0.9, 0.9], reflect=[1, 1, 1], metalness=1.0, metalFresnel=3,
                        reflectGlossiness=0.97)
    copper = s.material("Copper", diffuse=[0.9, 0.9, 0.9], reflect=[1, 1, 1], metalness=1.0, metalFresnel=4,
                        reflectGlossiness=0.8)
    titanium = s.material("Anodized titanium", diffuse=[0.9, 0.9, 0.9], reflect=[1, 1, 1], metalness=1.0,
                          metalFresnel=8, reflectGlossiness=0.9, thinFilm=True, thinFilmThickness=280,
                          thinFilmIor=2.3)
    glass = s.material("Perfume glass", diffuse=[0, 0, 0], reflect=[1, 1, 1], refract=[1, 1, 1], ior=1.5,
                       fresnelIor=1.5, fogColor=[0.95, 0.82, 0.6], fogMultiplier=0.06)
    velvet = s.material("Velvet", diffuse=[0.12, 0.02, 0.05], sheenColor=[0.9, 0.45, 0.6], sheenGlossiness=0.55)
    pearl = s.material("Pearl", diffuse=[0.85, 0.82, 0.8], coatAmount=1.0, coatGlossiness=0.97, thinFilm=True,
                       thinFilmLayer=1, thinFilmThickness=520, thinFilmIor=1.6)

    # Backdrop floor that falls off into darkness behind the products.
    s.node("Plane", "BackdropFloor", pos=(0, 0, 0), material=backdrop, params={"length": 3000, "width": 3000})
    s.node("Cylinder", "Plinth", pos=(0, 60, 0), material=plinth,
           params={"radius": 70, "height": 24, "sides": 96, "capSegs": 1})
    s.box("VelvetCloth", (-20, 95), 24, (110, 60, 1.5), velvet, rot_z=-8, otype="ChamferBox",
          extra={"fillet": 0.6, "filletSegs": 3})
    s.node("Torus", "GoldRing", pos=(-28, 50, 24 + 1.8), material=gold,
           params={"radius1": 12, "radius2": 1.8, "segments": 96, "sides": 32})
    s.node("Sphere", "SilverBall", pos=(22, 30, 24), material=silver, params={"radius": 9, "segments": 64,
                                                                              "basePivot": True})
    s.node("Cylinder", "TitaniumCap", pos=(-2, 85, 25.5), material=titanium,
           params={"radius": 7, "height": 9, "sides": 64})
    s.node("Cylinder", "Bottle", pos=(-2, 85, 34.5), material=glass,
           params={"radius": 11, "height": 30, "sides": 96}, modifiers=[("Taper", {"amount": -0.15, "curve": 0.3})])
    s.node("Sphere", "Pearl", pos=(36, 70, 25.5), material=pearl, params={"radius": 4, "segments": 48,
                                                                          "basePivot": True})
    s.node("Cylinder", "CopperCup", pos=(45, 100, 24), material=copper,
           params={"radius": 9, "height": 14, "sides": 64}, modifiers=[("Taper", {"amount": 0.25, "curve": 0.0})])
    # Softboxes: a large key above left, a strip rim behind right and a weak fill.
    s.light("VeraaLight", "KeySoftbox", (-170, -60, 230), target=(0, 70, 30), lightType=0, width=120, length=90,
            intensity=24, invisible=True)
    s.light("VeraaLight", "RimStrip", (190, 230, 120), target=(0, 70, 35), lightType=0, width=30, length=160,
            intensity=30, invisible=True)
    s.light("VeraaLight", "Fill", (150, -200, 90), target=(0, 70, 30), lightType=0, width=100, length=100,
            intensity=3.0, invisible=True)
    s.camera("Camera", (25, -165, 85), (5, 70, 32), 36)
    s.save("studio_product.veraa")


if __name__ == "__main__":
    cornell_box()
    living_room()
    product_shot()
    kitchen()
    exterior_daylight()
    studio_product()
