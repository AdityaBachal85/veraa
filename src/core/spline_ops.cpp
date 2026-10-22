#include "spline_ops.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace veraa::splineops {

namespace {

constexpr float kEps = 1e-6f;

Vec2 xy(const Vec3& p) { return Vec2(p.x, p.y); }

Vec3 dirTo(const Vec3& from, const Vec3& to) {
  Vec3 d = to - from;
  return length2(d) > 1e-20f ? normalize(d) : Vec3();
}

// Unit directions from knot k toward the incoming and outgoing curve.
void knotDirs(const Spline& sp, int k, Vec3& toPrev, Vec3& toNext, float& lenPrev, float& lenNext) {
  int n = static_cast<int>(sp.knots.size());
  const Knot& kn = sp.knots[k];
  bool hasPrev = sp.closed || k > 0, hasNext = sp.closed || k < n - 1;
  const Knot* prev = hasPrev ? &sp.knots[(k + n - 1) % n] : nullptr;
  const Knot* next = hasNext ? &sp.knots[(k + 1) % n] : nullptr;
  toPrev = length2(kn.in - kn.p) > 1e-12f ? dirTo(kn.p, kn.in) : (prev ? dirTo(kn.p, prev->p) : Vec3());
  toNext = length2(kn.out - kn.p) > 1e-12f ? dirTo(kn.p, kn.out) : (next ? dirTo(kn.p, next->p) : Vec3());
  lenPrev = prev ? length(prev->p - kn.p) : 0.0f;
  lenNext = next ? length(next->p - kn.p) : 0.0f;
}

void translateKnot(Knot& k, const Vec3& d) {
  k.p += d;
  k.in += d;
  k.out += d;
}

void reverseSpline(Spline& sp) {
  std::reverse(sp.knots.begin(), sp.knots.end());
  for (auto& k : sp.knots) std::swap(k.in, k.out);
}

// Points along a spline with (segment, t) of each point. No optimization so the mapping is exact.
struct Dense {
  std::vector<Vec3> pts;
  std::vector<std::pair<int, float>> param;
  std::vector<float> arc;  // cumulative length
};

Dense densify(const Spline& sp, int steps) {
  Dense d;
  int segs = sp.segmentCount();
  for (int s = 0; s < segs; ++s)
    for (int i = 0; i <= steps; ++i) {
      if (i == steps && s + 1 < segs) continue;
      if (i == steps && sp.closed) continue;
      float t = static_cast<float>(i) / steps;
      d.pts.push_back(sp.evaluate(s, t));
      d.param.emplace_back(s, t);
    }
  if (sp.closed && !d.pts.empty()) {  // close the loop explicitly
    d.pts.push_back(d.pts.front());
    d.param.emplace_back(segs - 1, 1.0f);
  }
  if (segs == 0 && !sp.knots.empty()) {
    d.pts.push_back(sp.knots[0].p);
    d.param.emplace_back(0, 0.0f);
  }
  d.arc.resize(d.pts.size(), 0.0f);
  for (size_t i = 1; i < d.pts.size(); ++i) d.arc[i] = d.arc[i - 1] + length(d.pts[i] - d.pts[i - 1]);
  return d;
}

// Interpolated (segment, t) at fraction f between dense points j and j + 1.
std::pair<int, float> paramAt(const Dense& d, size_t j, float f) {
  auto a = d.param[j], b = d.param[std::min(j + 1, d.param.size() - 1)];
  if (b.first != a.first) b = {a.first, 1.0f};
  return {a.first, a.second + (b.second - a.second) * f};
}

Spline polylineSpline(const std::vector<Vec3>& pts, bool closed) {
  Spline sp;
  for (const auto& p : pts) sp.knots.push_back(cornerKnot(p));
  sp.closed = closed;
  sp.updateAutoHandles();
  return sp;
}

// Removes nearly collinear points from a closed polygon.
std::vector<Vec2> simplifyCollinear(const std::vector<Vec2>& p) {
  if (p.size() < 4) return p;
  std::vector<Vec2> out;
  size_t n = p.size();
  for (size_t i = 0; i < n; ++i) {
    Vec2 a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
    float cr = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    float scale = std::max(1e-6f, std::sqrt((c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y)));
    if (std::fabs(cr) / scale > 1e-4f * scale + 1e-5f) out.push_back(b);
  }
  return out.size() >= 3 ? out : p;
}

Vec3 offsetAt(const Spline& sp, int k, float d) {
  int n = static_cast<int>(sp.knots.size());
  const Knot& kn = sp.knots[k];
  bool hasPrev = sp.closed || k > 0, hasNext = sp.closed || k < n - 1;
  Vec3 dirIn, dirOut;
  if (hasPrev) {
    const Knot& prev = sp.knots[(k + n - 1) % n];
    dirIn = length2(kn.p - kn.in) > 1e-12f ? dirTo(kn.in, kn.p) : dirTo(prev.p, kn.p);
  }
  if (hasNext) {
    const Knot& next = sp.knots[(k + 1) % n];
    dirOut = length2(kn.out - kn.p) > 1e-12f ? dirTo(kn.p, kn.out) : dirTo(kn.p, next.p);
  }
  if (!hasPrev) dirIn = dirOut;
  if (!hasNext) dirOut = dirIn;
  Vec3 nIn(dirIn.y, -dirIn.x, 0), nOut(dirOut.y, -dirOut.x, 0);
  Vec3 nrm = nIn + nOut;
  if (length2(nrm) < 1e-12f) nrm = nOut;
  nrm = normalize(nrm);
  float c = std::max(0.3f, dot(nrm, nOut));
  return nrm * (d / c);
}

Spline offsetSpline(const Spline& sp, float d) {
  Spline o = sp;
  for (int k = 0; k < static_cast<int>(sp.knots.size()); ++k) translateKnot(o.knots[k], offsetAt(sp, k, d));
  return o;
}

}  // namespace

