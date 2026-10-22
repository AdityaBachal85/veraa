// Track evaluation (controllers, tangents, out-of-range types) and quaternion math.
#include <algorithm>
#include <cmath>

#include "animation.h"
#include "modifier.h"  // perlinNoise

namespace veraa {

const char* tangentTypeName(TangentType t) {
  switch (t) {
    case TangentType::Auto: return "Auto";
    case TangentType::Smooth: return "Smooth";
    case TangentType::Linear: return "Linear";
    case TangentType::Step: return "Step";
    case TangentType::Fast: return "Fast";
    case TangentType::Slow: return "Slow";
    case TangentType::Custom: return "Custom";
  }
  return "Auto";
}

const char* outOfRangeName(OutOfRange o) {
  switch (o) {
    case OutOfRange::Constant: return "Constant";
    case OutOfRange::Cycle: return "Cycle";
    case OutOfRange::Loop: return "Loop";
    case OutOfRange::PingPong: return "Ping Pong";
    case OutOfRange::Linear: return "Linear";
    case OutOfRange::RelativeRepeat: return "Relative Repeat";
  }
  return "Constant";
}

const char* controllerTypeName(ControllerType c) {
  switch (c) {
    case ControllerType::Bezier: return "Bezier";
    case ControllerType::Linear: return "Linear";
    case ControllerType::TCB: return "TCB";
    case ControllerType::Noise: return "Noise";
    case ControllerType::Expression: return "Expression";
    case ControllerType::List: return "List";
  }
  return "Bezier";
}

// ---------------------------------------------------------------------------------------------
// Track

bool Track::hasKeys() const {
  if (!keys.empty()) return true;
  for (const auto& s : subs)
    if (s.hasKeys() || s.procedural()) return true;
  return false;
}

bool Track::keyRange(double& t0, double& t1) const {
  bool any = false;
  if (!keys.empty()) {
    t0 = keys.front().time;
    t1 = keys.back().time;
    any = true;
  }
  for (const auto& s : subs) {
    double a, b;
    if (s.keyRange(a, b)) {
      t0 = any ? std::min(t0, a) : a;
      t1 = any ? std::max(t1, b) : b;
      any = true;
    }
  }
  return any;
}

int Track::keyIndexAt(double frame, double eps) const {
  for (size_t i = 0; i < keys.size(); ++i)
    if (std::fabs(keys[i].time - frame) <= eps) return static_cast<int>(i);
  return -1;
}

void Track::sortKeys() {
  std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

int Track::setKey(double frame, double value) {
  if (type == ControllerType::List) {
    if (subs.empty()) {
      subs.emplace_back();
      subNames.push_back("Bezier Float");
      weights.push_back(100.0);
      active = 0;
    }
    active = std::clamp(active, 0, static_cast<int>(subs.size()) - 1);
    double w = weights[active] / 100.0;
    double others = 0.0;
    for (size_t i = 0; i < subs.size(); ++i)
      if (static_cast<int>(i) != active) others += weights[i] / 100.0 * subs[i].evaluate(frame);
    return subs[active].setKey(frame, w != 0.0 ? (value - others) / w : value);
  }
  int idx = keyIndexAt(frame);
  if (idx >= 0) {
    keys[idx].value = value;
    return idx;
  }
  Key k;
  k.time = frame;
  k.value = value;
  if (type == ControllerType::Linear) k.inType = k.outType = TangentType::Linear;
  keys.push_back(k);
  sortKeys();
  return keyIndexAt(frame);
}

bool Track::deleteKeyAt(double frame, double eps) {
  if (type == ControllerType::List && !subs.empty())
    return subs[std::clamp(active, 0, static_cast<int>(subs.size()) - 1)].deleteKeyAt(frame, eps);
  int idx = keyIndexAt(frame, eps);
  if (idx < 0) return false;
  keys.erase(keys.begin() + idx);
  return true;
}

void Track::deleteKeysInRange(double t0, double t1) {
  keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Key& k) { return k.time >= t0 && k.time <= t1; }),
             keys.end());
  for (auto& s : subs) s.deleteKeysInRange(t0, t1);
}

