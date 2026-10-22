// Keyframe animation: tracks with controllers (Bezier, Linear, TCB, Noise, Expression, List),
// out-of-range behaviour, a quaternion TCB rotation controller, constraints (Path, Look-At,
// Position, Orientation, Link, Attachment, Surface), additive layers, wired parameters and
// reaction mappings. The data lives in the scene as the "animation" scene module.
//
// Every animatable value is addressed by a path:
//   node:<id>/pos.x  pos.y  pos.z  rot.x  rot.y  rot.z (degrees)  scale.x  scale.y  scale.z
//   node:<id>/object/<param>[.r|.g|.b]       object parameters (colors per component)
//   node:<id>/mod:<index>/<param>[.r|.g|.b]  modifier parameters (index from the bottom)
//   node:<id>/c:<index>/percent | w<k>       constraint percent along path / target weights
//   node:<id>/custom/<name>                  custom attributes (Parameter Editor)
//   material:<id>/<param>[.r|.g|.b]          material parameters
//   render/<param>[.r|.g|.b]                 render settings
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "expression.h"
#include "scene.h"

namespace veraa {

enum class TangentType { Auto, Smooth, Linear, Step, Fast, Slow, Custom };
enum class OutOfRange { Constant, Cycle, Loop, PingPong, Linear, RelativeRepeat };
enum class ControllerType { Bezier, Linear, TCB, Noise, Expression, List };

const char* tangentTypeName(TangentType t);
const char* outOfRangeName(OutOfRange o);
const char* controllerTypeName(ControllerType c);

struct Key {
  double time = 0.0;   // frames
  double value = 0.0;
  TangentType inType = TangentType::Auto;
  TangentType outType = TangentType::Auto;
  double inSlope = 0.0, outSlope = 0.0;               // value per frame (used by Custom tangents)
  double inLength = 1.0 / 3.0, outLength = 1.0 / 3.0;  // handle length, fraction of the interval
  // TCB controller parameters (-1..1) and ease (0..50).
  double tension = 0.0, continuity = 0.0, bias = 0.0, easeTo = 0.0, easeFrom = 0.0;
  bool selected = false;
};

struct NoiseSettings {
  int seed = 0;
  double frequency = 0.5;   // cycles per 30 frames
  double strength = 50.0;
  bool fractal = true;
  double roughness = 0.0;   // 0..1, fractal detail
};

// Environment for evaluating a track: timing, and other tracks for expressions.
struct TrackEnv {
  double fps = 30.0;
  double startFrame = 0.0, endFrame = 100.0;
  std::function<double(const std::string& path, double frame)> trackValue;
};

class Track {
 public:
  ControllerType type = ControllerType::Bezier;
  std::vector<Key> keys;  // sorted by time
  OutOfRange before = OutOfRange::Constant;
  OutOfRange after = OutOfRange::Constant;
  NoiseSettings noise;
  // Expression controller: F (frame), T (ticks, 4800 per second), S (seconds), NT (0..1 over the
  // active range) plus bound variables.
  std::string expression;
  struct ExprVar {
    std::string name;
    std::string path;     // track path; empty = constant
    double constant = 0.0;
    double frameOffset = 0.0;
  };
  std::vector<ExprVar> exprVars;
  // List controller: weighted sum of sub-tracks (weights in percent).
  std::vector<Track> subs;
  std::vector<std::string> subNames;
  std::vector<double> weights;
  int active = 0;

  double evaluate(double frame, const TrackEnv* env = nullptr) const;
  // Slope (value per frame) by central difference.
  double derivative(double frame, const TrackEnv* env = nullptr) const;
  bool procedural() const { return type == ControllerType::Noise || type == ControllerType::Expression; }
  bool hasKeys() const;
  bool keyRange(double& t0, double& t1) const;
  int keyIndexAt(double frame, double eps = 1e-3) const;
  // Creates or replaces a key (in the active list entry for List controllers); returns its index.
  int setKey(double frame, double value);
  bool deleteKeyAt(double frame, double eps = 1e-3);
  void deleteKeysInRange(double t0, double t1);
  void sortKeys();
  void offsetValues(double delta);
  void keyTimes(std::vector<double>& out) const;
  // Effective in/out slopes of key i (automatic tangents resolved).
  void slopes(size_t i, double& in, double& out) const;
  void setController(ControllerType t);

  json toJson() const;
  static Track fromJson(const json& j);