void setKnotType(BezierShape& s, const std::vector<KnotRef>& knots, KnotType type) {
  std::set<int> touched;
  for (const auto& r : knots) {
    if (r.spline < 0 || r.spline >= static_cast<int>(s.splines.size())) continue;
    Spline& sp = s.splines[r.spline];
    if (r.knot < 0 || r.knot >= static_cast<int>(sp.knots.size())) continue;
    Knot& k = sp.knots[r.knot];
    if ((type == KnotType::Bezier) && (k.type == KnotType::Corner || k.type == KnotType::BezierCorner)) {
      // Give it smooth handles first.
      KnotType old = k.type;
      k.type = KnotType::Smooth;
      sp.updateAutoHandles();
      (void)old;
    }
    if (type == KnotType::Bezier) {
      // Make handles collinear, keeping their lengths.
      Vec3 a = k.in - k.p, b = k.out - k.p;
      Vec3 dir = b - a;
      if (length2(dir) > 1e-20f) {
        dir = normalize(dir);
        k.in = k.p - dir * length(a);
        k.out = k.p + dir * length(b);
      }
    }
    k.type = type;
    touched.insert(r.spline);
  }
  for (int i : touched) s.splines[i].updateAutoHandles();
}

KnotRef refine(BezierShape& s, SegmentRef seg, float t) {
  Spline& sp = s.splines[seg.spline];
  int n = static_cast<int>(sp.knots.size());
  int ia = seg.knot, ib = (seg.knot + 1) % n;
  Knot& a = sp.knots[ia];
  Knot& b = sp.knots[ib];
  t = clampf(t, 0.0f, 1.0f);
  Knot nk;
  if (a.type == KnotType::Corner && b.type == KnotType::Corner) {
    nk = cornerKnot(lerp(a.p, b.p, t));
  } else {
    Vec3 p01 = lerp(a.p, a.out, t), p12 = lerp(a.out, b.in, t), p23 = lerp(b.in, b.p, t);
    Vec3 p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
    nk = Knot{lerp(p012, p123, t), p012, p123, KnotType::Bezier};
    // Freeze automatic handles of the neighbours so the curve keeps its exact shape.
    if (a.type == KnotType::Corner || a.type == KnotType::Smooth) a.type = KnotType::BezierCorner;
    if (b.type == KnotType::Corner || b.type == KnotType::Smooth) b.type = KnotType::BezierCorner;
    a.out = p01;
    b.in = p23;
  }
  sp.knots.insert(sp.knots.begin() + ia + 1, nk);
  sp.updateAutoHandles();
  return KnotRef{seg.spline, ia + 1};
}