void Track::offsetValues(double delta) {
  for (auto& k : keys) k.value += delta;
  if (type == ControllerType::List && !subs.empty()) {
    int a = std::clamp(active, 0, static_cast<int>(subs.size()) - 1);
    double w = weights[a] / 100.0;
    subs[a].offsetValues(w != 0.0 ? delta / w : delta);
  }
}

void Track::keyTimes(std::vector<double>& out) const {
  for (const auto& k : keys) out.push_back(k.time);
  for (const auto& s : subs) s.keyTimes(out);
}

void Track::setController(ControllerType t) {
  if (t == type) return;
  if (t == ControllerType::List) {
    // Wrap the current controller as the first list entry.
    Track inner = *this;
    *this = Track();
    type = ControllerType::List;
    before = inner.before;
    after = inner.after;
    subs.push_back(std::move(inner));
    subNames.push_back(std::string(controllerTypeName(subs.back().type)) + " Float");
    weights.push_back(100.0);
    active = 0;
    return;
  }
  if (type == ControllerType::List && !subs.empty()) {
    Track inner = subs[0];
    *this = inner;
  }
  type = t;
  for (auto& k : keys) {
    if (t == ControllerType::Linear) k.inType = k.outType = TangentType::Linear;
    else if (t == ControllerType::Bezier && k.inType == TangentType::Linear && k.outType == TangentType::Linear)
      k.inType = k.outType = TangentType::Auto;
  }
}

void Track::slopes(size_t i, double& in, double& out) const {
  const size_t n = keys.size();
  const Key& k = keys[i];
  bool hasPrev = i > 0, hasNext = i + 1 < n;
  double sPrev = hasPrev ? (k.value - keys[i - 1].value) / std::max(k.time - keys[i - 1].time, 1e-9) : 0.0;
  double sNext = hasNext ? (keys[i + 1].value - k.value) / std::max(keys[i + 1].time - k.time, 1e-9) : 0.0;
  auto catmull = [&] {
    if (hasPrev && hasNext)
      return (keys[i + 1].value - keys[i - 1].value) / std::max(keys[i + 1].time - keys[i - 1].time, 1e-9);
    return hasPrev ? sPrev : (hasNext ? sNext : 0.0);
  };
  auto slopeFor = [&](TangentType t, bool incoming, double custom) -> double {
    switch (t) {
      case TangentType::Auto: {
        // Flat at the first and last keys and at peaks and valleys; otherwise smooth but limited
        // so the curve doesn't overshoot.
        if (!hasPrev || !hasNext) return 0.0;
        if (sPrev * sNext <= 0.0) return 0.0;
        double c = catmull();
        if (hasPrev && hasNext) {
          double lim = 3.0 * std::min(std::fabs(sPrev), std::fabs(sNext));
          c = std::clamp(c, -lim, lim);
        }
        return c;
      }
      case TangentType::Smooth: return catmull();
      case TangentType::Linear: return incoming ? (hasPrev ? sPrev : sNext) : (hasNext ? sNext : sPrev);
      case TangentType::Step: return 0.0;
      case TangentType::Fast: return 2.0 * (incoming ? (hasPrev ? sPrev : sNext) : (hasNext ? sNext : sPrev));
      case TangentType::Slow: return 0.0;
      case TangentType::Custom: return custom;
    }
    return 0.0;
  };
  in = slopeFor(k.inType, true, k.inSlope);
  out = slopeFor(k.outType, false, k.outSlope);
}

