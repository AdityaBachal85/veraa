// Animation tests: controllers, out-of-range types, expressions, constraints, IK, skin, particles,
// rigid bodies and the AVI writer.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <vector>

#include "animation.h"
#include "avi.h"
#include "dynamics.h"
#include "expression.h"
#include "ik.h"
#include "particles.h"
#include "rigging.h"
#include "scene.h"

using namespace veraa;

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)
#define CHECK_NEAR(a, b, eps)                                                                            \
  do {                                                                                                   \
    double va_ = (a), vb_ = (b);                                                                         \
    if (!(std::fabs(va_ - vb_) <= (eps))) {                                                              \
      std::printf("FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, va_, vb_);                \
      ++g_failures;                                                                                      \
    }                                                                                                    \
  } while (0)

static Track twoKeys(TangentType t) {
  Track tr;
  int a = tr.setKey(0, 0);
  int b = tr.setKey(10, 10);
  tr.keys[static_cast<size_t>(a)].inType = tr.keys[static_cast<size_t>(a)].outType = t;
  tr.keys[static_cast<size_t>(b)].inType = tr.keys[static_cast<size_t>(b)].outType = t;
  return tr;
}

static void testBezier() {
  Track lin = twoKeys(TangentType::Linear);
  CHECK_NEAR(lin.evaluate(5), 5.0, 1e-6);
  CHECK_NEAR(lin.evaluate(2.5), 2.5, 1e-6);
  CHECK_NEAR(lin.evaluate(10), 10.0, 1e-9);
  CHECK_NEAR(lin.evaluate(0), 0.0, 1e-9);

  Track step = twoKeys(TangentType::Step);
  CHECK_NEAR(step.evaluate(9.9), 0.0, 1e-9);
  CHECK_NEAR(step.evaluate(10), 10.0, 1e-9);

  // Auto: symmetric, passes through keys, eases at the ends.
  Track autoT = twoKeys(TangentType::Auto);
  CHECK_NEAR(autoT.evaluate(5), 5.0, 1e-4);
  CHECK(autoT.evaluate(1) < 1.0);
  // Auto tangents do not overshoot at a peak.
  Track peak;
  peak.setKey(0, 0);
  peak.setKey(10, 10);
  peak.setKey(20, 0);
  double maxV = 0;
  for (double t = 0; t <= 20; t += 0.25) maxV = std::max(maxV, peak.evaluate(t));
  CHECK(maxV <= 10.0 + 1e-6);
  CHECK_NEAR(peak.derivative(10), 0.0, 1e-3);
  // Smooth (Catmull-Rom) passes through every key.
  Track sm;
  for (int i = 0; i < 5; ++i) sm.setKey(i * 10, (i % 2) * 7.0);
  for (auto& k : sm.keys) k.inType = k.outType = TangentType::Smooth;
  for (int i = 0; i < 5; ++i) CHECK_NEAR(sm.evaluate(i * 10), (i % 2) * 7.0, 1e-6);
  // Custom tangents.
  Track cu = twoKeys(TangentType::Custom);
  cu.keys[0].outSlope = 3.0;
  cu.keys[1].inSlope = 3.0;
  CHECK(cu.evaluate(1) > lin.evaluate(1));
  // Fast/Slow.
  Track slow = twoKeys(TangentType::Slow);
  CHECK(slow.evaluate(1) < 1.0);
  Track fast = twoKeys(TangentType::Fast);
  CHECK(fast.evaluate(1) > slow.evaluate(1));
}

static void testTcb() {
  Track t;
  t.setController(ControllerType::TCB);
  t.setKey(0, 0);
  t.setKey(10, 10);
  t.setKey(20, 0);
  t.setKey(30, 10);
  CHECK_NEAR(t.evaluate(0), 0.0, 1e-6);
  CHECK_NEAR(t.evaluate(10), 10.0, 1e-6);
  CHECK_NEAR(t.evaluate(20), 0.0, 1e-6);
  // Symmetric segment between two equal-slope neighbours.
  CHECK_NEAR(t.evaluate(15), 5.0, 1e-3);
  // Tension 1 at the peak flattens it (lower values next to it than with tension 0).
  double before = t.evaluate(8);
  for (auto& k : t.keys) k.tension = 1.0;
  CHECK(t.evaluate(8) < before + 1e-9);
  // Ease to/from slows the start of a segment.
  Track e;
  e.setController(ControllerType::TCB);
  e.setKey(0, 0);
  e.setKey(10, 10);
  double plain = e.evaluate(2);
  e.keys[0].easeFrom = 50;
  CHECK(e.evaluate(2) < plain);
  // Linear controller.
  Track l;
  l.setController(ControllerType::Linear);
  l.setKey(0, 0);
  l.setKey(10, 20);
  CHECK_NEAR(l.evaluate(2.5), 5.0, 1e-9);
}

static void testOutOfRange() {
  Track t = twoKeys(TangentType::Linear);
  t.after = OutOfRange::Constant;
  CHECK_NEAR(t.evaluate(13), 10.0, 1e-9);
  t.after = OutOfRange::Cycle;
  CHECK_NEAR(t.evaluate(13), 3.0, 1e-6);
  t.after = OutOfRange::Loop;
  CHECK_NEAR(t.evaluate(13), 3.0, 1e-6);
  t.after = OutOfRange::PingPong;
  CHECK_NEAR(t.evaluate(13), 7.0, 1e-6);
  CHECK_NEAR(t.evaluate(23), 3.0, 1e-6);
  t.after = OutOfRange::Linear;
  CHECK_NEAR(t.evaluate(15), 15.0, 1e-6);
  t.after = OutOfRange::RelativeRepeat;
  CHECK_NEAR(t.evaluate(15), 15.0, 1e-6);
  CHECK_NEAR(t.evaluate(25), 25.0, 1e-6);
  t.before = OutOfRange::Cycle;
  CHECK_NEAR(t.evaluate(-3), 7.0, 1e-6);
  t.before = OutOfRange::Linear;
  CHECK_NEAR(t.evaluate(-4), -4.0, 1e-6);
}

static void testExpressions() {
  auto eval = [](const char* text, double x = 0.0) {
    Expression e;
    std::string err;
    if (!e.parse(text, &err)) return std::nan("");
    return e.evaluate([&](const std::string& n) { return n == "x" ? x : 0.0; });
  };
  CHECK_NEAR(eval("2+3*4"), 14.0, 1e-12);
  CHECK_NEAR(eval("(2+3)*4"), 20.0, 1e-12);
  CHECK_NEAR(eval("2^3^2"), 512.0, 1e-9);
  CHECK_NEAR(eval("-2^2"), -4.0, 1e-12);
  CHECK_NEAR(eval("sin(pi/2)"), 1.0, 1e-12);
  CHECK_NEAR(eval("max(1, 5, 3)"), 5.0, 1e-12);
  CHECK_NEAR(eval("clamp(x, 0, 1)", 5.0), 1.0, 1e-12);
  CHECK_NEAR(eval("x > 1 ? 10 : 20", 2.0), 10.0, 1e-12);
  CHECK_NEAR(eval("x > 1 && x < 3", 2.0), 1.0, 1e-12);
  CHECK_NEAR(eval("7 % 3"), 1.0, 1e-12);
  CHECK_NEAR(eval("1/0"), 0.0, 1e-12);  // non-finite results become 0
  Expression bad;
  std::string err;
  CHECK(!bad.parse("2+", &err));
  CHECK(!err.empty());
  CHECK(!bad.parse("foo(1", &err));
  Expression vars;
  CHECK(vars.parse("a*F + b", &err));
  auto v = vars.variables();
  CHECK(v.size() == 3);

  // Expression controller.
  Track t;
  t.setController(ControllerType::Expression);
  t.expression = "F*2 + S";
  TrackEnv env;
  env.fps = 30;
  CHECK_NEAR(t.evaluate(30, &env), 61.0, 1e-9);
  // Noise controller: deterministic and bounded.
  Track n;
  n.setController(ControllerType::Noise);
  n.noise.strength = 10;
  double a = n.evaluate(12.5), b = n.evaluate(12.5);
  CHECK(a == b);
  for (double f = 0; f < 200; f += 1.7) CHECK(std::fabs(n.evaluate(f)) <= 10.0 + 1e-9);
  // List controller: weighted sum.
  Track list;
  list.setController(ControllerType::List);
  list.active = 0;
  list.setKey(0, 2);
  CHECK(list.subs.size() >= 1);
  Track extra;
  extra.setKey(0, 3);
  list.subs.push_back(extra);
  list.subNames.push_back("Extra");
  list.weights.push_back(50.0);
  CHECK_NEAR(list.evaluate(0), 2.0 + 1.5, 1e-9);
  // JSON round trip.
  Track rt = Track::fromJson(list.toJson());
  CHECK_NEAR(rt.evaluate(0), 3.5, 1e-9);
}

static void testQuat() {
  Vec3 e(30, -20, 70);
  Quat q = Quat::fromEulerDeg(e);
  Mat4 a = q.toMatrix(), b = Mat4::fromEulerDeg(e);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) CHECK_NEAR(a.m[r][c], b.m[r][c], 1e-5);
  Vec3 back = q.toEulerDeg();
  Mat4 c = Mat4::fromEulerDeg(back);
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) CHECK_NEAR(c.m[r][k], b.m[r][k], 1e-4);
  Quat s = Quat::slerp(Quat(), Quat::fromAxisAngle(Vec3(0, 0, 1), kPi / 2), 0.5);
  CHECK_NEAR(s.toEulerDeg().z, 45.0, 1e-3);
}

static void testAutoKeyAndEvaluate() {
  Scene s;
  Animation* anim = Animation::of(s);
  CHECK(anim != nullptr);
  Node* box = s.createNode("Box");
  std::string px = Animation::nodePath(box->id, "pos.x");
  // Auto Key at frame 50: moving the box to x = 100 keys the original at frame 0.
  anim->setKey(s, px, 50, 100.0, 0, 0.0);
  CHECK(anim->isAnimated(px));
  CHECK(anim->findTrack(px)->keys.size() == 2);
  CHECK_NEAR(anim->valueAt(s, px, 25), 50.0, 1e-3);
  auto r = anim->evaluate(s, 25);
  CHECK(!r.transformChanged.empty());
  CHECK_NEAR(box->transform.position.x, 50.0, 1e-3);
  anim->evaluate(s, 50);
  CHECK_NEAR(box->transform.position.x, 100.0, 1e-4);
  // Object parameters are animatable.
  std::string rad = Animation::nodePath(box->id, "object/length");
  anim->setKey(s, rad, 10, 80.0, 0);
  anim->evaluate(s, 10);
  CHECK_NEAR(box->object->params.getFloat("length"), 80.0, 1e-4);
  // Save/load keeps the animation.
  json j = s.toJson();
  Scene t;
  CHECK(t.fromJson(j));
  Animation* a2 = Animation::of(t);
  CHECK(a2->isAnimated(px));
  CHECK_NEAR(a2->valueAt(t, px, 25), 50.0, 1e-3);
  // Delete keys.
  CHECK(anim->deleteKey(px, 50));
  CHECK(anim->findTrack(px) == nullptr || anim->findTrack(px)->keys.size() == 1);
  // A cleared scene has no animation.
  s.clear();
  CHECK(Animation::of(s)->empty());
}

static void testLayersWiresReactions() {
  Scene s;
  Animation* anim = Animation::of(s);
  Node* a = s.createNode("Box");
  Node* b = s.createNode("Sphere");
  std::string ax = Animation::nodePath(a->id, "pos.x"), bx = Animation::nodePath(b->id, "pos.x");
  anim->setKey(s, ax, 10, 10.0, 0, 0.0);
  // Additive layer.
  anim->layers.push_back(AnimLayer{"Layer 1", 100.0, true, {}});
  anim->activeLayer = 1;
  anim->setKey(s, ax, 10, 15.0, 0);
  CHECK_NEAR(anim->valueAt(s, ax, 10), 15.0, 1e-6);
  anim->layers[0].weight = 0.0;
  CHECK_NEAR(anim->valueAt(s, ax, 10), 10.0, 1e-6);
  anim->layers[0].weight = 100.0;
  anim->activeLayer = 0;
  // Wire: b.x = a.x * 2.
  anim->wires.push_back(Wire{ax, bx, "x*2", "x/2", false});
  CHECK_NEAR(anim->valueAt(s, bx, 10), 30.0, 1e-6);
  anim->evaluate(s, 10);
  CHECK_NEAR(b->transform.position.x, 30.0, 1e-4);
  anim->wires.clear();
  // Reaction: driver a.x 0..10 -> driven b.z 0..100.
  std::string bz = Animation::nodePath(b->id, "pos.z");
  Reaction rc;
  rc.driver = ax;
  rc.driven = bz;
  rc.points = {{0.0, 0.0}, {10.0, 100.0}};
  rc.smooth = false;
  anim->layers.clear();
  anim->reactions.push_back(rc);
  CHECK_NEAR(anim->valueAt(s, bz, 10), 100.0, 1e-6);
  CHECK_NEAR(anim->valueAt(s, bz, 5), anim->valueAt(s, ax, 5) * 10.0, 1e-6);
}

static Node* makeLine(Scene& s, const Vec3& a, const Vec3& b) {
  Node* n = s.createNode("EditableSpline");
  auto* sp = static_cast<EditableSplineObject*>(n->object.get());
  Spline line;
  line.knots = {cornerKnot(a), cornerKnot(b)};
  sp->shape.splines = {line};
  sp->params.setNumber("steps", 0);
  n->invalidate();
  return n;
}

static void testConstraints() {
  Scene s;
  Animation* anim = Animation::of(s);
  Node* path = makeLine(s, Vec3(0, 0, 0), Vec3(100, 0, 0));
  Node* box = s.createNode("Box");
  Constraint c;
  c.type = ConstraintType::Path;
  c.targets.push_back(ConstraintTarget{path->id});
  c.follow = true;
  int ci = anim->addConstraint(s, box->id, c);
  CHECK(ci == 0);
  // Animate the percent along the path: 0 at frame 0, 100 at frame 100.
  std::string pct = Animation::nodePath(box->id, "c:0/percent");
  anim->setKey(s, pct, 0, 0.0, 0);
  anim->setKey(s, pct, 100, 100.0, 0);
  for (auto& k : anim->findTrack(pct)->keys) k.inType = k.outType = TangentType::Linear;
  anim->evaluate(s, 50);
  CHECK_NEAR(box->worldPosition().x, 50.0, 0.5);
  CHECK_NEAR(box->worldPosition().y, 0.0, 1e-3);
  anim->evaluate(s, 100);
  CHECK_NEAR(box->worldPosition().x, 100.0, 0.5);
  // The path moves: the box follows.
  path->transform.position = Vec3(0, 20, 0);
  anim->evaluate(s, 100);
  CHECK_NEAR(box->worldPosition().y, 20.0, 1e-3);

  // Look-At: the X axis points at the target.
  Node* eye = s.createNode("Sphere");
  Node* target = s.createNode("Sphere");
  target->transform.position = Vec3(0, 50, 0);
  Constraint la;
  la.type = ConstraintType::LookAt;
  la.axis = 0;
  la.targets.push_back(ConstraintTarget{target->id});
  anim->addConstraint(s, eye->id, la);
  anim->evaluate(s, 0);
  Vec3 xAxis = normalize(eye->worldMatrix().column(0));
  CHECK_NEAR(xAxis.y, 1.0, 1e-3);

  // Position constraint with two weighted targets: the average.
  Node* p = s.createNode("Box");
  Node* t1 = s.createNode("Box");
  Node* t2 = s.createNode("Box");
  t1->transform.position = Vec3(10, 0, 0);
  t2->transform.position = Vec3(30, 0, 0);
  Constraint pc;
  pc.type = ConstraintType::Position;
  pc.targets = {ConstraintTarget{t1->id}, ConstraintTarget{t2->id}};
  anim->addConstraint(s, p->id, pc);
  anim->evaluate(s, 0);
  CHECK_NEAR(p->worldPosition().x, 20.0, 1e-3);

  // Link constraint: follows parent A, then B from frame 10.
  Node* ch = s.createNode("Box");
  Node* pa = s.createNode("Box");
  Node* pb = s.createNode("Box");
  pb->transform.position = Vec3(0, 0, 100);
  anim->addLinkTarget(s, ch->id, pa->id, 0);
  anim->addLinkTarget(s, ch->id, pb->id, 10);
  anim->setKey(s, Animation::nodePath(pb->id, "pos.z"), 20, 200.0, 0, 100.0);
  anim->evaluate(s, 20);
  CHECK(ch->worldPosition().z > 50.0);

  // Constraints survive save/load.
  Scene t;
  CHECK(t.fromJson(s.toJson()));
  CHECK(Animation::of(t)->nodes[box->id].constraints.size() == 1);
}

static void testIk() {
  // FABRIK reaches a reachable goal.
  std::vector<Vec3> j = {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(20, 0, 0), Vec3(30, 0, 0)};
  Vec3 goal(10, 15, 5);
  CHECK(solveFabrik(j, goal, 50, 0.01f));
  CHECK(length(j.back() - goal) < 0.02f);
  CHECK_NEAR(length(j[1] - j[0]), 10.0, 1e-3);
  CHECK_NEAR(length(j[2] - j[1]), 10.0, 1e-3);
  // Two-bone solver: exact.
  Vec3 root(0, 0, 0), mid(10, 0, 1), end(20, 0, 0);
  CHECK(solveTwoBone(root, mid, end, Vec3(12, 5, 0), Vec3(0, 0, 1)));
  CHECK(length(end - Vec3(12, 5, 0)) < 1e-3f);
  CHECK_NEAR(length(mid - root), length(Vec3(10, 0, 1)), 1e-3);
  // Unreachable goal: the chain stretches towards it.
  std::vector<Vec3> k = {Vec3(0, 0, 0), Vec3(10, 0, 0), Vec3(20, 0, 0)};
  solveFabrik(k, Vec3(0, 100, 0));
  CHECK(k.back().y > 19.0f);

  // Scene: a three-bone chain solved towards an IK chain helper.
  Scene s;
  Node* b0 = s.createNode("Bone");
  Node* b1 = s.createNode("Bone");
  Node* b2 = s.createNode("Bone");
  for (Node* b : {b0, b1, b2}) b->object->params.setNumber("length", 10);
  s.setParent(b1->id, b0->id);
  s.setParent(b2->id, b1->id);
  b1->transform.position = Vec3(10, 0, 0);
  b2->transform.position = Vec3(10, 0, 0);
  Node* ik = s.createNode("IKChain");
  ik->object->params.setNumber("startBone", b0->id);
  ik->object->params.setNumber("endBone", b2->id);
  ik->transform.position = Vec3(12, 12, 0);
  Animation::of(s)->evaluate(s, 0);
  CHECK(length(b2->worldPosition() - Vec3(12, 12, 0)) < 0.2f);
  CHECK_NEAR(length(b1->worldPosition() - b0->worldPosition()), 10.0, 1e-2);
  // Limb solver.
  ik->object->params.setNumber("solver", 1);
  ik->transform.position = Vec3(5, 14, 0);
  Animation::of(s)->evaluate(s, 1);
  CHECK(length(b2->worldPosition() - Vec3(5, 14, 0)) < 0.05f);
}

static void testSkinMorphPathDeform() {
  Scene s;
  Node* body = s.createNode("Box");
  body->object->params.setNumber("height", 40);
  body->object->params.setNumber("heightSegs", 4);
  body->invalidate();
  Node* bone = s.createNode("Bone");
  bone->transform.position = Vec3(0, 0, 20);
  auto skin = std::make_shared<SkinModifier>();
  body->modifiers.push_back(skin);
  CHECK(skin->addBone(s, bone->id, *body) == 0);
  body->invalidate();
  PolyMesh before = body->mesh();
  // All vertices weighted to the only bone: moving it moves the mesh rigidly.
  bone->transform.position = Vec3(0, 0, 30);
  Animation::of(s)->evaluate(s, 1);
  body->invalidate();
  const PolyMesh& after = body->mesh();
  CHECK(after.positions.size() == before.positions.size());
  float moved = 0;
  for (size_t i = 0; i < after.positions.size(); ++i) moved = std::max(moved, length(after.positions[i] - before.positions[i]));
  CHECK_NEAR(moved, 10.0, 1e-3);
  // Weight painting: moving vertex 0 fully to a second (unmoved) bone keeps it at the skin pose.
  Node* bone2 = s.createNode("Bone");
  bone2->transform.position = Vec3(200, 0, 0);
  CHECK(skin->addBone(s, bone2->id, *body) == 1);
  skin->setVertexWeight(before, 0, 1, 1.0f);
  CHECK(skin->overrides[0].size() == 1 && skin->overrides[0][0].first == 1);
  body->invalidate();
  CHECK(length(body->mesh().positions[0] - before.positions[0]) < 1e-3f);
  // Skin survives JSON.
  Scene t;
  CHECK(t.fromJson(s.toJson()));
  auto* skin2 = dynamic_cast<SkinModifier*>(t.findNode(body->id)->modifiers[0].get());
  CHECK(skin2 && skin2->bones.size() == 2 && skin2->overrides.size() == 1);

  // Morpher.
  Node* base = s.createNode("Box");
  Node* target = s.createNode("Box");
  target->object->params.setNumber("height", 50);
  target->invalidate();
  auto morph = std::make_shared<MorpherModifier>();
  base->modifiers.push_back(morph);
  CHECK(morph->captureTarget(0, *target));
  morph->params.setNumber("ch1", 50);
  base->invalidate();
  float baseH = target->mesh().bounds().hi.z;
  float plainH = s.createNode("Box")->mesh().bounds().hi.z;
  CHECK_NEAR(base->mesh().bounds().hi.z, (baseH + plainH) * 0.5f, 1e-3);

  // Path deform along a straight X path maps Z to X.
  std::vector<Vec3> pts = {Vec3(0, 0, 0), Vec3(1, 0, 10), Vec3(0, 2, 20)};
  std::vector<Vec3> path = {Vec3(0, 0, 0), Vec3(100, 0, 0)};
  deformAlongPolyline(pts, path, 2, 0.0f, 1.0f, 0.0f, 0.0f, false);
  CHECK_NEAR(pts[1].x, 10.0, 1e-4);
  CHECK_NEAR(pts[2].x, 20.0, 1e-4);
  CHECK_NEAR(length(Vec3(0, pts[1].y, pts[1].z)), 1.0, 1e-4);
}

static void testParticles() {
  Scene s;
  Node* spray = s.createNode("Spray");
  spray->transform.position = Vec3(0, 0, 100);
  auto* ps = dynamic_cast<ParticleSystemObject*>(spray->object.get());
  CHECK(ps != nullptr);
  auto count = [&](double f) { return ps->simulate(s, spray->id, f).size(); };
  size_t c5 = count(5), c20 = count(20);
  CHECK(c5 > 0);
  CHECK(c20 > c5);
  // Particle count grows in the mesh too.
  Animation::of(s)->evaluate(s, 3);
  size_t f3 = spray->mesh().faces.size();
  Animation::of(s)->evaluate(s, 15);
  size_t f15 = spray->mesh().faces.size();
  CHECK(f15 > f3);
  // Gravity bound to the system speeds the drops up.
  auto avgVz = [&](double f) {
    auto p = ps->simulate(s, spray->id, f);
    double v = 0;
    for (const auto& q : p) v += q.vel.z;
    return p.empty() ? 0.0 : v / p.size();
  };
  double vNoGravity = avgVz(20);
  Node* g = s.createNode("Gravity");
  CHECK(canBindToSpaceWarp(*spray, *g));
  CHECK(bindToSpaceWarp(*spray, *g));
  CHECK(!bindToSpaceWarp(*spray, *g));  // already bound
  double vGravity = avgVz(20);
  CHECK(vGravity < vNoGravity - 1.0);
  // A deflector below bounces particles up.
  Node* d = s.createNode("Deflector");
  d->object->params.setNumber("width", 1000);
  d->object->params.setNumber("length", 1000);
  d->transform.position = Vec3(0, 0, 50);
  bindToSpaceWarp(*spray, *d);
  auto parts = ps->simulate(s, spray->id, 40);
  size_t below = 0, up = 0;
  for (const auto& q : parts) {
    if (q.pos.z < 49.0f) ++below;
    if (q.vel.z > 0) ++up;
  }
  CHECK(below == 0);
  CHECK(up > 0);
  // Super Spray emits a total over its emission time.
  Node* ss = s.createNode("SuperSpray");
  ss->object->params.setNumber("useRate", 1);
  ss->object->params.setNumber("total", 60);
  ss->object->params.setNumber("emitStop", 30);
  ss->object->params.setNumber("life", 1000);
  auto* sso = dynamic_cast<ParticleSystemObject*>(ss->object.get());
  size_t total = sso->simulate(s, ss->id, 40).size();
  CHECK(total >= 58 && total <= 60);
  // Ripple deforms bound geometry.
  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("lengthSegs", 20);
  plane->object->params.setNumber("widthSegs", 20);
  Node* rip = s.createNode("Ripple");
  CHECK(!canBindToSpaceWarp(*spray, *rip));
  CHECK(bindToSpaceWarp(*plane, *rip));
  Aabb pb = plane->mesh().bounds();
  CHECK(pb.hi.z > 1.0f);
}

static void testRigidBodies() {
  // A ball dropped on the ground bounces lower each time.
  RigidWorld w;
  RigidBody ball;
  ball.shape = BodyShape::Sphere;
  ball.radius = 5;
  ball.bounce = 0.5f;
  ball.pos = Vec3(0, 0, 105);
  w.bodies.push_back(ball);
  w.settings.groundBounce = 0.5f;
  w.settings.substeps = 8;
  double e0 = 100.0 * 980.0;  // potential energy per unit mass above the resting height
  bool bounced = false;
  float peakAfter = 0;
  float prevVz = 0;
  for (int f = 0; f < 120; ++f) {
    w.stepFrame();
    const RigidBody& b = w.bodies[0];
    if (prevVz < 0 && b.vel.z > 0) bounced = true;
    if (bounced) peakAfter = std::max(peakAfter, b.pos.z - 5.0f);
    prevVz = b.vel.z;
  }
  CHECK(bounced);
  CHECK(peakAfter > 5.0f);
  CHECK(peakAfter < 60.0f);  // restitution 0.5 => about 25% of the drop height
  double e1 = peakAfter * 980.0;
  CHECK(e1 < e0 * 0.5);
  // Never sinks through the ground.
  CHECK(w.bodies[0].pos.z > 4.0f);

  // A box comes to rest on the ground.
  RigidWorld w2;
  RigidBody box;
  box.shape = BodyShape::Box;
  box.halfExtents = Vec3(5, 5, 5);
  box.bounce = 0.2f;
  box.pos = Vec3(0, 0, 40);
  box.rot = Quat::fromEulerDeg(Vec3(20, 10, 0));
  w2.bodies.push_back(box);
  for (int f = 0; f < 150; ++f) w2.stepFrame();
  CHECK(w2.bodies[0].pos.z > 3.5f && w2.bodies[0].pos.z < 9.0f);
  CHECK(length(w2.bodies[0].vel) < 20.0f);

  // Bake into keys.
  Scene s;
  Node* sph = s.createNode("Sphere");
  sph->transform.position = Vec3(0, 0, 100);
  sph->modifiers.push_back(std::make_shared<RigidBodyModifier>());
  sph->modifiers.back()->params.setNumber("shape", 0);
  sph->invalidate();
  BakeResult r = bakeRigidBodies(s, 0, 30, RigidWorldSettings());
  CHECK(r.bodies == 1 && r.frames == 30);
  Animation* anim = Animation::of(s);
  CHECK(anim->isAnimated(Animation::nodePath(sph->id, "pos.z")));
  CHECK(anim->valueAt(s, Animation::nodePath(sph->id, "pos.z"), 10) < 100.0);
  CHECK(unbakeRigidBodies(s) == 1);
  CHECK(!anim->isAnimated(Animation::nodePath(sph->id, "pos.z")));
}

static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
  return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}