namespace {

void cornerReplace(BezierShape& s, const std::vector<KnotRef>& knots, float amount, bool round) {
  std::map<int, std::vector<int>> bySpline;
  for (const auto& r : knots) bySpline[r.spline].push_back(r.knot);
  for (auto& [si, ks] : bySpline) {
    if (si < 0 || si >= static_cast<int>(s.splines.size())) continue;
    std::sort(ks.rbegin(), ks.rend());
    ks.erase(std::unique(ks.begin(), ks.end()), ks.end());
    Spline& sp = s.splines[si];
    for (int k : ks) {
      int n = static_cast<int>(sp.knots.size());
      if (k < 0 || k >= n || n < 2) continue;
      if (!sp.closed && (k == 0 || k == n - 1)) continue;
      Vec3 dp, dn;
      float lp, ln;
      knotDirs(sp, k, dp, dn, lp, ln);
      if (length2(dp) < 0.5f || length2(dn) < 0.5f) continue;
      float cosT = clampf(dot(dp, dn), -1.0f, 1.0f);
      float theta = std::acos(cosT);
      if (theta > kPi - 1e-3f || theta < 1e-3f) continue;
      Vec3 p = sp.knots[k].p;
      float d = round ? amount / std::tan(theta * 0.5f) : amount;
      float r = amount;
      float dmax = 0.5f * std::min(lp, ln);
      if (d > dmax) {
        d = dmax;
        r = d * std::tan(theta * 0.5f);
      }
      if (d <= kEps) continue;
      Vec3 A = p + dp * d, B = p + dn * d;
      Knot ka, kb;
      if (round) {
        float phi = kPi - theta;
        float kh = 4.0f / 3.0f * std::tan(phi / 4.0f) * r;
        ka = Knot{A, A + dp * ((lp - d) / 3.0f), A - dp * kh, KnotType::BezierCorner};
        kb = Knot{B, B - dn * kh, B + dn * ((ln - d) / 3.0f), KnotType::BezierCorner};
      } else {
        ka = cornerKnot(A);
        kb = cornerKnot(B);
      }
      sp.knots[k] = ka;
      sp.knots.insert(sp.knots.begin() + k + 1, kb);
    }
    sp.updateAutoHandles();
  }
}

}  // namespace

void fillet(BezierShape& s, const std::vector<KnotRef>& knots, float radius) { cornerReplace(s, knots, radius, true); }

void chamfer(BezierShape& s, const std::vector<KnotRef>& knots, float distance) {
  cornerReplace(s, knots, distance, false);
}

void breakKnots(BezierShape& s, const std::vector<KnotRef>& knots) {
  std::map<int, std::set<int>> bySpline;
  for (const auto& r : knots)
    if (r.spline >= 0 && r.spline < static_cast<int>(s.splines.size())) bySpline[r.spline].insert(r.knot);
  std::vector<Spline> result;
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    Spline sp = s.splines[si];
    auto it = bySpline.find(si);
    if (it == bySpline.end()) {
      result.push_back(sp);
      continue;
    }
    std::set<int> breaks = it->second;
    int n = static_cast<int>(sp.knots.size());
    if (sp.closed) {
      int first = *breaks.begin();
      std::rotate(sp.knots.begin(), sp.knots.begin() + first, sp.knots.end());
      sp.knots.push_back(sp.knots.front());
      sp.closed = false;
      std::set<int> shifted;
      for (int b : breaks)
        if (b != first) shifted.insert((b - first + n) % n);
      breaks = shifted;
      n = static_cast<int>(sp.knots.size());
    }
    Spline cur;
    cur.closed = false;
    for (int k = 0; k < n; ++k) {
      cur.knots.push_back(sp.knots[k]);
      if (breaks.count(k) && k > 0 && k < n - 1) {
        result.push_back(cur);
        cur = Spline();
        cur.knots.push_back(sp.knots[k]);
      }
    }
    if (cur.knots.size() >= 2) result.push_back(cur);
  }
  s.splines = std::move(result);
  s.updateAutoHandles();
}

void deleteKnots(BezierShape& s, const std::vector<KnotRef>& knots) {
  std::map<int, std::set<int>> bySpline;
  for (const auto& r : knots) bySpline[r.spline].insert(r.knot);
  std::vector<Spline> result;
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    Spline sp = s.splines[si];
    auto it = bySpline.find(si);
    if (it != bySpline.end())
      for (auto k = it->second.rbegin(); k != it->second.rend(); ++k)
        if (*k >= 0 && *k < static_cast<int>(sp.knots.size())) sp.knots.erase(sp.knots.begin() + *k);
    if (sp.knots.size() < 2) continue;
    if (sp.knots.size() < 3) sp.closed = false;
    sp.updateAutoHandles();
    result.push_back(std::move(sp));
  }
  s.splines = std::move(result);
}