namespace {

// Hermite basis on s in [0,1].
inline double hermite(double p0, double m0, double p1, double m1, double s) {
  double s2 = s * s, s3 = s2 * s;
  return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
}

// Ease mapping for TCB: slows the start (easeFrom) and the end (easeTo) of a segment.
inline double easeParam(double s, double easeFrom, double easeTo) {
  double a = std::clamp(easeFrom / 50.0, 0.0, 1.0), b = std::clamp(easeTo / 50.0, 0.0, 1.0);
  if (a == 0.0 && b == 0.0) return s;
  return hermite(0.0, 1.0 - a, 1.0, 1.0 - b, s);
}

double cubicBezier(double p0, double p1, double p2, double p3, double u) {
  double v = 1.0 - u;
  return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
}

double cubicBezierDeriv(double p0, double p1, double p2, double p3, double u) {
  double v = 1.0 - u;
  return 3 * v * v * (p1 - p0) + 6 * v * u * (p2 - p1) + 3 * u * u * (p3 - p2);
}

}  // namespace

double Track::evaluateKeys(double t) const {
  const size_t n = keys.size();
  if (n == 0) return 0.0;
  if (n == 1 || t <= keys.front().time) return keys.front().value;
  if (t >= keys.back().time) return keys.back().value;
  auto it = std::upper_bound(keys.begin(), keys.end(), t, [](double v, const Key& k) { return v < k.time; });
  size_t i = static_cast<size_t>(std::distance(keys.begin(), it)) - 1;
  const Key& a = keys[i];
  const Key& b = keys[i + 1];
  double dt = std::max(b.time - a.time, 1e-9);
  double local = t - a.time;

  if (type == ControllerType::Linear) return a.value + (b.value - a.value) * (local / dt);

  if (type == ControllerType::TCB) {
    // Kochanek-Bartels tangents (per-segment units) with time adjustment for uneven spacing.
    auto tangents = [&](size_t j, double& ds, double& dd) {
      const Key& k = keys[j];
      bool hp = j > 0, hn = j + 1 < n;
      double g1 = hp ? k.value - keys[j - 1].value : (hn ? keys[j + 1].value - k.value : 0.0);
      double g2 = hn ? keys[j + 1].value - k.value : g1;
      double T = k.tension, C = k.continuity, B = k.bias;
      ds = (1 - T) * (1 + C) * (1 + B) / 2 * g1 + (1 - T) * (1 - C) * (1 - B) / 2 * g2;
      dd = (1 - T) * (1 - C) * (1 + B) / 2 * g1 + (1 - T) * (1 + C) * (1 - B) / 2 * g2;
      if (hp && hn) {
        double dp = k.time - keys[j - 1].time, dn = keys[j + 1].time - k.time;
        ds *= 2.0 * dn / (dp + dn);
        dd *= 2.0 * dp / (dp + dn);
      }
    };
    double dsA, ddA, dsB, ddB;
    tangents(i, dsA, ddA);
    tangents(i + 1, dsB, ddB);
    double s = easeParam(local / dt, a.easeFrom, b.easeTo);
    return hermite(a.value, dsA, b.value, ddB, s);
  }

  // Bezier.
  if (a.outType == TangentType::Step || b.inType == TangentType::Step) return a.value;
  double aIn, aOut, bIn, bOut;
  slopes(i, aIn, aOut);
  slopes(i + 1, bIn, bOut);
  double la = a.outType == TangentType::Custom ? std::clamp(a.outLength, 0.0, 1.0) : 1.0 / 3.0;
  double lb = b.inType == TangentType::Custom ? std::clamp(b.inLength, 0.0, 1.0) : 1.0 / 3.0;
  double x1 = la * dt, x2 = dt - lb * dt;
  double y0 = a.value, y1 = a.value + aOut * la * dt, y2 = b.value - bIn * lb * dt, y3 = b.value;
  // Solve x(u) = local (x is monotonic because the handle times stay inside the interval).
  double u = local / dt;
  for (int it2 = 0; it2 < 8; ++it2) {
    double x = cubicBezier(0.0, x1, x2, dt, u) - local;
    double dx = cubicBezierDeriv(0.0, x1, x2, dt, u);
    if (std::fabs(x) < 1e-9 * dt) break;
    if (std::fabs(dx) < 1e-12) break;
    double next = u - x / dx;
    if (next < 0.0 || next > 1.0) break;
    u = next;
  }
  // Bisection refinement for robustness.
  double lo = 0.0, hi = 1.0;
  if (std::fabs(cubicBezier(0.0, x1, x2, dt, u) - local) > 1e-6 * dt) {
    for (int it2 = 0; it2 < 50; ++it2) {
      double mid = 0.5 * (lo + hi);
      if (cubicBezier(0.0, x1, x2, dt, mid) < local) lo = mid;
      else hi = mid;
    }
    u = 0.5 * (lo + hi);
  }
  return cubicBezier(y0, y1, y2, y3, u);
}

