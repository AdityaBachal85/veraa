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


class SceneBuilder:
    def __init__(self):
        self.nodes = []
        self.materials = []
        self.settings = {}
        self.names = {}

    def material(self, name, **params):
        mid = len(self.materials) + 1
        self.materials.append({"id": mid, "name": name, "type": "VeraaMtl", "params": params})
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


if __name__ == "__main__":
    cornell_box()
    living_room()
    product_shot()
