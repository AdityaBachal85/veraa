// Doors (Pivot, Sliding, BiFold) and Windows (Awning, Casement, Fixed, Pivoted, Projected,
// Sliding). Local axes: X along the width, Y through the wall (depth), Z up; the object's pivot is
// at the bottom-left-front corner of the frame.
#include <algorithm>
#include <cmath>

#include "aec.h"
#include "meshbuild.h"
#include "shape_mesh.h"

namespace veraa {

namespace {

constexpr float kUv = 0.01f;

// Door material IDs.
constexpr int kFront = 0, kBack = 1, kPanel = 2, kFrame = 3, kEdge = 4;
// Window material IDs.
constexpr int kWinFrame = 0, kWinRail = 1, kWinGlass = 2;

// Box with per-side materials: front (-Y), back (+Y), the four others `edge`.
void partBox(PolyMesh& m, const Mat4& xf, const Vec3& lo, const Vec3& hi, int front, int back, int edge) {
  if (hi.x - lo.x <= 1e-6f || hi.y - lo.y <= 1e-6f || hi.z - lo.z <= 1e-6f) return;
  MeshBuilder b(m, kUv);
  size_t first = m.faces.size();
  b.box(xf, lo, hi, edge);
  // MeshBuilder::box face order: -Z, +Z, -Y, +Y, -X, +X.
  if (m.faces.size() == first + 6) {
    m.faces[first + 2].matId = front;
    m.faces[first + 3].matId = back;
  }
}

void solidBox(PolyMesh& m, const Mat4& xf, const Vec3& lo, const Vec3& hi, int mat) { partBox(m, xf, lo, hi, mat, mat, mat); }

// Bevelled raised panel of w x h (local x, z) and thickness t centered on y = 0.
void raisedPanel(PolyMesh& m, const Mat4& xf, float x0, float z0, float w, float h, float t, float bevel) {
  if (w <= 0 || h <= 0) return;
  bevel = std::min(bevel, std::min(w, h) * 0.3f);
  Contour2D c{{Vec2(0, 0), Vec2(w, 0), Vec2(w, h), Vec2(0, h)}, true};
  float bd = t * 0.3f;
  std::vector<ExtrudeLevel> lv = {{-t * 0.5f, -bevel}, {-t * 0.5f + bd, 0}, {t * 0.5f - bd, 0}, {t * 0.5f, -bevel}};
  ExtrudeOptions o;
  o.generateUVs = false;
  o.capStartMatId = o.capEndMatId = o.sideMatId = kPanel;
  PolyMesh p = extrudeContours({c}, lv, o);
  // Extrusion runs along local Z; turn it so the panel's thickness is along Y and its height Z.
  Mat4 toLeaf;
  toLeaf.m[0][0] = 1;
  toLeaf.m[1][1] = 0;
  toLeaf.m[1][2] = -1;
  toLeaf.m[2][1] = 1;
  toLeaf.m[2][2] = 0;
  p.transform(xf * Mat4::translate(Vec3(x0, 0, z0)) * toLeaf);
  boxMapUvs(p, kUv);
  m.append(p);
}

struct LeafStyle {
  int panels = 0;  // 0 none, 1 glass, 2 beveled
  int countH = 1, countV = 1;
  float stile = 10, topRail = 10, bottomRail = 20, muntin = 4, glass = 0.5f, bevel = 3;
};

// Door leaf in its own frame: x 0..w (hinge at 0), z 0..h, thickness t centered on y = 0.
void doorLeaf(PolyMesh& m, const Mat4& xf, float w, float h, float t, const LeafStyle& s) {
  if (w <= 0 || h <= 0) return;
  float ht = t * 0.5f;
  if (s.panels == 0 || w <= 2 * s.stile + 1 || h <= s.topRail + s.bottomRail + 1) {
    partBox(m, xf, Vec3(0, -ht, 0), Vec3(w, ht, h), kFront, kBack, kEdge);
    return;
  }
  // Stiles and rails.
  partBox(m, xf, Vec3(0, -ht, 0), Vec3(s.stile, ht, h), kFront, kBack, kEdge);
  partBox(m, xf, Vec3(w - s.stile, -ht, 0), Vec3(w, ht, h), kFront, kBack, kEdge);
  partBox(m, xf, Vec3(s.stile, -ht, 0), Vec3(w - s.stile, ht, s.bottomRail), kFront, kBack, kEdge);
  partBox(m, xf, Vec3(s.stile, -ht, h - s.topRail), Vec3(w - s.stile, ht, h), kFront, kBack, kEdge);
  // Panels separated by muntins.
  float ix0 = s.stile, ix1 = w - s.stile, iz0 = s.bottomRail, iz1 = h - s.topRail;
  int nh = std::max(1, s.countH), nv = std::max(1, s.countV);
  float pw = (ix1 - ix0 - (nh - 1) * s.muntin) / nh, ph = (iz1 - iz0 - (nv - 1) * s.muntin) / nv;
  if (pw <= 0 || ph <= 0) {
    partBox(m, xf, Vec3(ix0, -ht * 0.5f, iz0), Vec3(ix1, ht * 0.5f, iz1), kPanel, kPanel, kPanel);
    return;
  }
  for (int i = 1; i < nh; ++i) {
    float x = ix0 + i * pw + (i - 1) * s.muntin;
    partBox(m, xf, Vec3(x, -ht, iz0), Vec3(x + s.muntin, ht, iz1), kFront, kBack, kEdge);
  }
  for (int j = 1; j < nv; ++j) {
    float z = iz0 + j * ph + (j - 1) * s.muntin;
    for (int i = 0; i < nh; ++i) {
      float x = ix0 + i * (pw + s.muntin);
      partBox(m, xf, Vec3(x, -ht, z), Vec3(x + pw, ht, z + s.muntin), kFront, kBack, kEdge);
    }
  }
  for (int i = 0; i < nh; ++i)
    for (int j = 0; j < nv; ++j) {
      float x = ix0 + i * (pw + s.muntin), z = iz0 + j * (ph + s.muntin);
      if (s.panels == 1) {
        float g = std::min(s.glass, t) * 0.5f;
        solidBox(m, xf, Vec3(x, -g, z), Vec3(x + pw, g, z + ph), kPanel);
      } else {
        raisedPanel(m, xf, x, z, pw, ph, t * 0.6f, s.bevel);
      }
    }
}

// ---- Doors ----

class DoorObject : public GeometryObject {
 public:
  enum Kind { Pivot, Sliding, BiFold };
  explicit DoorObject(Kind k) : kind_(k) {
    params.beginGroup("Parameters");
    params.addFloat("height", "Height", 210, 1.0, 1e7, 1.0);
    params.addFloat("width", "Width", 90, 1.0, 1e7, 1.0);
    params.addFloat("depth", "Depth", 20, 0.1, 1e7, 1.0);
    if (k == Pivot) {
      params.addBool("doubleDoors", "Double Doors", false);
      params.addBool("flipSwing", "Flip Swing", false);
      params.addBool("flipHinge", "Flip Hinge", false);
      params.addFloat("open", "Open (degrees)", 0, 0.0, 180.0, 1.0, 1);
    } else if (k == Sliding) {
      params.addBool("flipFrontBack", "Flip Front Back", false);
      params.addBool("flipSide", "Flip Side", false);
      params.addFloat("open", "Open %", 0, 0.0, 100.0, 1.0, 1);
    } else {
      params.addBool("doubleDoors", "Double Doors", false);
      params.addBool("flipSwing", "Flip Swing", false);
      params.addBool("flipHinge", "Flip Hinge", false);
      params.addFloat("open", "Open %", 0, 0.0, 100.0, 1.0, 1);
    }
    params.beginGroup("Frame");
    params.addBool("createFrame", "Create Frame", true);
    params.addFloat("frameWidth", "Width", 5, 0.0, 1e7, 0.5);
    params.addFloat("frameDepth", "Depth", 2, 0.0, 1e7, 0.5);
    params.addFloat("doorOffset", "Door Offset", 0, -1e7, 1e7, 0.5);
    params.beginGroup("Leaf Parameters");
    params.addFloat("thickness", "Thickness", 4, 0.1, 1e7, 0.5);
    params.addFloat("stile", "Stiles/Top Rail", 10, 0.0, 1e7, 0.5);
    params.addFloat("bottomRail", "Bottom Rail", 20, 0.0, 1e7, 0.5);
    params.addInt("panelsH", "# Panels Horiz", 1, 1, 50);
    params.addInt("panelsV", "# Panels Vert", 2, 1, 50);
    params.addFloat("muntin", "Muntin", 4, 0.0, 1e7, 0.5);
    params.addEnum("panels", "Panels", {"None", "Glass", "Beveled"}, 2);
    params.addFloat("glass", "Glass Thickness", 0.5, 0.01, 1e7, 0.1);
    params.addFloat("bevel", "Bevel Width", 3, 0.0, 1e7, 0.5);
  }
  std::string type() const override {
    return kind_ == Pivot ? "PivotDoor" : kind_ == Sliding ? "SlidingDoor" : "BiFoldDoor";
  }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<DoorObject>(*this); }
  PolyMesh build() const override;