double Track::evaluateInRange(double t, const TrackEnv* env) const {
  (void)env;
  return evaluateKeys(t);
}

double Track::evaluate(double frame, const TrackEnv* env) const {
  switch (type) {
    case ControllerType::Noise: {
      double x = frame / 30.0 * noise.frequency;
      double sx = noise.seed * 7.13 + 0.37, sy = noise.seed * 3.71 + 0.5;
      double v = 0.0;
      if (noise.fractal) {
        double amp = 1.0, freq = 1.0, norm = 0.0, gain = 0.45 + 0.4 * std::clamp(noise.roughness, 0.0, 1.0);
        for (int o = 0; o < 4; ++o) {
          v += amp * perlinNoise(static_cast<float>(x * freq + sx), static_cast<float>(sy + o * 1.7), 0.5f);
          norm += amp;
          amp *= gain;
          freq *= 2.0;
        }
        v /= norm;
      } else {
        v = perlinNoise(static_cast<float>(x + sx), static_cast<float>(sy), 0.5f);
      }
      return noise.strength * v;
    }
    case ControllerType::Expression: {
      if (!compiled_ || compiledText_ != expression) {
        auto e = std::make_shared<Expression>();
        e->parse(expression);
        compiled_ = e;
        compiledText_ = expression;
      }
      double fps = env ? env->fps : 30.0;
      double start = env ? env->startFrame : 0.0, end = env ? env->endFrame : 100.0;
      return compiled_->evaluate([&](const std::string& name) -> double {
        if (name == "F") return frame;
        if (name == "T") return frame * 4800.0 / fps;
        if (name == "S") return frame / fps;
        if (name == "NT") return end > start ? (frame - start) / (end - start) : 0.0;
        for (const auto& v : exprVars)
          if (v.name == name) {
            if (v.path.empty() || !env || !env->trackValue) return v.constant;
            return env->trackValue(v.path, frame + v.frameOffset);
          }
        return 0.0;
      });
    }
    case ControllerType::List: {
      double sum = 0.0;
      for (size_t i = 0; i < subs.size(); ++i)
        sum += (i < weights.size() ? weights[i] / 100.0 : 1.0) * subs[i].evaluate(frame, env);
      return sum;
    }
    default: break;
  }
  if (keys.empty()) return 0.0;
  double t0 = keys.front().time, t1 = keys.back().time, len = t1 - t0;
  if (len <= 1e-9 || (frame >= t0 && frame <= t1)) return evaluateInRange(frame, env);
  OutOfRange mode = frame < t0 ? before : after;
  switch (mode) {
    case OutOfRange::Constant: return evaluateInRange(std::clamp(frame, t0, t1), env);
    case OutOfRange::Cycle:
    case OutOfRange::Loop: {
      double r = std::fmod(frame - t0, len);
      if (r < 0) r += len;
      return evaluateInRange(t0 + r, env);
    }
    case OutOfRange::PingPong: {
      double k = std::floor((frame - t0) / len);
      double r = (frame - t0) - k * len;
      bool odd = static_cast<long long>(std::fabs(k)) % 2 == 1;
      return evaluateInRange(odd ? t1 - r : t0 + r, env);
    }
    case OutOfRange::Linear: {
      double eps = std::min(0.01, len * 0.01);
      if (frame < t0) {
        double s = (evaluateInRange(t0 + eps, env) - evaluateInRange(t0, env)) / eps;
        return keys.front().value + s * (frame - t0);
      }
      double s = (evaluateInRange(t1, env) - evaluateInRange(t1 - eps, env)) / eps;
      return keys.back().value + s * (frame - t1);
    }
    case OutOfRange::RelativeRepeat: {
      double k = std::floor((frame - t0) / len);
      double r = (frame - t0) - k * len;
      return evaluateInRange(t0 + r, env) + k * (keys.back().value - keys.front().value);
    }
  }
  return evaluateInRange(std::clamp(frame, t0, t1), env);
}