 private:
  double evaluateKeys(double t) const;
  double evaluateInRange(double t, const TrackEnv* env) const;
  mutable std::shared_ptr<Expression> compiled_;
  mutable std::string compiledText_;
};

struct Quat {
  double x = 0, y = 0, z = 0, w = 1;
  static Quat fromEulerDeg(const Vec3& deg);
  static Quat fromMatrix(const Mat4& m);  // rotation part (columns normalized)
  static Quat fromAxisAngle(const Vec3& axis, double radians);
  Mat4 toMatrix() const;
  Vec3 toEulerDeg() const;
  Quat operator*(const Quat& o) const;
  Quat conjugate() const { return {-x, -y, -z, w}; }
  Quat normalized() const;
  double dot(const Quat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }
  static Quat slerp(const Quat& a, Quat b, double t);
  static Quat log(const Quat& q);  // unit quaternion -> pure quaternion
  static Quat exp(const Quat& q);  // pure quaternion -> unit quaternion
};

struct RotationKey {
  double time = 0.0;
  Quat q;
  double tension = 0.0, continuity = 0.0, bias = 0.0, easeTo = 0.0, easeFrom = 0.0;
  bool selected = false;
};

enum class ConstraintType { Path, LookAt, Position, Orientation, Link, Attachment, Surface };
const char* constraintTypeName(ConstraintType t);

struct ConstraintTarget {
  uint32_t nodeId = 0;
  double weight = 100.0;
  // Link constraint: from this frame on, the node follows this parent (0 = world).
  double frame = 0.0;
  Mat4 offset;  // Link: node world relative to the parent world at the link frame
  // Attachment: triangle and barycentric position on the target mesh.
  int face = 0;
  double baryU = 1.0 / 3.0, baryV = 1.0 / 3.0;
};

struct Constraint {
  ConstraintType type = ConstraintType::Position;
  std::vector<ConstraintTarget> targets;
  bool keepOffset = false;
  Vec3 positionOffset;
  Quat rotationOffset;
  // Path: percent along the path (animatable through node:<id>/c:<i>/percent).
  double percent = 0.0;
  bool follow = false, bank = false, loop = false, constantVelocity = true, flip = false;
  double bankAmount = 0.5, smoothness = 0.5;
  int axis = 0;  // Path follow / Look-At: 0 X, 1 Y, 2 Z
  // Look-At.
  uint32_t upNode = 0;  // 0 = world Z
  // Attachment / Surface alignment.
  bool align = true;
  // Surface: U/V position in percent.
  double u = 0.0, v = 0.0;
};

struct CustomAttribute {
  std::string name;
  int type = 0;  // 0 float, 1 integer, 2 boolean
  double value = 0.0, minValue = 0.0, maxValue = 100.0, defaultValue = 0.0;
};

struct NodeAnimation {
  int rotationController = 0;  // 0 Euler XYZ (rot.* tracks), 1 TCB quaternion (rotationKeys)
  std::vector<RotationKey> rotationKeys;
  std::vector<Constraint> constraints;
  std::vector<CustomAttribute> custom;
  bool trajectory = false;  // show the motion path in viewports
  bool empty() const {
    return rotationController == 0 && rotationKeys.empty() && constraints.empty() && custom.empty() && !trajectory;
  }
};

// Drives `to` from `from`: to = exprTo(x = from). Two-way wires also keep from = exprFrom(x = to)
// when `to` is edited.
struct Wire {
  std::string from, to;
  std::string exprTo = "x";
  std::string exprFrom = "x";
  bool twoWay = false;
};

// Reaction mapping (driven keys): `driven` follows `driver` through the (driver, driven) points.
struct Reaction {
  std::string driver, driven;
  std::vector<std::pair<double, double>> points;
  bool smooth = true;
};

struct AnimLayer {
  std::string name;
  double weight = 100.0;
  bool enabled = true;
  std::map<std::string, Track> tracks;
};

// Parsed form of a value path.
struct ValuePath {
  enum class Kind { Invalid, Transform, ObjectParam, ModifierParam, Constraint, Custom, MaterialParam, RenderParam };
  Kind kind = Kind::Invalid;
  uint32_t id = 0;     // node or material id
  int index = -1;      // modifier or constraint index
  int channel = -1;    // Transform: 0..8 (pos xyz, rot xyz, scale xyz)
  std::string key;     // parameter / constraint field / custom attribute name
  int component = -1;  // color component 0..2, -1 for scalars
  static ValuePath parse(const std::string& path);
};

class Animation : public SceneModule {
 public:
  static Animation* of(const Scene& scene);

  // ---- Data ----
  std::map<std::string, Track> tracks;  // base layer
  std::map<uint32_t, NodeAnimation> nodes;
  std::vector<AnimLayer> layers;  // additive layers above the base
  int activeLayer = 0;            // 0 = base, k = layers[k - 1]
  std::vector<Wire> wires;
  std::vector<Reaction> reactions;