int weld(BezierShape& s, const std::vector<KnotRef>& knots, float threshold) {
  std::vector<Vec3> targets;
  for (const auto& r : knots)
    if (r.spline >= 0 && r.spline < static_cast<int>(s.splines.size()) && r.knot >= 0 &&
        r.knot < static_cast<int>(s.splines[r.spline].knots.size()))
      targets.push_back(s.splines[r.spline].knots[r.knot].p);
  auto isTarget = [&](const Vec3& p) {
    for (const auto& t : targets)
      if (length2(t - p) < 1e-10f) return true;
    return false;
  };
  int welds = 0;
  for (bool changed = true; changed;) {
    changed = false;
    // Interior neighbours within the threshold.
    for (auto& sp : s.splines) {
      for (int k = 0; k + 1 < static_cast<int>(sp.knots.size()) && sp.knots.size() > 2; ++k) {
        Knot& a = sp.knots[k];
        Knot& b = sp.knots[k + 1];
        if (!isTarget(a.p) || !isTarget(b.p) || length(a.p - b.p) > threshold) continue;
        Vec3 mid = (a.p + b.p) * 0.5f;
        Knot m{mid, a.in + (mid - a.p), b.out + (mid - b.p), a.type};
        sp.knots[k] = m;
        sp.knots.erase(sp.knots.begin() + k + 1);
        targets.push_back(mid);
        ++welds;
        changed = true;
        break;
      }
      if (changed) break;
    }
    if (changed) continue;
    // Ends of open splines.
    for (int i = 0; i < static_cast<int>(s.splines.size()) && !changed; ++i) {
      Spline& a = s.splines[i];
      if (a.closed || a.knots.size() < 2) continue;
      // Same spline: close it.
      if (isTarget(a.knots.front().p) && isTarget(a.knots.back().p) && a.knots.size() > 2 &&
          length(a.knots.front().p - a.knots.back().p) <= threshold) {
        Vec3 mid = (a.knots.front().p + a.knots.back().p) * 0.5f;
        a.knots.front().in = a.knots.back().in + (mid - a.knots.back().p);
        translateKnot(a.knots.front(), mid - a.knots.front().p);
        a.knots.front().in = a.knots.front().in;
        a.knots.pop_back();
        a.closed = true;
        ++welds;
        changed = true;
        break;
      }
      for (int j = 0; j < static_cast<int>(s.splines.size()) && !changed; ++j) {
        if (j == i) continue;
        Spline& b = s.splines[j];
        if (b.closed || b.knots.size() < 2) continue;
        for (int ea = 0; ea < 2 && !changed; ++ea)
          for (int eb = 0; eb < 2 && !changed; ++eb) {
            Knot& ka = ea ? a.knots.back() : a.knots.front();
            Knot& kb = eb ? b.knots.back() : b.knots.front();
            if (!isTarget(ka.p) || !isTarget(kb.p) || length(ka.p - kb.p) > threshold) continue;
            Spline A = a, B = b;
            if (!ea) reverseSpline(A);  // A must end at the weld point
            if (eb) reverseSpline(B);   // B must start at it
            Vec3 mid = (A.knots.back().p + B.knots.front().p) * 0.5f;
            Knot m{mid, A.knots.back().in + (mid - A.knots.back().p), B.knots.front().out + (mid - B.knots.front().p),
                   KnotType::BezierCorner};
            A.knots.back() = m;
            A.knots.insert(A.knots.end(), B.knots.begin() + 1, B.knots.end());
            targets.push_back(mid);
            s.splines[i] = A;
            s.splines.erase(s.splines.begin() + j);
            ++welds;
            changed = true;
          }
      }
    }
  }
  s.updateAutoHandles();
  return welds;
}

void fuse(BezierShape& s, const std::vector<KnotRef>& knots) {
  Vec3 c;
  int count = 0;
  for (const auto& r : knots) {
    c += s.splines[r.spline].knots[r.knot].p;
    ++count;
  }
  if (count < 2) return;
  c /= static_cast<float>(count);
  for (const auto& r : knots) {
    Knot& k = s.splines[r.spline].knots[r.knot];
    translateKnot(k, c - k.p);
  }
  s.updateAutoHandles();
}

bool connect(BezierShape& s, KnotRef a, KnotRef b) {
  if (a.spline < 0 || b.spline < 0 || a.spline >= static_cast<int>(s.splines.size()) ||
      b.spline >= static_cast<int>(s.splines.size()))
    return false;
  Spline& sa = s.splines[a.spline];
  Spline& sb = s.splines[b.spline];
  auto isEnd = [](const Spline& sp, int k) { return !sp.closed && (k == 0 || k == static_cast<int>(sp.knots.size()) - 1); };
  if (!isEnd(sa, a.knot) || !isEnd(sb, b.knot)) return false;
  if (a.spline == b.spline) {
    if (a.knot == b.knot || sa.knots.size() < 3) return false;
    sa.closed = true;
    sa.updateAutoHandles();
    return true;
  }
  Spline A = sa, B = sb;
  if (a.knot == 0) reverseSpline(A);
  if (b.knot != 0) reverseSpline(B);
  A.knots.back().out = A.knots.back().p;
  B.knots.front().in = B.knots.front().p;
  A.knots.insert(A.knots.end(), B.knots.begin(), B.knots.end());
  A.updateAutoHandles();
  int keep = std::min(a.spline, b.spline), drop = std::max(a.spline, b.spline);
  s.splines[keep] = A;
  s.splines.erase(s.splines.begin() + drop);
  return true;
}