 private:
  Kind kind_;
};

PolyMesh DoorObject::build() const {
  PolyMesh m;
  float H = params.getFloat("height"), W = params.getFloat("width"), D = params.getFloat("depth");
  bool frame = params.getBool("createFrame");
  float fw = frame ? std::min(params.getFloat("frameWidth"), W * 0.25f) : 0.0f;
  float fd = params.getFloat("frameDepth");
  if (frame && fw > 0) {
    // Jambs and header span the wall depth plus the casing depth on both faces.
    Mat4 I;
    solidBox(m, I, Vec3(0, -fd, 0), Vec3(fw, D + fd, H), kFrame);
    solidBox(m, I, Vec3(W - fw, -fd, 0), Vec3(W, D + fd, H), kFrame);
    solidBox(m, I, Vec3(fw, -fd, H - fw), Vec3(W - fw, D + fd, H), kFrame);
  }
  float x0 = fw, x1 = W - fw, oh = H - fw;  // opening
  float t = std::min(params.getFloat("thickness"), D);
  float yc = D * 0.5f + params.getFloat("doorOffset");
  LeafStyle s;
  s.panels = params.getInt("panels");
  s.countH = params.getInt("panelsH");
  s.countV = params.getInt("panelsV");
  s.stile = s.topRail = params.getFloat("stile");
  s.bottomRail = params.getFloat("bottomRail");
  s.muntin = params.getFloat("muntin");
  s.glass = params.getFloat("glass");
  s.bevel = params.getFloat("bevel");
  float ow = x1 - x0;
  if (ow <= 0 || oh <= 0) return m;
  auto leafAt = [&](const Vec3& hinge, float angleDeg, float w, bool mirrored) {
    Mat4 xf = Mat4::translate(hinge) * Mat4::rotateZ(radians(angleDeg));
    if (mirrored) xf = xf * Mat4::scale(Vec3(-1, 1, 1));
    doorLeaf(m, xf, w, oh, t, s);
  };
  float open = params.getFloat("open");
  if (kind_ == Pivot) {
    float sw = params.getBool("flipSwing") ? 1.0f : -1.0f;  // default swings toward the front (-Y)
    if (params.getBool("doubleDoors")) {
      leafAt(Vec3(x0, yc, 0), sw * open, ow * 0.5f, false);
      leafAt(Vec3(x1, yc, 0), -sw * open, ow * 0.5f, true);
    } else if (params.getBool("flipHinge")) {
      leafAt(Vec3(x1, yc, 0), -sw * open, ow, true);
    } else {
      leafAt(Vec3(x0, yc, 0), sw * open, ow, false);
    }
  } else if (kind_ == Sliding) {
    // Two leaves on parallel tracks, overlapping a little; one slides over the other.
    float lw = ow * 0.5f + t;
    float f = params.getFloat("open") / 100.0f;
    float front = params.getBool("flipFrontBack") ? 1.0f : -1.0f;
    bool flip = params.getBool("flipSide");
    float fixedX = flip ? x0 : x1 - lw;
    float slideX0 = flip ? x1 - lw : x0;
    float slideX = flip ? slideX0 - f * (ow - lw) : slideX0 + f * (ow - lw);
    leafAt(Vec3(fixedX, yc - front * t * 0.55f, 0), 0, lw, false);
    leafAt(Vec3(slideX, yc + front * t * 0.55f, 0), 0, lw, false);
  } else {
    // Bi-fold: each side folds in two panels; the far panel's edge runs along the track.
    float a = 90.0f * open / 100.0f;
    float sw = params.getBool("flipSwing") ? 1.0f : -1.0f;
    auto foldPair = [&](float hingeX, float width, bool fromRight) {
      float w = width * 0.5f;
      float dir = fromRight ? -1.0f : 1.0f;
      float ar = radians(a);
      Vec3 h(hingeX, yc, 0);
      Vec3 e1 = h + Vec3(dir * w * std::cos(ar), sw * w * std::sin(ar), 0);
      leafAt(h, fromRight ? 180.0f - sw * a : sw * a, w, false);
      leafAt(e1, fromRight ? 180.0f + sw * a : -sw * a, w, false);
    };
    if (params.getBool("doubleDoors")) {
      foldPair(x0, ow * 0.5f, false);
      foldPair(x1, ow * 0.5f, true);
    } else if (params.getBool("flipHinge")) {
      foldPair(x1, ow, true);
    } else {
      foldPair(x0, ow, false);
    }
  }
  return m;
}

// ---- Windows ----

// Sash: rails around glass panes (x 0..w, z 0..h, thickness t centered on y = 0).
void sash(PolyMesh& m, const Mat4& xf, float w, float h, float rail, float t, float glass, int nh = 1, int nv = 1,
          float muntin = 0) {
  if (w <= 0 || h <= 0) return;
  float ht = t * 0.5f;
  rail = std::min(rail, std::min(w, h) * 0.45f);
  solidBox(m, xf, Vec3(0, -ht, 0), Vec3(rail, ht, h), kWinRail);
  solidBox(m, xf, Vec3(w - rail, -ht, 0), Vec3(w, ht, h), kWinRail);
  solidBox(m, xf, Vec3(rail, -ht, 0), Vec3(w - rail, ht, rail), kWinRail);
  solidBox(m, xf, Vec3(rail, -ht, h - rail), Vec3(w - rail, ht, h), kWinRail);
  float ix0 = rail, ix1 = w - rail, iz0 = rail, iz1 = h - rail;
  nh = std::max(1, nh);
  nv = std::max(1, nv);
  float pw = (ix1 - ix0 - (nh - 1) * muntin) / nh, ph = (iz1 - iz0 - (nv - 1) * muntin) / nv;
  if (pw <= 0 || ph <= 0) return;
  for (int i = 1; i < nh; ++i) {
    float x = ix0 + i * pw + (i - 1) * muntin;
    solidBox(m, xf, Vec3(x, -ht, iz0), Vec3(x + muntin, ht, iz1), kWinRail);
  }
  for (int j = 1; j < nv; ++j) {
    float z = iz0 + j * ph + (j - 1) * muntin;
    for (int i = 0; i < nh; ++i) {
      float x = ix0 + i * (pw + muntin);
      solidBox(m, xf, Vec3(x, -ht, z), Vec3(x + pw, ht, z + muntin), kWinRail);
    }
  }
  float g = std::min(glass, t) * 0.5f;
  for (int i = 0; i < nh; ++i)
    for (int j = 0; j < nv; ++j) {
      float x = ix0 + i * (pw + muntin), z = iz0 + j * (ph + muntin);
      solidBox(m, xf, Vec3(x, -g, z), Vec3(x + pw, g, z + ph), kWinGlass);
    }
}

class WindowObject : public GeometryObject {
 public:
  enum Kind { Awning, Casement, Fixed, Pivoted, Projected, Sliding };
  explicit WindowObject(Kind k) : kind_(k) {
    params.beginGroup("Parameters");
    params.addFloat("height", "Height", 120, 1.0, 1e7, 1.0);
    params.addFloat("width", "Width", 100, 1.0, 1e7, 1.0);
    params.addFloat("depth", "Depth", 15, 0.1, 1e7, 1.0);
    params.beginGroup("Frame");
    params.addFloat("frameH", "Horiz. Width", 5, 0.0, 1e7, 0.5);
    params.addFloat("frameV", "Vert. Width", 5, 0.0, 1e7, 0.5);
    params.addFloat("frameThickness", "Thickness", 8, 0.1, 1e7, 0.5);
    params.beginGroup("Glazing");
    params.addFloat("glass", "Thickness", 0.5, 0.01, 1e7, 0.1);
    params.beginGroup("Rails and Panels");
    params.addFloat("rail", "Rail Width", 5, 0.0, 1e7, 0.5);
    params.addFloat("sashThickness", "Sash Thickness", 4, 0.1, 1e7, 0.5);
    switch (k) {
      case Casement: params.addInt("casements", "Casements", 1, 1, 2); break;
      case Fixed:
        params.addInt("panelsH", "# Panels Horiz", 1, 1, 50);
        params.addInt("panelsV", "# Panels Vert", 1, 1, 50);
        params.addFloat("muntin", "Muntin Width", 3, 0.0, 1e7, 0.5);
        break;
      case Pivoted: params.addBool("verticalRotation", "Vertical Rotation", false); break;
      case Projected:
        params.addFloat("middleHeight", "Middle Height", 40, 0.0, 1e7, 1.0);
        params.addFloat("bottomHeight", "Bottom Height", 30, 0.0, 1e7, 1.0);
        break;
      case Sliding:
        params.addBool("hung", "Hung", false);
        params.addInt("panelsH", "# Panels Horiz", 1, 1, 50);
        params.addInt("panelsV", "# Panels Vert", 1, 1, 50);
        params.addFloat("muntin", "Muntin Width", 3, 0.0, 1e7, 0.5);
        break;
      default: break;
    }
    if (k != Fixed) {
      params.beginGroup("Open Window");
      params.addFloat("open", "Open %", 0, 0.0, 100.0, 1.0, 1);
      if (k == Casement) params.addBool("flipSwing", "Flip Swing", false);
    }
  }
  std::string type() const override {
    static const char* names[] = {"AwningWindow", "CasementWindow", "FixedWindow",
                                  "PivotedWindow", "ProjectedWindow", "SlidingWindow"};
    return names[kind_];
  }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<WindowObject>(*this); }
  PolyMesh build() const override;