  // ---- Paths and values ----
  static std::string nodePath(uint32_t id, const std::string& channel);
  static const std::vector<std::string>& transformChannels();
  // Reads/writes the current value stored in the scene. writeValue returns true if it changed.
  static bool readValue(const Scene& s, const std::string& path, double& v);
  static bool writeValue(Scene& s, const std::string& path, double v);
  // Animatable paths of a node: transform, object, modifier, constraint and custom attributes.
  std::vector<std::string> animatablePaths(const Scene& s, uint32_t nodeId) const;
  static std::string displayName(const Scene& s, const std::string& path);

  // ---- Tracks and keys ----
  Track* findTrack(const std::string& path);
  const Track* findTrack(const std::string& path) const;
  // Track in the active layer (created when missing).
  Track& activeTrack(const std::string& path);
  bool isAnimated(const std::string& path) const;
  bool nodeAnimated(uint32_t id) const;
  // Paths with tracks belonging to a node.
  std::vector<std::string> nodeTrackPaths(uint32_t id) const;
  // Keys `path` so its evaluated value at `frame` is `value`, in the active layer. When the track
  // is new and frame != startFrame, the original value (the current stored value when NaN) is
  // also keyed at startFrame, like Auto Key does.
  void setKey(const Scene& s, const std::string& path, double frame, double value, double startFrame,
              double originalValue = std::numeric_limits<double>::quiet_NaN());
  // Keys every transform channel of a node (Set Key / Auto Key for rotation-TCB nodes).
  void setRotationKey(uint32_t id, double frame, const Vec3& eulerDeg);
  bool deleteKey(const std::string& path, double frame);
  void deleteNodeKeysAt(uint32_t id, double frame);
  void deleteNodeAnimation(uint32_t id);
  void keyTimesForNodes(const std::vector<uint32_t>& ids, std::vector<double>& out) const;
  // Moves keys at `from` to `to` for the given nodes' tracks.
  void moveNodeKeys(const std::vector<uint32_t>& ids, double from, double to);
  // Removes tracks and node data of nodes/materials that no longer exist.
  void purge(const Scene& s);
  void copyNodeAnimation(uint32_t from, uint32_t to);

  // ---- Values over time ----
  TrackEnv env(const Scene& s) const;
  // Evaluated value of any path at a time (layers, wires, reactions; static value if not animated).
  double valueAt(const Scene& s, const std::string& path, double frame) const;

  // ---- Constraints ----
  // Adds a constraint, capturing offsets when keepOffset is set. Returns its index.
  int addConstraint(Scene& s, uint32_t nodeId, Constraint c);
  // Adds a Link constraint entry: from `frame` on, the node follows `parentId` (0 = world).
  void addLinkTarget(Scene& s, uint32_t nodeId, uint32_t parentId, double frame);

  // ---- Evaluation ----
  struct EvalResult {
    std::vector<uint32_t> transformChanged;  // node transforms written
    std::vector<uint32_t> geometryChanged;   // nodes re-evaluated (params, time-dependent, dependents)
    std::vector<uint32_t> otherChanged;      // light/camera/helper parameters written
    bool materialsChanged = false;
    bool renderSettingsChanged = false;
  };
  // Sets scene time and writes every animated value for that frame into the scene.
  EvalResult evaluate(Scene& scene, double frame);
  // Pivot world matrix of a node at an arbitrary time, without changing the scene.
  Mat4 worldMatrixAt(const Scene& scene, uint32_t nodeId, double frame) const;
  // Local transform from tracks only (no constraints or IK).
  Transform trackTransformAt(const Scene& scene, const Node& n, double frame) const;

  bool empty() const;
  json toJson() const override;
  void fromJson(const json& j) override;

  // Bumped on every structural change of the animation data (for UI caches).
  uint64_t revision() const { return revision_; }
  void touch() { ++revision_; }

 private:
  friend class AnimEvaluator;
  uint64_t revision_ = 1;
};

// Pivot world matrix of a node at a time (current matrix when the scene has no animation).
Mat4 nodeWorldMatrixAt(const Scene& scene, uint32_t nodeId, double frame);

// Builds the arc-length parameterized polyline of a shape node in world space at `frame`.
std::vector<Vec3> shapeWorldPolylineAt(const Scene& scene, uint32_t nodeId, double frame);
// Point and tangent at `percent` (0..100) along a polyline (arc length or vertex spacing).
void polylinePointAt(const std::vector<Vec3>& line, double percent, bool constantVelocity, Vec3& point,
                     Vec3& tangent);

}  // namespace veraa