static void testAvi() {
  std::string path = (std::filesystem::temp_directory_path() / "veraa_anim_test.avi").string();
  {
    AviWriter w;
    CHECK(w.open(path, 32, 16, 25.0, 80));
    std::vector<uint8_t> px(32 * 16 * 3);
    for (int f = 0; f < 3; ++f) {
      for (size_t i = 0; i < px.size(); ++i) px[i] = static_cast<uint8_t>((i * 7 + f * 40) & 0xFF);
      CHECK(w.addFrame(px.data(), 3));
    }
    CHECK(w.frameCount() == 3);
    CHECK(w.close());
  }
  std::ifstream in(path, std::ios::binary);
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(b.size() > 300);
  if (b.size() < 300) return;
  CHECK(std::memcmp(&b[0], "RIFF", 4) == 0);
  CHECK(rd32(b, 4) == b.size() - 8);
  CHECK(std::memcmp(&b[8], "AVI ", 4) == 0);
  CHECK(std::memcmp(&b[12], "LIST", 4) == 0);
  CHECK(std::memcmp(&b[20], "hdrl", 4) == 0);
  CHECK(std::memcmp(&b[24], "avih", 4) == 0);
  CHECK(rd32(b, 28) == 56);
  CHECK(rd32(b, 32) == 40000);  // microseconds per frame at 25 fps
  CHECK(rd32(b, 48) == 3);      // total frames
  CHECK(rd32(b, 64) == 32 && rd32(b, 68) == 16);
  // Walk to the movi list and check the frames are JPEGs.
  size_t hdrlEnd = 20 + rd32(b, 16);
  CHECK(std::memcmp(&b[hdrlEnd], "LIST", 4) == 0);
  CHECK(std::memcmp(&b[hdrlEnd + 8], "movi", 4) == 0);
  size_t movi = hdrlEnd + 8, pos = movi + 4, end = hdrlEnd + 8 + rd32(b, hdrlEnd + 4);
  int frames = 0;
  while (pos + 8 <= end) {
    CHECK(std::memcmp(&b[pos], "00dc", 4) == 0);
    uint32_t sz = rd32(b, pos + 4);
    CHECK(b[pos + 8] == 0xFF && b[pos + 9] == 0xD8);
    pos += 8 + sz + (sz & 1);
    ++frames;
  }
  CHECK(frames == 3);
  CHECK(std::memcmp(&b[end], "idx1", 4) == 0);
  CHECK(rd32(b, end + 4) == 48);
  CHECK(rd32(b, end + 8 + 8) == 4);  // first frame offset relative to "movi"
  std::filesystem::remove(path);
}

int main() {
  // Unbuffered, so a crash still shows how far the tests got.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const struct {
    const char* name;
    void (*run)();
  } tests[] = {
      {"testBezier", testBezier},
      {"testTcb", testTcb},
      {"testOutOfRange", testOutOfRange},
      {"testExpressions", testExpressions},
      {"testQuat", testQuat},
      {"testAutoKeyAndEvaluate", testAutoKeyAndEvaluate},
      {"testLayersWiresReactions", testLayersWiresReactions},
      {"testConstraints", testConstraints},
      {"testIk", testIk},
      {"testSkinMorphPathDeform", testSkinMorphPathDeform},
      {"testParticles", testParticles},
      {"testRigidBodies", testRigidBodies},
      {"testAvi", testAvi},
  };
  for (const auto& t : tests) {
    std::printf("%s\n", t.name);
    try {
      t.run();
    } catch (const std::exception& e) {
      std::printf("FAIL %s threw: %s\n", t.name, e.what());
      ++g_failures;
    }
  }
  if (g_failures) {
    std::printf("%d animation test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All animation tests passed\n");
  return 0;
}