double Track::derivative(double frame, const TrackEnv* env) const {
  const double h = 0.01;
  return (evaluate(frame + h, env) - evaluate(frame - h, env)) / (2 * h);
}

json Track::toJson() const {
  json j;
  j["c"] = static_cast<int>(type);
  if (before != OutOfRange::Constant) j["before"] = static_cast<int>(before);
  if (after != OutOfRange::Constant) j["after"] = static_cast<int>(after);
  json ks = json::array();
  for (const auto& k : keys)
    ks.push_back(json::array({k.time, k.value, static_cast<int>(k.inType), static_cast<int>(k.outType), k.inSlope,
                              k.outSlope, k.inLength, k.outLength, k.tension, k.continuity, k.bias, k.easeTo,
                              k.easeFrom}));
  j["k"] = std::move(ks);
  if (type == ControllerType::Noise)
    j["noise"] = json{{"seed", noise.seed},
                      {"frequency", noise.frequency},
                      {"strength", noise.strength},
                      {"fractal", noise.fractal},
                      {"roughness", noise.roughness}};
  if (!expression.empty()) j["expr"] = expression;
  if (!exprVars.empty()) {
    json vs = json::array();
    for (const auto& v : exprVars)
      vs.push_back(json{{"name", v.name}, {"path", v.path}, {"constant", v.constant}, {"offset", v.frameOffset}});
    j["vars"] = std::move(vs);
  }
  if (!subs.empty()) {
    json ss = json::array();
    for (const auto& s : subs) ss.push_back(s.toJson());
    j["subs"] = std::move(ss);
    j["subNames"] = subNames;
    j["weights"] = weights;
    j["active"] = active;
  }
  return j;
}

Track Track::fromJson(const json& j) {
  Track t;
  if (!j.is_object()) return t;
  t.type = static_cast<ControllerType>(std::clamp(j.value("c", 0), 0, 5));
  t.before = static_cast<OutOfRange>(std::clamp(j.value("before", 0), 0, 5));
  t.after = static_cast<OutOfRange>(std::clamp(j.value("after", 0), 0, 5));
  if (j.contains("k"))
    for (const auto& a : j["k"]) {
      if (!a.is_array() || a.size() < 2) continue;
      Key k;
      auto num = [&](size_t i, double def) { return i < a.size() && a[i].is_number() ? a[i].get<double>() : def; };
      k.time = num(0, 0);
      k.value = num(1, 0);
      k.inType = static_cast<TangentType>(std::clamp(static_cast<int>(num(2, 0)), 0, 6));
      k.outType = static_cast<TangentType>(std::clamp(static_cast<int>(num(3, 0)), 0, 6));
      k.inSlope = num(4, 0);
      k.outSlope = num(5, 0);
      k.inLength = num(6, 1.0 / 3.0);
      k.outLength = num(7, 1.0 / 3.0);
      k.tension = num(8, 0);
      k.continuity = num(9, 0);
      k.bias = num(10, 0);
      k.easeTo = num(11, 0);
      k.easeFrom = num(12, 0);
      t.keys.push_back(k);
    }
  t.sortKeys();
  if (j.contains("noise")) {
    const json& n = j["noise"];
    t.noise.seed = n.value("seed", 0);
    t.noise.frequency = n.value("frequency", 0.5);
    t.noise.strength = n.value("strength", 50.0);
    t.noise.fractal = n.value("fractal", true);
    t.noise.roughness = n.value("roughness", 0.0);
  }
  t.expression = j.value("expr", std::string());
  if (j.contains("vars"))
    for (const auto& v : j["vars"])
      t.exprVars.push_back(
          {v.value("name", std::string()), v.value("path", std::string()), v.value("constant", 0.0), v.value("offset", 0.0)});
  if (j.contains("subs")) {
    for (const auto& s : j["subs"]) t.subs.push_back(Track::fromJson(s));
    t.subNames = j.value("subNames", std::vector<std::string>{});
    t.weights = j.value("weights", std::vector<double>{});
    t.active = j.value("active", 0);
    t.subNames.resize(t.subs.size(), "Float");
    t.weights.resize(t.subs.size(), 100.0);
  }
  return t;
}