void makeFirst(BezierShape& s, KnotRef r) {
  Spline& sp = s.splines[r.spline];
  int n = static_cast<int>(sp.knots.size());
  if (r.knot <= 0 || r.knot >= n) return;
  if (sp.closed) std::rotate(sp.knots.begin(), sp.knots.begin() + r.knot, sp.knots.end());
  else if (r.knot == n - 1) reverseSpline(sp);
}

int crossInsert(BezierShape& s, int ia, int ib) {
  if (ia == ib || ia < 0 || ib < 0 || ia >= static_cast<int>(s.splines.size()) ||
      ib >= static_cast<int>(s.splines.size()))
    return 0;
  const int steps = 16;
  Dense A = densify(s.splines[ia], steps), B = densify(s.splines[ib], steps);
  std::vector<std::pair<int, float>> hitsA, hitsB;
  for (size_t i = 0; i + 1 < A.pts.size(); ++i)
    for (size_t j = 0; j + 1 < B.pts.size(); ++j) {
      float ta, tb;
      if (shape2d::segmentIntersect(xy(A.pts[i]), xy(A.pts[i + 1]), xy(B.pts[j]), xy(B.pts[j + 1]), ta, tb)) {
        hitsA.push_back(paramAt(A, i, ta));
        hitsB.push_back(paramAt(B, j, tb));
      }
    }
  auto apply = [&](int si, std::vector<std::pair<int, float>> hits) {
    std::sort(hits.begin(), hits.end());
    // Insert from the end so earlier segment indices stay valid; adjust t within a segment.
    for (size_t h = hits.size(); h-- > 0;) {
      int seg = hits[h].first;
      float t = hits[h].second;
      if (t <= 1e-4f || t >= 1.0f - 1e-4f) continue;
      refine(s, SegmentRef{si, seg}, t);
    }
  };
  apply(ia, hitsA);
  apply(ib, hitsB);
  return static_cast<int>(hitsA.size());
}

void divideSegments(BezierShape& s, const std::vector<SegmentRef>& segsIn, int divisions) {
  if (divisions < 1) return;
  std::vector<SegmentRef> segs = segsIn;
  std::sort(segs.rbegin(), segs.rend());
  segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
  for (SegmentRef seg : segs) {
    if (seg.spline < 0 || seg.spline >= static_cast<int>(s.splines.size())) continue;
    if (seg.knot < 0 || seg.knot >= s.splines[seg.spline].segmentCount()) continue;
    SegmentRef cur = seg;
    for (int i = 0; i < divisions; ++i) {
      KnotRef nk = refine(s, cur, 1.0f / static_cast<float>(divisions + 1 - i));
      cur = SegmentRef{nk.spline, nk.knot};
    }
  }
}

namespace {

// Splits spline sp into runs of segments where keep(seg) is true.
std::vector<Spline> runs(const Spline& sp, const std::function<bool(int)>& keep) {
  std::vector<Spline> out;
  int segs = sp.segmentCount();
  int n = static_cast<int>(sp.knots.size());
  bool all = true;
  for (int i = 0; i < segs; ++i) all = all && keep(i);
  if (all) {
    out.push_back(sp);
    return out;
  }
  int start = 0;
  if (sp.closed)
    for (int i = 0; i < segs; ++i)
      if (!keep(i)) {
        start = (i + 1) % segs;
        break;
      }
  Spline cur;
  for (int k = 0; k < segs; ++k) {
    int i = (start + k) % segs;
    if (keep(i)) {
      if (cur.knots.empty()) cur.knots.push_back(sp.knots[i]);
      cur.knots.push_back(sp.knots[(i + 1) % n]);
    } else if (!cur.knots.empty()) {
      out.push_back(cur);
      cur = Spline();
    }
  }
  if (!cur.knots.empty()) out.push_back(cur);
  for (auto& r : out) {
    r.closed = false;
    r.knots.front().in = r.knots.front().p;
    r.knots.back().out = r.knots.back().p;
    r.updateAutoHandles();
  }
  return out;
}

}  // namespace

void deleteSegments(BezierShape& s, const std::vector<SegmentRef>& segs) {
  std::vector<Spline> result;
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    std::set<int> del;
    for (const auto& r : segs)
      if (r.spline == si) del.insert(r.knot);
    if (del.empty()) {
      result.push_back(s.splines[si]);
      continue;
    }
    for (auto& r : runs(s.splines[si], [&](int i) { return !del.count(i); })) result.push_back(std::move(r));
  }
  s.splines = std::move(result);
}