 private:
  Kind kind_;
};

PolyMesh WindowObject::build() const {
  PolyMesh m;
  float H = params.getFloat("height"), W = params.getFloat("width"), D = params.getFloat("depth");
  float fh = std::min(params.getFloat("frameH"), H * 0.3f), fv = std::min(params.getFloat("frameV"), W * 0.3f);
  float ft = std::min(params.getFloat("frameThickness"), D);
  float fy0 = (D - ft) * 0.5f, fy1 = fy0 + ft;
  Mat4 I;
  solidBox(m, I, Vec3(0, fy0, 0), Vec3(fv, fy1, H), kWinFrame);
  solidBox(m, I, Vec3(W - fv, fy0, 0), Vec3(W, fy1, H), kWinFrame);
  solidBox(m, I, Vec3(fv, fy0, 0), Vec3(W - fv, fy1, fh), kWinFrame);
  solidBox(m, I, Vec3(fv, fy0, H - fh), Vec3(W - fv, fy1, H), kWinFrame);
  float x0 = fv, z0 = fh, ow = W - 2 * fv, oh = H - 2 * fh;
  if (ow <= 0 || oh <= 0) return m;
  float rail = params.getFloat("rail"), st = std::min(params.getFloat("sashThickness"), D), glass = params.getFloat("glass");
  float yc = D * 0.5f;
  float open = params.has("open") ? params.getFloat("open") / 100.0f : 0.0f;
  switch (kind_) {
    case Fixed:
      sash(m, Mat4::translate(Vec3(x0, yc, z0)), ow, oh, rail, st, glass, params.getInt("panelsH"),
           params.getInt("panelsV"), params.getFloat("muntin"));
      break;
    case Awning: {
      // Hinged along the top, the bottom swings out (toward -Y).
      float a = radians(90.0f * open);
      Mat4 xf = Mat4::translate(Vec3(x0, yc, z0 + oh)) * Mat4::rotateX(-a) * Mat4::translate(Vec3(0, 0, -oh));
      sash(m, xf, ow, oh, rail, st, glass);
      break;
    }
    case Casement: {
      float a = 90.0f * open * (params.getBool("flipSwing") ? 1.0f : -1.0f);
      if (params.getInt("casements") == 2) {
        sash(m, Mat4::translate(Vec3(x0, yc, z0)) * Mat4::rotateZ(radians(a)), ow * 0.5f, oh, rail, st, glass);
        sash(m, Mat4::translate(Vec3(x0 + ow, yc, z0)) * Mat4::rotateZ(radians(-a)) * Mat4::scale(Vec3(-1, 1, 1)),
             ow * 0.5f, oh, rail, st, glass);
      } else {
        sash(m, Mat4::translate(Vec3(x0, yc, z0)) * Mat4::rotateZ(radians(a)), ow, oh, rail, st, glass);
      }
      break;
    }
    case Pivoted: {
      float a = radians(90.0f * open);
      Mat4 xf;
      if (params.getBool("verticalRotation"))
        xf = Mat4::translate(Vec3(x0 + ow * 0.5f, yc, z0)) * Mat4::rotateZ(a) * Mat4::translate(Vec3(-ow * 0.5f, 0, 0));
      else
        xf = Mat4::translate(Vec3(x0, yc, z0 + oh * 0.5f)) * Mat4::rotateX(a) * Mat4::translate(Vec3(0, 0, -oh * 0.5f));
      sash(m, xf, ow, oh, rail, st, glass);
      break;
    }
    case Projected: {
      // Top pane fixed; the middle and bottom panes project out from their top edges.
      float mh = std::min(params.getFloat("middleHeight"), oh * 0.45f);
      float bh = std::min(params.getFloat("bottomHeight"), oh * 0.45f);
      float th = oh - mh - bh;
      float a = radians(60.0f * open);
      sash(m, Mat4::translate(Vec3(x0, yc, z0 + bh + mh)), ow, th, rail, st, glass);
      sash(m, Mat4::translate(Vec3(x0, yc, z0 + bh + mh)) * Mat4::rotateX(-a) * Mat4::translate(Vec3(0, 0, -mh)), ow, mh,
           rail, st, glass);
      sash(m, Mat4::translate(Vec3(x0, yc, z0 + bh)) * Mat4::rotateX(-a) * Mat4::translate(Vec3(0, 0, -bh)), ow, bh, rail,
           st, glass);
      break;
    }
    case Sliding: {
      int nh = params.getInt("panelsH"), nv = params.getInt("panelsV");
      float mu = params.getFloat("muntin");
      if (params.getBool("hung")) {
        // Vertical sliding: the lower sash rises in front of the upper one.
        float sh = oh * 0.5f + rail * 0.5f;
        sash(m, Mat4::translate(Vec3(x0, yc + st * 0.55f, z0 + oh - sh)), ow, sh, rail, st, glass, nh, nv, mu);
        sash(m, Mat4::translate(Vec3(x0, yc - st * 0.55f, z0 + open * (oh - sh))), ow, sh, rail, st, glass, nh, nv, mu);
      } else {
        float sw = ow * 0.5f + rail * 0.5f;
        sash(m, Mat4::translate(Vec3(x0 + ow - sw, yc + st * 0.55f, z0)), sw, oh, rail, st, glass, nh, nv, mu);
        sash(m, Mat4::translate(Vec3(x0 + open * (ow - sw), yc - st * 0.55f, z0)), sw, oh, rail, st, glass, nh, nv, mu);
      }
      break;
    }
  }
  return m;
}

}  // namespace

void registerDoorWindowTypes() {
  const char* geo = "Geometry";
  registerObjectType({"PivotDoor", "Pivot", geo, "Doors", [] { return std::make_unique<DoorObject>(DoorObject::Pivot); }});
  registerObjectType(
      {"SlidingDoor", "Sliding", geo, "Doors", [] { return std::make_unique<DoorObject>(DoorObject::Sliding); }});
  registerObjectType({"BiFoldDoor", "BiFold", geo, "Doors", [] { return std::make_unique<DoorObject>(DoorObject::BiFold); }});
  const char* win = "Windows";
  auto w = [&](const char* type, const char* name, WindowObject::Kind k) {
    registerObjectType({type, name, geo, win, [k] { return std::make_unique<WindowObject>(k); }});
  };
  w("AwningWindow", "Awning", WindowObject::Awning);
  w("CasementWindow", "Casement", WindowObject::Casement);
  w("FixedWindow", "Fixed", WindowObject::Fixed);
  w("PivotedWindow", "Pivoted", WindowObject::Pivoted);
  w("ProjectedWindow", "Projected", WindowObject::Projected);
  w("SlidingWindow", "Sliding", WindowObject::Sliding);
}

}  // namespace veraa