// ---------------------------------------------------------------------------------------------
// Quaternions

Quat Quat::fromAxisAngle(const Vec3& axisIn, double radians) {
  Vec3 a = normalize(axisIn);
  double s = std::sin(radians * 0.5);
  return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5)};
}

Quat Quat::fromEulerDeg(const Vec3& deg) {
  Quat qx = fromAxisAngle({1, 0, 0}, radians(deg.x));
  Quat qy = fromAxisAngle({0, 1, 0}, radians(deg.y));
  Quat qz = fromAxisAngle({0, 0, 1}, radians(deg.z));
  return (qz * qy * qx).normalized();
}

Quat Quat::fromMatrix(const Mat4& in) {
  Vec3 c0 = normalize(in.column(0)), c1 = normalize(in.column(1)), c2 = normalize(in.column(2));
  double m00 = c0.x, m10 = c0.y, m20 = c0.z;
  double m01 = c1.x, m11 = c1.y, m21 = c1.z;
  double m02 = c2.x, m12 = c2.y, m22 = c2.z;
  double tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0) {
    double s = std::sqrt(tr + 1.0) * 2;
    q.w = 0.25 * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
    q.w = (m21 - m12) / s;
    q.x = 0.25 * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25 * s;
    q.z = (m12 + m21) / s;
  } else {
    double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25 * s;
  }
  return q.normalized();
}

Mat4 Quat::toMatrix() const {
  Quat q = normalized();
  double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  Mat4 m;
  m.m[0][0] = static_cast<float>(1 - 2 * (yy + zz));
  m.m[0][1] = static_cast<float>(2 * (xy - wz));
  m.m[0][2] = static_cast<float>(2 * (xz + wy));
  m.m[1][0] = static_cast<float>(2 * (xy + wz));
  m.m[1][1] = static_cast<float>(1 - 2 * (xx + zz));
  m.m[1][2] = static_cast<float>(2 * (yz - wx));
  m.m[2][0] = static_cast<float>(2 * (xz - wy));
  m.m[2][1] = static_cast<float>(2 * (yz + wx));
  m.m[2][2] = static_cast<float>(1 - 2 * (xx + yy));
  return m;
}

Vec3 Quat::toEulerDeg() const { return eulerDegFromMatrix(toMatrix()); }

Quat Quat::operator*(const Quat& o) const {
  return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
          w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
}

Quat Quat::normalized() const {
  double l = std::sqrt(x * x + y * y + z * z + w * w);
  if (l < 1e-15) return Quat();
  return {x / l, y / l, z / l, w / l};
}

Quat Quat::slerp(const Quat& a, Quat b, double t) {
  double d = a.dot(b);
  if (d < 0) {
    b = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  if (d > 0.9995) {
    Quat r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    return r.normalized();
  }
  double th = std::acos(std::clamp(d, -1.0, 1.0));
  double s = std::sin(th);
  double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
  return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
}

Quat Quat::log(const Quat& q) {
  double vlen = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (vlen < 1e-12) return {0, 0, 0, 0};
  double th = std::atan2(vlen, q.w);
  double k = th / vlen;
  return {q.x * k, q.y * k, q.z * k, 0};
}

Quat Quat::exp(const Quat& q) {
  double th = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
  if (th < 1e-12) return Quat();
  double k = std::sin(th) / th;
  return {q.x * k, q.y * k, q.z * k, std::cos(th)};
}

}  // namespace veraa