BezierShape extractSegments(const BezierShape& s, const std::vector<SegmentRef>& segs) {
  BezierShape out;
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    std::set<int> sel;
    for (const auto& r : segs)
      if (r.spline == si) sel.insert(r.knot);
    if (sel.empty()) continue;
    for (auto& r : runs(s.splines[si], [&](int i) { return sel.count(i) != 0; })) out.splines.push_back(std::move(r));
  }
  return out;
}

BezierShape extractSplines(const BezierShape& s, const std::vector<int>& splines) {
  BezierShape out;
  for (int i : splines)
    if (i >= 0 && i < static_cast<int>(s.splines.size())) out.splines.push_back(s.splines[i]);
  return out;
}

void deleteSplines(BezierShape& s, const std::vector<int>& splinesIn) {
  std::vector<int> splines = splinesIn;
  std::sort(splines.rbegin(), splines.rend());
  splines.erase(std::unique(splines.begin(), splines.end()), splines.end());
  for (int i : splines)
    if (i >= 0 && i < static_cast<int>(s.splines.size())) s.splines.erase(s.splines.begin() + i);
}

void outline(BezierShape& s, const std::vector<int>& splines, float width, bool center) {
  std::vector<Spline> added;
  for (int i : splines) {
    if (i < 0 || i >= static_cast<int>(s.splines.size())) continue;
    Spline sp = s.splines[i];
    if (sp.knots.size() < 2 || width == 0.0f) continue;
    float a = center ? -width * 0.5f : 0.0f, b = center ? width * 0.5f : width;
    Spline inner = a == 0.0f ? sp : offsetSpline(sp, a);
    Spline outer = offsetSpline(sp, b);
    if (sp.closed) {
      s.splines[i] = inner;
      added.push_back(outer);
    } else {
      reverseSpline(outer);
      inner.knots.back().out = inner.knots.back().p;
      outer.knots.front().in = outer.knots.front().p;
      outer.knots.back().out = outer.knots.back().p;
      inner.knots.front().in = inner.knots.front().p;
      inner.knots.insert(inner.knots.end(), outer.knots.begin(), outer.knots.end());
      inner.closed = true;
      inner.updateAutoHandles();
      s.splines[i] = inner;
    }
  }
  s.splines.insert(s.splines.end(), added.begin(), added.end());
}

bool booleanSplines(BezierShape& s, int target, const std::vector<int>& operands, shape2d::BoolOp op) {
  if (target < 0 || target >= static_cast<int>(s.splines.size()) || !s.splines[target].closed) return false;
  auto poly = [&](int i) {
    shape2d::Polygon p;
    for (const auto& v : s.splines[i].polyline(10, true)) p.push_back(xy(v));
    return p;
  };
  std::vector<shape2d::Polygon> a = {poly(target)}, b;
  std::vector<int> used;
  for (int i : operands)
    if (i != target && i >= 0 && i < static_cast<int>(s.splines.size()) && s.splines[i].closed) {
      b.push_back(poly(i));
      used.push_back(i);
    }
  if (b.empty()) return false;
  float z = s.splines[target].knots.empty() ? 0.0f : s.splines[target].knots[0].p.z;
  std::vector<shape2d::Polygon> res = shape2d::boolean(a, b, op);
  std::vector<Spline> made;
  for (const auto& p : res) {
    std::vector<Vec3> pts;
    for (const auto& v : simplifyCollinear(p)) pts.emplace_back(v.x, v.y, z);
    if (pts.size() >= 3) made.push_back(polylineSpline(pts, true));
  }
  used.push_back(target);
  deleteSplines(s, used);
  int insertAt = std::min(target, static_cast<int>(s.splines.size()));
  s.splines.insert(s.splines.begin() + insertAt, made.begin(), made.end());
  return true;
}

void mirror(BezierShape& s, const std::vector<int>& splines, int axis, bool copy) {
  std::vector<Spline> added;
  for (int i : splines) {
    if (i < 0 || i >= static_cast<int>(s.splines.size())) continue;
    Spline sp = s.splines[i];
    Aabb b;
    for (const auto& k : sp.knots) b.expand(k.p);
    Vec3 c = b.center();
    auto flip = [&](Vec3 p) {
      if (axis == 0 || axis == 2) p.x = 2 * c.x - p.x;
      if (axis == 1 || axis == 2) p.y = 2 * c.y - p.y;
      return p;
    };
    for (auto& k : sp.knots) {
      k.p = flip(k.p);
      k.in = flip(k.in);
      k.out = flip(k.out);
    }
    if (copy) added.push_back(sp);
    else s.splines[i] = sp;
  }
  s.splines.insert(s.splines.end(), added.begin(), added.end());
}

void reverse(BezierShape& s, const std::vector<int>& splines) {
  for (int i : splines)
    if (i >= 0 && i < static_cast<int>(s.splines.size())) reverseSpline(s.splines[i]);
}

void close(BezierShape& s, const std::vector<int>& splines) {
  for (int i : splines)
    if (i >= 0 && i < static_cast<int>(s.splines.size()) && s.splines[i].knots.size() >= 3) {
      s.splines[i].closed = true;
      s.splines[i].updateAutoHandles();
    }
}

namespace {

// Arc-length positions where spline `si` crosses other splines.
std::vector<float> crossings(const BezierShape& s, int si, const Dense& d) {
  std::vector<float> out;
  for (int j = 0; j < static_cast<int>(s.splines.size()); ++j) {
    if (j == si) continue;
    Dense o = densify(s.splines[j], 12);
    for (size_t a = 0; a + 1 < d.pts.size(); ++a)
      for (size_t b = 0; b + 1 < o.pts.size(); ++b) {
        float ta, tb;
        if (shape2d::segmentIntersect(xy(d.pts[a]), xy(d.pts[a + 1]), xy(o.pts[b]), xy(o.pts[b + 1]), ta, tb))
          out.push_back(d.arc[a] + (d.arc[a + 1] - d.arc[a]) * ta);
      }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<Vec3> cutPolyline(const Dense& d, float from, float to) {
  std::vector<Vec3> pts;
  auto at = [&](float l) {
    size_t j = 0;
    while (j + 1 < d.arc.size() && d.arc[j + 1] < l) ++j;
    if (j + 1 >= d.arc.size()) return d.pts.back();
    float f = (l - d.arc[j]) / std::max(1e-12f, d.arc[j + 1] - d.arc[j]);
    return lerp(d.pts[j], d.pts[j + 1], f);
  };
  pts.push_back(at(from));
  for (size_t j = 0; j < d.arc.size(); ++j)
    if (d.arc[j] > from && d.arc[j] < to) pts.push_back(d.pts[j]);
  pts.push_back(at(to));
  return pts;
}

}  // namespace

bool trim(BezierShape& s, int si, const Vec3& pick) {
  if (si < 0 || si >= static_cast<int>(s.splines.size())) return false;
  Spline sp = s.splines[si];
  Dense d = densify(sp, 12);
  if (d.pts.size() < 2) return false;
  std::vector<float> cuts = crossings(s, si, d);
  if (cuts.empty()) return false;
  // Arc length of the pick point.
  float best = kInfinity, pickL = 0;
  for (size_t j = 0; j + 1 < d.pts.size(); ++j) {
    Vec2 a = xy(d.pts[j]), b = xy(d.pts[j + 1]), p = xy(pick);
    Vec2 ab = b - a;
    float l2 = ab.x * ab.x + ab.y * ab.y;
    float t = l2 > 0 ? clampf(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
    Vec2 q = a + ab * t;
    float dist = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
    if (dist < best) {
      best = dist;
      pickL = d.arc[j] + (d.arc[j + 1] - d.arc[j]) * t;
    }
  }
  float total = d.arc.back();
  float lo = -1, hi = -1;
  for (float c : cuts) {
    if (c < pickL) lo = c;
    if (c > pickL && hi < 0) hi = c;
  }
  std::vector<Spline> pieces;
  if (sp.closed) {
    if (lo < 0 && hi < 0) return false;
    if (lo < 0) lo = cuts.back() - total;  // wrap around
    if (hi < 0) hi = cuts.front() + total;
    // Keep from hi forward to lo (+ total).
    std::vector<Vec3> pts;
    float start = hi, end = lo + total;
    for (float l = start;;) {
      float segEnd = std::min(end, std::floor(l / total) * total + total);
      Dense dd = d;
      float base = std::floor(l / total) * total;
      std::vector<Vec3> part = cutPolyline(dd, l - base, segEnd - base);
      pts.insert(pts.end(), part.begin() + (pts.empty() ? 0 : 1), part.end());
      if (segEnd >= end - 1e-6f) break;
      l = segEnd;
    }
    if (pts.size() >= 2) pieces.push_back(polylineSpline(pts, false));
  } else {
    if (lo >= 0) pieces.push_back(polylineSpline(cutPolyline(d, 0.0f, lo), false));
    if (hi >= 0) pieces.push_back(polylineSpline(cutPolyline(d, hi, total), false));
  }
  s.splines.erase(s.splines.begin() + si);
  s.splines.insert(s.splines.begin() + si, pieces.begin(), pieces.end());
  return true;
}

bool extend(BezierShape& s, int si, const Vec3& pick) {
  if (si < 0 || si >= static_cast<int>(s.splines.size())) return false;
  Spline& sp = s.splines[si];
  if (sp.closed || sp.knots.size() < 2) return false;
  auto d2 = [](const Vec2& v) { return v.x * v.x + v.y * v.y; };
  bool atEnd = d2(xy(pick) - xy(sp.knots.back().p)) < d2(xy(pick) - xy(sp.knots.front().p));
  Dense d = densify(sp, 12);
  Vec3 endP = atEnd ? d.pts.back() : d.pts.front();
  Vec3 prevP = atEnd ? d.pts[d.pts.size() - 2] : d.pts[1];
  Vec2 dir = xy(endP) - xy(prevP);
  float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
  if (dl < 1e-9f) return false;
  dir = dir / dl;
  Aabb b = s.bounds();
  float far = std::max(1.0f, length(b.extent()) * 4.0f);
  Vec2 a0 = xy(endP), a1 = a0 + dir * far;
  float bestT = kInfinity;
  for (int j = 0; j < static_cast<int>(s.splines.size()); ++j) {
    if (j == si) continue;
    Dense o = densify(s.splines[j], 12);
    for (size_t k = 0; k + 1 < o.pts.size(); ++k) {
      float ta, tb;
      if (shape2d::segmentIntersect(a0, a1, xy(o.pts[k]), xy(o.pts[k + 1]), ta, tb) && ta * far > 1e-4f)
        bestT = std::min(bestT, ta);
    }
  }
  if (bestT == kInfinity) return false;
  Vec2 hit = a0 + dir * (bestT * far);
  Knot nk = cornerKnot(Vec3(hit.x, hit.y, endP.z));
  if (atEnd) {
    sp.knots.back().out = sp.knots.back().p;
    sp.knots.push_back(nk);
  } else {
    sp.knots.front().in = sp.knots.front().p;
    sp.knots.insert(sp.knots.begin(), nk);
  }
  sp.updateAutoHandles();
  return true;
}

BezierShape normalized(const BezierShape& s, float segLength) {
  BezierShape out;
  segLength = std::max(segLength, 1e-3f);
  for (const auto& sp : s.splines) {
    Dense d = densify(sp, 24);
    if (d.pts.size() < 2) continue;
    float total = d.arc.back();
    int count = std::max(sp.closed ? 3 : 2, static_cast<int>(std::round(total / segLength)) + (sp.closed ? 0 : 1));
    Spline r;
    r.closed = sp.closed;
    int samples = sp.closed ? count : count;
    for (int i = 0; i < samples; ++i) {
      float l = sp.closed ? total * i / count : total * i / (count - 1);
      size_t j = 0;
      while (j + 1 < d.arc.size() && d.arc[j + 1] < l) ++j;
      Vec3 p = j + 1 < d.arc.size()
                   ? lerp(d.pts[j], d.pts[j + 1], (l - d.arc[j]) / std::max(1e-12f, d.arc[j + 1] - d.arc[j]))
                   : d.pts.back();
      r.knots.push_back(Knot{p, p, p, KnotType::Smooth});
    }
    r.updateAutoHandles();
    out.splines.push_back(std::move(r));
  }
  return out;
}

BezierShape filletAll(const BezierShape& s, float radius, bool chamferInstead) {
  BezierShape out = s;
  std::vector<KnotRef> all;
  for (int i = 0; i < static_cast<int>(s.splines.size()); ++i)
    for (int k = 0; k < static_cast<int>(s.splines[i].knots.size()); ++k)
      if (s.splines[i].knots[k].type == KnotType::Corner || s.splines[i].knots[k].type == KnotType::BezierCorner)
        all.push_back({i, k});
  if (chamferInstead) chamfer(out, all, radius);
  else fillet(out, all, radius);
  return out;
}

bool nearestSegment(const BezierShape& s, const Vec3& p, SegmentRef& seg, float& t, float& dist) {
  dist = kInfinity;
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    const Spline& sp = s.splines[si];
    for (int g = 0; g < sp.segmentCount(); ++g) {
      const int steps = 24;
      Vec3 prev = sp.evaluate(g, 0.0f);
      for (int i = 1; i <= steps; ++i) {
        Vec3 cur = sp.evaluate(g, static_cast<float>(i) / steps);
        Vec2 a = xy(prev), b = xy(cur), q = xy(p);
        Vec2 ab = b - a;
        float l2 = ab.x * ab.x + ab.y * ab.y;
        float f = l2 > 0 ? clampf(((q.x - a.x) * ab.x + (q.y - a.y) * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
        Vec2 c = a + ab * f;
        float dd = std::sqrt((c.x - q.x) * (c.x - q.x) + (c.y - q.y) * (c.y - q.y));
        if (dd < dist) {
          dist = dd;
          seg = SegmentRef{si, g};
          t = (static_cast<float>(i - 1) + f) / steps;
        }
        prev = cur;
      }
    }
  }
  return dist < kInfinity;
}

}  // namespace veraa::splineops
