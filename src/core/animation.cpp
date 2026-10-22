// The animation scene module: value paths, keying, constraints, IK and scene evaluation.
#include "animation.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "ik.h"
#include "motionhook.h"
#include "registry.h"

namespace veraa {

namespace {

const std::vector<std::string> kChannels = {"pos.x", "pos.y", "pos.z", "rot.x",  "rot.y",
                                            "rot.z", "scale.x", "scale.y", "scale.z"};
const char* const kChannelNames[] = {"X Position", "Y Position", "Z Position", "X Rotation", "Y Rotation",
                                     "Z Rotation", "X Scale",    "Y Scale",    "Z Scale"};

bool parseUInt(const std::string& s, uint32_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > 0xffffffffull) return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Splits "key.r" into key and component index.
void splitComponent(const std::string& in, std::string& key, int& comp) {
  comp = -1;
  key = in;
  if (in.size() > 2 && in[in.size() - 2] == '.') {
    char c = in.back();
    if (c == 'r' || c == 'x') comp = 0;
    else if (c == 'g' || c == 'y') comp = 1;
    else if (c == 'b' || c == 'z') comp = 2;
    if (comp >= 0) key = in.substr(0, in.size() - 2);
  }
}

bool readParam(const ParamBlock& p, const std::string& key, int comp, double& v) {
  const ParamDef* d = p.def(key);
  if (!d) return false;
  switch (d->type) {
    case ParamType::Float: v = p.getFloat(key); return comp < 0;
    case ParamType::Int:
    case ParamType::Enum: v = p.getInt(key); return comp < 0;
    case ParamType::Bool: v = p.getBool(key) ? 1.0 : 0.0; return comp < 0;
    case ParamType::Color:
      if (comp < 0 || comp > 2) return false;
      v = p.getColor(key)[comp];
      return true;
    default: return false;
  }
}

bool writeParam(ParamBlock& p, const std::string& key, int comp, double v) {
  const ParamDef* d = p.def(key);
  if (!d) return false;
  if (d->type == ParamType::Color) {
    if (comp < 0 || comp > 2) return false;
    Vec3 c = p.getColor(key);
    c[comp] = static_cast<float>(v);
    return p.setColor(key, c);
  }
  if (d->type == ParamType::String || d->type == ParamType::File) return false;
  if (d->type == ParamType::Float) {
    // Avoid churn from float round-off.
    if (std::fabs(p.getFloat(key) - v) < 1e-7 * std::max(1.0, std::fabs(v))) return false;
  }
  return p.setNumber(key, v);
}

void appendParamPaths(const ParamBlock& p, const std::string& prefix, std::vector<std::string>& out) {
  for (const auto& d : p.defs()) {
    switch (d.type) {
      case ParamType::Float:
      case ParamType::Int:
      case ParamType::Enum:
      case ParamType::Bool: out.push_back(prefix + d.key); break;
      case ParamType::Color:
        out.push_back(prefix + d.key + ".r");
        out.push_back(prefix + d.key + ".g");
        out.push_back(prefix + d.key + ".b");
        break;
      default: break;
    }
  }
}

std::string paramLabel(const ParamBlock& p, const std::string& key, int comp) {
  const ParamDef* d = p.def(key);
  std::string l = d ? d->label : key;
  if (comp >= 0) l += comp == 0 ? " R" : (comp == 1 ? " G" : " B");
  return l;
}

thread_local int g_valueDepth = 0;

}  // namespace

// ---------------------------------------------------------------------------------------------
// Value paths

ValuePath ValuePath::parse(const std::string& path) {
  ValuePath v;
  auto slash = path.find('/');
  if (slash == std::string::npos) return v;
  std::string head = path.substr(0, slash), rest = path.substr(slash + 1);
  if (head == "render") {
    v.kind = Kind::RenderParam;
    splitComponent(rest, v.key, v.component);
    return v;
  }
  if (head.rfind("material:", 0) == 0) {
    if (!parseUInt(head.substr(9), v.id)) return v;
    v.kind = Kind::MaterialParam;
    splitComponent(rest, v.key, v.component);
    return v;
  }
  if (head.rfind("node:", 0) != 0 || !parseUInt(head.substr(5), v.id)) return v;
  for (size_t i = 0; i < kChannels.size(); ++i)
    if (rest == kChannels[i]) {
      v.kind = Kind::Transform;
      v.channel = static_cast<int>(i);
      return v;
    }
  auto s2 = rest.find('/');
  if (s2 == std::string::npos) return v;
  std::string section = rest.substr(0, s2), tail = rest.substr(s2 + 1);
  if (section == "object") {
    v.kind = Kind::ObjectParam;
    splitComponent(tail, v.key, v.component);
  } else if (section.rfind("mod:", 0) == 0) {
    uint32_t idx;
    if (!parseUInt(section.substr(4), idx)) return v;
    v.kind = Kind::ModifierParam;
    v.index = static_cast<int>(idx);
    splitComponent(tail, v.key, v.component);
  } else if (section.rfind("c:", 0) == 0) {
    uint32_t idx;
    if (!parseUInt(section.substr(2), idx)) return v;
    v.kind = Kind::Constraint;
    v.index = static_cast<int>(idx);
    v.key = tail;
  } else if (section == "custom") {
    v.kind = Kind::Custom;
    v.key = tail;
  }
  return v;
}

std::string Animation::nodePath(uint32_t id, const std::string& channel) {
  return "node:" + std::to_string(id) + "/" + channel;
}

const std::vector<std::string>& Animation::transformChannels() { return kChannels; }

Animation* Animation::of(const Scene& scene) { return static_cast<Animation*>(scene.module("animation")); }

bool Animation::readValue(const Scene& s, const std::string& path, double& v) {
  ValuePath p = ValuePath::parse(path);
  switch (p.kind) {
    case ValuePath::Kind::Invalid: return false;
    case ValuePath::Kind::RenderParam: return readParam(s.renderSettings, p.key, p.component, v);
    case ValuePath::Kind::MaterialParam: {
      const Material* m = s.findMaterial(p.id);
      return m && readParam(m->params, p.key, p.component, v);
    }
    default: break;
  }
  const Node* n = s.findNode(p.id);
  if (!n) return false;
  switch (p.kind) {
    case ValuePath::Kind::Transform: {
      const Transform& t = n->transform;
      int c = p.channel % 3;
      v = p.channel < 3 ? t.position[c] : (p.channel < 6 ? t.rotation[c] : t.scale[c]);
      return true;
    }
    case ValuePath::Kind::ObjectParam: return n->object && readParam(n->object->params, p.key, p.component, v);
    case ValuePath::Kind::ModifierParam:
      return p.index >= 0 && p.index < static_cast<int>(n->modifiers.size()) && n->modifiers[p.index] &&
             readParam(n->modifiers[p.index]->params, p.key, p.component, v);
    case ValuePath::Kind::Constraint:
    case ValuePath::Kind::Custom: {
      const Animation* a = of(s);
      if (!a) return false;
      auto it = a->nodes.find(p.id);
      if (it == a->nodes.end()) return false;
      if (p.kind == ValuePath::Kind::Custom) {
        for (const auto& c : it->second.custom)
          if (c.name == p.key) {
            v = c.value;
            return true;
          }
        return false;
      }
      if (p.index < 0 || p.index >= static_cast<int>(it->second.constraints.size())) return false;
      const Constraint& c = it->second.constraints[p.index];
      if (p.key == "percent") {
        v = c.percent;
        return true;
      }
      if (p.key.size() > 1 && p.key[0] == 'w') {
        uint32_t k;
        if (parseUInt(p.key.substr(1), k) && k < c.targets.size()) {
          v = c.targets[k].weight;
          return true;
        }
      }
      return false;
    }
    default: return false;
  }
}

bool Animation::writeValue(Scene& s, const std::string& path, double v) {
  ValuePath p = ValuePath::parse(path);
  switch (p.kind) {
    case ValuePath::Kind::Invalid: return false;
    case ValuePath::Kind::RenderParam: return writeParam(s.renderSettings, p.key, p.component, v);
    case ValuePath::Kind::MaterialParam: {
      Material* m = s.findMaterial(p.id);
      return m && writeParam(m->params, p.key, p.component, v);
    }
    default: break;
  }
  Node* n = s.findNode(p.id);
  if (!n) return false;
  switch (p.kind) {
    case ValuePath::Kind::Transform: {
      Transform& t = n->transform;
      int c = p.channel % 3;
      float& slot = p.channel < 3 ? t.position[c] : (p.channel < 6 ? t.rotation[c] : t.scale[c]);
      float fv = static_cast<float>(v);
      if (slot == fv) return false;
      slot = fv;
      return true;
    }
    case ValuePath::Kind::ObjectParam: {
      if (!n->object) return false;
      bool changed = writeParam(n->object->params, p.key, p.component, v);
      if (changed) n->invalidate();
      return changed;
    }
    case ValuePath::Kind::ModifierParam: {
      if (p.index < 0 || p.index >= static_cast<int>(n->modifiers.size()) || !n->modifiers[p.index]) return false;
      bool changed = writeParam(n->modifiers[p.index]->params, p.key, p.component, v);
      if (changed) n->invalidate();
      return changed;
    }
    case ValuePath::Kind::Constraint:
    case ValuePath::Kind::Custom: {
      Animation* a = of(s);
      if (!a) return false;
      auto it = a->nodes.find(p.id);
      if (it == a->nodes.end()) return false;
      if (p.kind == ValuePath::Kind::Custom) {
        for (auto& c : it->second.custom)
          if (c.name == p.key) {
            double nv = c.type == 2 ? (v != 0.0 ? 1.0 : 0.0) : (c.type == 1 ? std::round(v) : v);
            nv = std::clamp(nv, std::min(c.minValue, c.maxValue), std::max(c.minValue, c.maxValue));
            if (c.value == nv) return false;
            c.value = nv;
            return true;
          }
        return false;
      }
      if (p.index < 0 || p.index >= static_cast<int>(it->second.constraints.size())) return false;
      Constraint& c = it->second.constraints[p.index];
      if (p.key == "percent") {
        if (c.percent == v) return false;
        c.percent = v;
        return true;
      }
      if (p.key.size() > 1 && p.key[0] == 'w') {
        uint32_t k;
        if (parseUInt(p.key.substr(1), k) && k < c.targets.size() && c.targets[k].weight != v) {
          c.targets[k].weight = v;
          return true;
        }
      }
      return false;
    }
    default: return false;
  }
}

std::vector<std::string> Animation::animatablePaths(const Scene& s, uint32_t nodeId) const {
  std::vector<std::string> out;
  const Node* n = s.findNode(nodeId);
  if (!n) return out;
  std::string base = "node:" + std::to_string(nodeId) + "/";
  for (const auto& c : kChannels) out.push_back(base + c);
  if (n->object) appendParamPaths(n->object->params, base + "object/", out);
  for (size_t i = 0; i < n->modifiers.size(); ++i)
    if (n->modifiers[i]) appendParamPaths(n->modifiers[i]->params, base + "mod:" + std::to_string(i) + "/", out);
  auto it = nodes.find(nodeId);
  if (it != nodes.end()) {
    for (size_t i = 0; i < it->second.constraints.size(); ++i) {
      std::string cp = base + "c:" + std::to_string(i) + "/";
      if (it->second.constraints[i].type == ConstraintType::Path) out.push_back(cp + "percent");
      for (size_t k = 0; k < it->second.constraints[i].targets.size(); ++k) out.push_back(cp + "w" + std::to_string(k));
    }
    for (const auto& c : it->second.custom) out.push_back(base + "custom/" + c.name);
  }
  return out;
}

std::string Animation::displayName(const Scene& s, const std::string& path) {
  ValuePath p = ValuePath::parse(path);
  switch (p.kind) {
    case ValuePath::Kind::RenderParam: return "Render > " + paramLabel(s.renderSettings, p.key, p.component);
    case ValuePath::Kind::MaterialParam: {
      const Material* m = s.findMaterial(p.id);
      return m ? m->name + " > " + paramLabel(m->params, p.key, p.component) : path;
    }
    case ValuePath::Kind::Invalid: return path;
    default: break;
  }
  const Node* n = s.findNode(p.id);
  std::string nn = n ? n->name : "node " + std::to_string(p.id);
  switch (p.kind) {
    case ValuePath::Kind::Transform: return nn + " > " + kChannelNames[p.channel];
    case ValuePath::Kind::ObjectParam:
      return nn + " > " + (n && n->object ? paramLabel(n->object->params, p.key, p.component) : p.key);
    case ValuePath::Kind::ModifierParam:
      if (n && p.index < static_cast<int>(n->modifiers.size()) && n->modifiers[p.index])
        return nn + " > " + modifierDisplayName(n->modifiers[p.index]->type()) + " > " +
               paramLabel(n->modifiers[p.index]->params, p.key, p.component);
      return path;
    case ValuePath::Kind::Constraint:
      return nn + " > Constraint " + std::to_string(p.index + 1) + " > " +
             (p.key == "percent" ? std::string("Percent") : "Weight " + p.key.substr(1));
    case ValuePath::Kind::Custom: return nn + " > " + p.key;
    default: return path;
  }
}

// ---------------------------------------------------------------------------------------------
// Tracks and keys

Track* Animation::findTrack(const std::string& path) {
  auto it = tracks.find(path);
  return it == tracks.end() ? nullptr : &it->second;
}

const Track* Animation::findTrack(const std::string& path) const {
  auto it = tracks.find(path);
  return it == tracks.end() ? nullptr : &it->second;
}

Track& Animation::activeTrack(const std::string& path) {
  if (activeLayer > 0 && activeLayer <= static_cast<int>(layers.size())) return layers[activeLayer - 1].tracks[path];
  return tracks[path];
}

bool Animation::isAnimated(const std::string& path) const {
  auto live = [](const Track& t) { return t.hasKeys() || t.procedural() || t.type == ControllerType::List; };
  if (const Track* t = findTrack(path); t && live(*t)) return true;
  for (const auto& l : layers) {
    auto it = l.tracks.find(path);
    if (it != l.tracks.end() && live(it->second)) return true;
  }
  for (const auto& w : wires)
    if (w.to == path) return true;
  for (const auto& r : reactions)
    if (r.driven == path && !r.points.empty()) return true;
  ValuePath p = ValuePath::parse(path);
  if (p.kind == ValuePath::Kind::Transform && p.channel >= 3 && p.channel < 6) {
    auto it = nodes.find(p.id);
    if (it != nodes.end() && it->second.rotationController == 1 && !it->second.rotationKeys.empty()) return true;
  }
  return false;
}

std::vector<std::string> Animation::nodeTrackPaths(uint32_t id) const {
  std::string prefix = "node:" + std::to_string(id) + "/";
  std::set<std::string> out;
  for (const auto& kv : tracks)
    if (kv.first.compare(0, prefix.size(), prefix) == 0) out.insert(kv.first);
  for (const auto& l : layers)
    for (const auto& kv : l.tracks)
      if (kv.first.compare(0, prefix.size(), prefix) == 0) out.insert(kv.first);
  return std::vector<std::string>(out.begin(), out.end());
}

bool Animation::nodeAnimated(uint32_t id) const {
  for (const auto& p : nodeTrackPaths(id))
    if (isAnimated(p)) return true;
  auto it = nodes.find(id);
  return it != nodes.end() && (!it->second.constraints.empty() || !it->second.rotationKeys.empty());
}

void Animation::setRotationKey(uint32_t id, double frame, const Vec3& eulerDeg) {
  NodeAnimation& na = nodes[id];
  Quat q = Quat::fromEulerDeg(eulerDeg);
  for (auto& k : na.rotationKeys)
    if (std::fabs(k.time - frame) < 1e-3) {
      k.q = q;
      touch();
      return;
    }
  RotationKey k;
  k.time = frame;
  k.q = q;
  na.rotationKeys.push_back(k);
  std::sort(na.rotationKeys.begin(), na.rotationKeys.end(),
            [](const RotationKey& a, const RotationKey& b) { return a.time < b.time; });
  touch();
}

void Animation::setKey(const Scene& s, const std::string& path, double frame, double value, double startFrame,
                       double originalValue) {
  ValuePath p = ValuePath::parse(path);
  if (p.kind == ValuePath::Kind::Invalid) return;
  double staticValue = 0.0;
  if (!readValue(s, path, staticValue)) return;
  double original = std::isnan(originalValue) ? staticValue : originalValue;

  // TCB rotation controller: keys whole rotations.
  if (p.kind == ValuePath::Kind::Transform && p.channel >= 3 && p.channel < 6) {
    auto it = nodes.find(p.id);
    if (it != nodes.end() && it->second.rotationController == 1) {
      const Node* n = s.findNode(p.id);
      if (!n) return;
      if (it->second.rotationKeys.empty() && std::fabs(frame - startFrame) > 1e-6) {
        Vec3 orig = n->transform.rotation;
        orig[p.channel - 3] = static_cast<float>(original);
        setRotationKey(p.id, startFrame, orig);
      }
      Vec3 e = n->transform.rotation;
      e[p.channel - 3] = static_cast<float>(value);
      setRotationKey(p.id, frame, e);
      return;
    }
  }

  auto layerSum = [&](double f, int skip) {
    double sum = 0.0;
    for (size_t i = 0; i < layers.size(); ++i) {
      if (static_cast<int>(i) + 1 == skip || !layers[i].enabled) continue;
      auto it = layers[i].tracks.find(path);
      if (it != layers[i].tracks.end() && (it->second.hasKeys() || it->second.procedural()))
        sum += layers[i].weight / 100.0 * it->second.evaluate(f);
    }
    return sum;
  };

  if (activeLayer <= 0 || activeLayer > static_cast<int>(layers.size())) {
    Track& tr = tracks[path];
    bool isNew = !tr.hasKeys() && !tr.procedural() && tr.type != ControllerType::List;
    if (isNew && std::fabs(frame - startFrame) > 1e-6) tr.setKey(startFrame, original - layerSum(startFrame, 0));
    tr.setKey(frame, value - layerSum(frame, 0));
  } else {
    // Keep a base track so the base value doesn't drift with the layer's contribution.
    Track& base = tracks[path];
    if (!base.hasKeys() && !base.procedural() && base.type != ControllerType::List) base.setKey(startFrame, original);
    AnimLayer& L = layers[activeLayer - 1];
    Track& tr = L.tracks[path];
    bool isNew = !tr.hasKeys() && !tr.procedural();
    if (isNew && std::fabs(frame - startFrame) > 1e-6) tr.setKey(startFrame, 0.0);
    double w = L.weight / 100.0;
    if (std::fabs(w) < 1e-9) w = 1.0;
    double baseValue = base.evaluate(frame) + layerSum(frame, activeLayer);
    tr.setKey(frame, (value - baseValue) / w);
  }
  touch();
}

bool Animation::deleteKey(const std::string& path, double frame) {
  bool any = false;
  ValuePath p = ValuePath::parse(path);
  if (p.kind == ValuePath::Kind::Transform && p.channel >= 3 && p.channel < 6) {
    auto it = nodes.find(p.id);
    if (it != nodes.end() && it->second.rotationController == 1) {
      auto& rk = it->second.rotationKeys;
      size_t before = rk.size();
      rk.erase(std::remove_if(rk.begin(), rk.end(), [&](const RotationKey& k) { return std::fabs(k.time - frame) < 1e-3; }),
               rk.end());
      any = rk.size() != before;
    }
  }
  Track& tr = activeTrack(path);
  any |= tr.deleteKeyAt(frame);
  if (!tr.hasKeys() && !tr.procedural() && tr.type != ControllerType::List) {
    if (activeLayer > 0 && activeLayer <= static_cast<int>(layers.size())) layers[activeLayer - 1].tracks.erase(path);
    else tracks.erase(path);
  }
  if (any) touch();
  return any;
}

void Animation::deleteNodeKeysAt(uint32_t id, double frame) {
  for (const auto& p : nodeTrackPaths(id)) deleteKey(p, frame);
  auto it = nodes.find(id);
  if (it != nodes.end()) {
    auto& rk = it->second.rotationKeys;
    rk.erase(std::remove_if(rk.begin(), rk.end(), [&](const RotationKey& k) { return std::fabs(k.time - frame) < 1e-3; }),
             rk.end());
  }
  touch();
}

void Animation::deleteNodeAnimation(uint32_t id) {
  std::string prefix = "node:" + std::to_string(id) + "/";
  auto eraseIn = [&](std::map<std::string, Track>& m) {
    for (auto it = m.begin(); it != m.end();)
      it = it->first.compare(0, prefix.size(), prefix) == 0 ? m.erase(it) : std::next(it);
  };
  eraseIn(tracks);
  for (auto& l : layers) eraseIn(l.tracks);
  auto it = nodes.find(id);
  if (it != nodes.end()) {
    it->second.rotationKeys.clear();
    it->second.constraints.clear();
    it->second.rotationController = 0;
    if (it->second.empty()) nodes.erase(it);
  }
  wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& w) { return w.to.compare(0, prefix.size(), prefix) == 0; }),
              wires.end());
  touch();
}

void Animation::keyTimesForNodes(const std::vector<uint32_t>& ids, std::vector<double>& out) const {
  for (uint32_t id : ids) {
    for (const auto& p : nodeTrackPaths(id)) {
      if (const Track* t = findTrack(p)) t->keyTimes(out);
      for (const auto& l : layers) {
        auto it = l.tracks.find(p);
        if (it != l.tracks.end()) it->second.keyTimes(out);
      }
    }
    auto it = nodes.find(id);
    if (it != nodes.end())
      for (const auto& k : it->second.rotationKeys) out.push_back(k.time);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::fabs(a - b) < 1e-4; }), out.end());
}

void Animation::moveNodeKeys(const std::vector<uint32_t>& ids, double from, double to) {
  if (std::fabs(from - to) < 1e-9) return;
  auto moveIn = [&](Track& t) {
    std::function<void(Track&)> rec = [&](Track& tr) {
      int src = tr.keyIndexAt(from);
      if (src >= 0) {
        Key k = tr.keys[src];
        tr.keys.erase(tr.keys.begin() + src);
        int dst = tr.keyIndexAt(to);
        if (dst >= 0) tr.keys.erase(tr.keys.begin() + dst);
        k.time = to;
        tr.keys.push_back(k);
        tr.sortKeys();
      }
      for (auto& s : tr.subs) rec(s);
    };
    rec(t);
  };
  for (uint32_t id : ids) {
    for (const auto& p : nodeTrackPaths(id)) {
      if (Track* t = findTrack(p)) moveIn(*t);
      for (auto& l : layers) {
        auto it = l.tracks.find(p);
        if (it != l.tracks.end()) moveIn(it->second);
      }
    }
    auto it = nodes.find(id);
    if (it != nodes.end()) {
      auto& rk = it->second.rotationKeys;
      for (auto& k : rk)
        if (std::fabs(k.time - from) < 1e-3) k.time = to;
      std::sort(rk.begin(), rk.end(), [](const RotationKey& a, const RotationKey& b) { return a.time < b.time; });
    }
  }
  touch();
}

void Animation::purge(const Scene& s) {
  auto alive = [&](const std::string& path) {
    ValuePath p = ValuePath::parse(path);
    if (p.kind == ValuePath::Kind::Invalid) return false;
    if (p.kind == ValuePath::Kind::RenderParam) return true;
    if (p.kind == ValuePath::Kind::MaterialParam) return s.findMaterial(p.id) != nullptr;
    return s.findNode(p.id) != nullptr;
  };
  size_t before = tracks.size() + nodes.size() + wires.size() + reactions.size();
  for (auto it = tracks.begin(); it != tracks.end();) it = alive(it->first) ? std::next(it) : tracks.erase(it);
  for (auto& l : layers)
    for (auto it = l.tracks.begin(); it != l.tracks.end();) it = alive(it->first) ? std::next(it) : l.tracks.erase(it);
  for (auto it = nodes.begin(); it != nodes.end();) it = s.findNode(it->first) ? std::next(it) : nodes.erase(it);
  wires.erase(std::remove_if(wires.begin(), wires.end(), [&](const Wire& w) { return !alive(w.from) || !alive(w.to); }),
              wires.end());
  reactions.erase(std::remove_if(reactions.begin(), reactions.end(),
                                 [&](const Reaction& r) { return !alive(r.driver) || !alive(r.driven); }),
                  reactions.end());
  if (before != tracks.size() + nodes.size() + wires.size() + reactions.size()) touch();
}

void Animation::copyNodeAnimation(uint32_t from, uint32_t to) {
  std::string src = "node:" + std::to_string(from) + "/", dst = "node:" + std::to_string(to) + "/";
  auto copyIn = [&](std::map<std::string, Track>& m) {
    std::vector<std::pair<std::string, Track>> add;
    for (const auto& kv : m)
      if (kv.first.compare(0, src.size(), src) == 0) add.emplace_back(dst + kv.first.substr(src.size()), kv.second);
    for (auto& a : add) m[a.first] = std::move(a.second);
  };
  copyIn(tracks);
  for (auto& l : layers) copyIn(l.tracks);
  auto it = nodes.find(from);
  if (it != nodes.end()) nodes[to] = it->second;
  touch();
}

TrackEnv Animation::env(const Scene& s) const {
  TrackEnv e;
  e.fps = s.time.fps;
  e.startFrame = s.time.startFrame;
  e.endFrame = s.time.endFrame;
  e.trackValue = [this, &s](const std::string& path, double frame) { return valueAt(s, path, frame); };
  return e;
}

namespace {

double evalRotationComponent(const NodeAnimation& na, double frame, int comp) {
  // Squad interpolation with Kochanek-Bartels tangents in quaternion log space.
  const auto& k = na.rotationKeys;
  size_t n = k.size();
  if (n == 0) return 0.0;
  if (n == 1 || frame <= k.front().time) return k.front().q.toEulerDeg()[comp];
  if (frame >= k.back().time) return k.back().q.toEulerDeg()[comp];
  size_t i = 0;
  while (i + 1 < n && k[i + 1].time < frame) ++i;
  // Align signs along the sequence for shortest paths.
  std::vector<Quat> q(n);
  q[0] = k[0].q;
  for (size_t j = 1; j < n; ++j) {
    q[j] = k[j].q;
    if (q[j].dot(q[j - 1]) < 0) q[j] = {-q[j].x, -q[j].y, -q[j].z, -q[j].w};
  }
  auto tangentsAt = [&](size_t j, Quat& aOut, Quat& bIn) {
    Quat g1 = j > 0 ? Quat::log(q[j - 1].conjugate() * q[j]) : Quat{0, 0, 0, 0};
    Quat g2 = j + 1 < n ? Quat::log(q[j].conjugate() * q[j + 1]) : g1;
    if (j == 0) g1 = g2;
    double T = k[j].tension, C = k[j].continuity, B = k[j].bias;
    double a1 = (1 - T) * (1 + C) * (1 + B) / 2, a2 = (1 - T) * (1 - C) * (1 - B) / 2;
    double b1 = (1 - T) * (1 - C) * (1 + B) / 2, b2 = (1 - T) * (1 + C) * (1 - B) / 2;
    Quat tOut{a1 * g1.x + a2 * g2.x, a1 * g1.y + a2 * g2.y, a1 * g1.z + a2 * g2.z, 0};
    Quat tIn{b1 * g1.x + b2 * g2.x, b1 * g1.y + b2 * g2.y, b1 * g1.z + b2 * g2.z, 0};
    aOut = q[j] * Quat::exp({(tOut.x - g2.x) * 0.5, (tOut.y - g2.y) * 0.5, (tOut.z - g2.z) * 0.5, 0});
    bIn = q[j] * Quat::exp({(g1.x - tIn.x) * 0.5, (g1.y - tIn.y) * 0.5, (g1.z - tIn.z) * 0.5, 0});
  };
  Quat a0, b0, a1, b1;
  tangentsAt(i, a0, b0);
  tangentsAt(i + 1, a1, b1);
  double dt = std::max(k[i + 1].time - k[i].time, 1e-9);
  double s = (frame - k[i].time) / dt;
  double ea = std::clamp(k[i].easeFrom / 50.0, 0.0, 1.0), eb = std::clamp(k[i + 1].easeTo / 50.0, 0.0, 1.0);
  if (ea > 0 || eb > 0) {
    double s2 = s * s, s3 = s2 * s;
    s = (s3 - 2 * s2 + s) * (1 - ea) + (-2 * s3 + 3 * s2) + (s3 - s2) * (1 - eb);
  }
  Quat r = Quat::slerp(Quat::slerp(q[i], q[i + 1], s), Quat::slerp(a0, b1, s), 2 * s * (1 - s));
  return r.toEulerDeg()[comp];
}

}  // namespace

double Animation::valueAt(const Scene& s, const std::string& path, double frame) const {
  if (g_valueDepth > 32) {
    double v = 0.0;
    readValue(s, path, v);
    return v;
  }
  struct Depth {
    Depth() { ++g_valueDepth; }
    ~Depth() { --g_valueDepth; }
  } depth;

  // Wires and reactions drive their targets completely.
  for (const auto& w : wires)
    if (w.to == path) {
      Expression e;
      if (!e.parse(w.exprTo)) break;
      double x = valueAt(s, w.from, frame);
      return e.evaluate([&](const std::string& n) { return n == "x" ? x : (n == "F" ? frame : 0.0); });
    }
  for (const auto& r : reactions)
    if (r.driven == path && !r.points.empty()) {
      double x = valueAt(s, r.driver, frame);
      auto pts = r.points;
      std::sort(pts.begin(), pts.end());
      if (x <= pts.front().first) return pts.front().second;
      if (x >= pts.back().first) return pts.back().second;
      for (size_t i = 0; i + 1 < pts.size(); ++i)
        if (x <= pts[i + 1].first) {
          double t = (x - pts[i].first) / std::max(pts[i + 1].first - pts[i].first, 1e-12);
          if (r.smooth) t = t * t * (3 - 2 * t);
          return pts[i].second + (pts[i + 1].second - pts[i].second) * t;
        }
    }

  ValuePath p = ValuePath::parse(path);
  TrackEnv e = env(s);
  double v = 0.0;
  bool base = false;
  if (p.kind == ValuePath::Kind::Transform && p.channel >= 3 && p.channel < 6) {
    auto it = nodes.find(p.id);
    if (it != nodes.end() && it->second.rotationController == 1 && !it->second.rotationKeys.empty()) {
      v = evalRotationComponent(it->second, frame, p.channel - 3);
      base = true;
    }
  }
  if (!base) {
    const Track* t = findTrack(path);
    if (t && (t->hasKeys() || t->procedural() || t->type == ControllerType::List)) v = t->evaluate(frame, &e);
    else readValue(s, path, v);
  }
  for (const auto& l : layers) {
    if (!l.enabled) continue;
    auto it = l.tracks.find(path);
    if (it != l.tracks.end() && (it->second.hasKeys() || it->second.procedural()))
      v += l.weight / 100.0 * it->second.evaluate(frame, &e);
  }
  return v;
}

// ---------------------------------------------------------------------------------------------
// Geometry helpers for constraints

std::vector<Vec3> shapeWorldPolylineAt(const Scene& scene, uint32_t nodeId, double frame) {
  std::vector<Vec3> out;
  const Node* n = scene.findNode(nodeId);
  if (!n) return out;
  const auto& lines = n->shapeLines();
  if (lines.empty() || lines[0].size() < 2) return out;
  Mat4 m = nodeWorldMatrixAt(scene, nodeId, frame) * n->objectOffset.matrix();
  for (const auto& p : lines[0]) out.push_back(m.transformPoint(p));
  return out;
}

void polylinePointAt(const std::vector<Vec3>& line, double percent, bool constantVelocity, Vec3& point, Vec3& tangent) {
  point = line.empty() ? Vec3() : line[0];
  tangent = Vec3(1, 0, 0);
  if (line.size() < 2) return;
  double f = std::clamp(percent / 100.0, 0.0, 1.0);
  size_t segs = line.size() - 1;
  if (constantVelocity) {
    std::vector<double> cum(line.size(), 0.0);
    for (size_t i = 1; i < line.size(); ++i) cum[i] = cum[i - 1] + length(line[i] - line[i - 1]);
    double target = f * cum.back();
    size_t i = 0;
    while (i + 1 < segs && cum[i + 1] < target) ++i;
    double segLen = std::max(cum[i + 1] - cum[i], 1e-12);
    double u = std::clamp((target - cum[i]) / segLen, 0.0, 1.0);
    point = lerp(line[i], line[i + 1], static_cast<float>(u));
    tangent = normalize(line[i + 1] - line[i]);
    return;
  }
  double x = f * segs;
  size_t i = std::min(static_cast<size_t>(x), segs - 1);
  double u = x - static_cast<double>(i);
  point = lerp(line[i], line[i + 1], static_cast<float>(u));
  tangent = normalize(line[i + 1] - line[i]);
}

namespace {

// Rotation matrix whose column `axis` is `dir` and whose up column follows `up`.
Mat4 basisAlong(const Vec3& dirIn, int axis, const Vec3& upIn) {
  Vec3 dir = normalize(dirIn);
  Vec3 up = upIn - dir * dot(upIn, dir);
  if (length2(up) < 1e-10f) {
    Vec3 alt = std::fabs(dir.z) < 0.9f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    up = alt - dir * dot(alt, dir);
  }
  up = normalize(up);
  Vec3 c[3];
  if (axis == 0) {
    c[0] = dir;
    c[2] = up;
    c[1] = cross(c[2], c[0]);
  } else if (axis == 1) {
    c[1] = dir;
    c[2] = up;
    c[0] = cross(c[1], c[2]);
  } else {
    c[2] = dir;
    c[1] = up;
    c[0] = cross(c[1], c[2]);
  }
  Mat4 m;
  for (int k = 0; k < 3; ++k) {
    m.m[0][k] = c[k].x;
    m.m[1][k] = c[k].y;
    m.m[2][k] = c[k].z;
  }
  return m;
}

struct Decomposed {
  Vec3 pos;
  Mat4 rot;   // pure rotation
  Vec3 scale; // column lengths (signed X for mirrored)
};

Decomposed decompose(const Mat4& w) {
  Decomposed d;
  d.pos = w.translation();
  Vec3 c0 = w.column(0), c1 = w.column(1), c2 = w.column(2);
  d.scale = Vec3(length(c0), length(c1), length(c2));
  if (w.determinant3x3() < 0) d.scale.x = -d.scale.x;
  Vec3 n0 = std::fabs(d.scale.x) > 1e-12f ? c0 / d.scale.x : Vec3(1, 0, 0);
  Vec3 n1 = d.scale.y > 1e-12f ? c1 / d.scale.y : Vec3(0, 1, 0);
  Vec3 n2 = d.scale.z > 1e-12f ? c2 / d.scale.z : Vec3(0, 0, 1);
  for (int r = 0; r < 3; ++r) {
    d.rot.m[r][0] = n0[r];
    d.rot.m[r][1] = n1[r];
    d.rot.m[r][2] = n2[r];
  }
  return d;
}

Mat4 compose(const Decomposed& d) { return Mat4::translate(d.pos) * d.rot * Mat4::scale(d.scale); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Evaluator: world matrices at a time, honouring parents, constraints and IK.

class AnimEvaluator {
 public:
  AnimEvaluator(const Animation& a, const Scene& s, double frame) : a_(a), s_(s), frame_(frame) {
    // IK chains: bones rotated by a solver (start .. end-1).
    for (const auto& up : s.nodes) {
      const Node& n = *up;
      if (!n.object || n.object->type() != "IKChain") continue;
      const ParamBlock& p = n.object->params;
      if (!p.getBool("enabled")) continue;
      std::vector<uint32_t> chain = chainBones(static_cast<uint32_t>(p.getInt("startBone")),
                                               static_cast<uint32_t>(p.getInt("endBone")));
      if (chain.size() < 2) continue;
      for (size_t i = 0; i + 1 < chain.size(); ++i) boneChain_[chain[i]] = n.id;
    }
  }

  std::vector<uint32_t> chainBones(uint32_t start, uint32_t end) const {
    std::vector<uint32_t> out;
    const Node* n = s_.findNode(end);
    for (int guard = 0; n && guard < 256; ++guard) {
      out.push_back(n->id);
      if (n->id == start) {
        std::reverse(out.begin(), out.end());
        return out;
      }
      n = n->parent();
    }
    return {};
  }

  bool usesIk(uint32_t id) const { return boneChain_.count(id) != 0; }

  Transform localTransform(const Node& n) {
    Transform t = n.transform;
    std::string base = "node:" + std::to_string(n.id) + "/";
    for (int c = 0; c < 9; ++c) {
      std::string path = base + kChannels[c];
      if (!a_.isAnimated(path)) continue;
      float v = static_cast<float>(a_.valueAt(s_, path, frame_));
      if (c < 3) t.position[c] = v;
      else if (c < 6) t.rotation[c - 3] = v;
      else t.scale[c - 6] = v;
    }
    return t;
  }

  Mat4 world(uint32_t id) {
    auto cached = cache_.find(id);
    if (cached != cache_.end()) return cached->second;
    const Node* n = s_.findNode(id);
    if (!n) return Mat4::identity();
    if (visiting_.count(id)) return n->worldMatrix();  // dependency cycle: fall back to the current pose
    visiting_.insert(id);

    auto chain = boneChain_.find(id);
    if (chain != boneChain_.end()) solveChain(chain->second);
    Transform local;
    auto ik = ikLocal_.find(id);
    if (ik != ikLocal_.end()) local = ik->second;
    else local = localTransform(*n);
    Mat4 parent = n->parentId ? world(n->parentId) : Mat4::identity();
    Mat4 w = parent * local.matrix();

    auto na = a_.nodes.find(id);
    if (na != a_.nodes.end())
      for (size_t i = 0; i < na->second.constraints.size(); ++i)
        w = applyConstraint(id, static_cast<int>(i), na->second.constraints[i], w, true);

    visiting_.erase(id);
    cache_[id] = w;
    return w;
  }

  // Applies one constraint to the world matrix w.
  Mat4 applyConstraint(uint32_t id, int index, const Constraint& c, const Mat4& w, bool withOffset) {
    Decomposed d = decompose(w);
    std::string cpath = "node:" + std::to_string(id) + "/c:" + std::to_string(index) + "/";
    auto weightOf = [&](size_t k) {
      std::string p = cpath + "w" + std::to_string(k);
      return a_.isAnimated(p) ? a_.valueAt(s_, p, frame_) : c.targets[k].weight;
    };
    double total = 0.0;
    switch (c.type) {
      case ConstraintType::Path: {
        double pct = a_.isAnimated(cpath + "percent") ? a_.valueAt(s_, cpath + "percent", frame_) : c.percent;
        if (c.loop) {
          pct = std::fmod(pct, 100.0);
          if (pct < 0) pct += 100.0;
        }
        Vec3 pos, tan;
        for (size_t k = 0; k < c.targets.size(); ++k) {
          std::vector<Vec3> line = worldPolyline(c.targets[k].nodeId);
          if (line.size() < 2) continue;
          double wk = weightOf(k);
          Vec3 p, t;
          polylinePointAt(line, pct, c.constantVelocity, p, t);
          pos += p * static_cast<float>(wk);
          tan += t * static_cast<float>(wk);
          total += wk;
        }
        if (total <= 0) return w;
        d.pos = pos / static_cast<float>(total);
        if (c.follow && length2(tan) > 1e-12f) {
          Vec3 dir = normalize(tan) * (c.flip ? -1.0f : 1.0f);
          Mat4 r = basisAlong(dir, c.axis, Vec3(0, 0, 1));
          if (c.bank && !c.targets.empty()) {
            std::vector<Vec3> line = worldPolyline(c.targets[0].nodeId);
            double delta = 0.5 + c.smoothness * 5.0;
            Vec3 p0, t0, p1, t1;
            polylinePointAt(line, std::max(0.0, pct - delta), c.constantVelocity, p0, t0);
            polylinePointAt(line, std::min(100.0, pct + delta), c.constantVelocity, p1, t1);
            float turn = std::atan2(cross(t0, t1).z, dot(Vec3(t0.x, t0.y, 0), Vec3(t1.x, t1.y, 0)));
            float roll = static_cast<float>(-c.bankAmount) * turn * 2.0f;
            r = Mat4::rotateAxis(dir, roll) * r;
          }
          d.rot = r;
          if (withOffset && c.keepOffset) d.rot = d.rot * c.rotationOffset.toMatrix();
        }
        if (withOffset && c.keepOffset) d.pos += c.positionOffset;
        return compose(d);
      }
      case ConstraintType::Position: {
        Vec3 pos;
        for (size_t k = 0; k < c.targets.size(); ++k) {
          double wk = weightOf(k);
          pos += world(c.targets[k].nodeId).translation() * static_cast<float>(wk);
          total += wk;
        }
        if (total <= 0) return w;
        d.pos = pos / static_cast<float>(total);
        if (withOffset && c.keepOffset) d.pos += c.positionOffset;
        return compose(d);
      }
      case ConstraintType::Orientation: {
        Quat acc{0, 0, 0, 0};
        Quat ref;
        bool first = true;
        for (size_t k = 0; k < c.targets.size(); ++k) {
          double wk = weightOf(k);
          Quat q = Quat::fromMatrix(world(c.targets[k].nodeId));
          if (first) {
            ref = q;
            first = false;
          } else if (q.dot(ref) < 0) {
            q = {-q.x, -q.y, -q.z, -q.w};
          }
          acc = {acc.x + q.x * wk, acc.y + q.y * wk, acc.z + q.z * wk, acc.w + q.w * wk};
          total += wk;
        }
        if (total <= 0) return w;
        d.rot = acc.normalized().toMatrix();
        if (withOffset && c.keepOffset) d.rot = d.rot * c.rotationOffset.toMatrix();
        return compose(d);
      }
      case ConstraintType::LookAt: {
        Vec3 tp;
        for (size_t k = 0; k < c.targets.size(); ++k) {
          double wk = weightOf(k);
          tp += world(c.targets[k].nodeId).translation() * static_cast<float>(wk);
          total += wk;
        }
        if (total <= 0) return w;
        tp = tp / static_cast<float>(total);
        Vec3 dir = tp - d.pos;
        if (length2(dir) < 1e-12f) return w;
        dir = normalize(dir) * (c.flip ? -1.0f : 1.0f);
        Vec3 up = c.upNode ? normalize(world(c.upNode).column(2)) : Vec3(0, 0, 1);
        d.rot = basisAlong(dir, c.axis, up);
        if (withOffset && c.keepOffset) d.rot = d.rot * c.rotationOffset.toMatrix();
        return compose(d);
      }
      case ConstraintType::Link: {
        if (c.targets.empty()) return w;
        const ConstraintTarget* active = &c.targets.front();
        for (const auto& t : c.targets)
          if (t.frame <= frame_ + 1e-6) active = &t;
        Mat4 pw = active->nodeId ? world(active->nodeId) : Mat4::identity();
        return pw * active->offset;
      }
      case ConstraintType::Attachment: {
        if (c.targets.empty()) return w;
        const ConstraintTarget& t = c.targets.front();
        const Node* tn = s_.findNode(t.nodeId);
        if (!tn) return w;
        const TriMesh& tm = tn->triMesh();
        size_t tris = tm.triangleCount();
        if (tris == 0) return w;
        size_t f = static_cast<size_t>(std::max(0, t.face)) % tris;
        Mat4 m = world(t.nodeId) * tn->objectOffset.matrix();
        Vec3 p0 = m.transformPoint(tm.positions[tm.indices[f * 3]]);
        Vec3 p1 = m.transformPoint(tm.positions[tm.indices[f * 3 + 1]]);
        Vec3 p2 = m.transformPoint(tm.positions[tm.indices[f * 3 + 2]]);
        float u = static_cast<float>(t.baryU), v = static_cast<float>(t.baryV);
        d.pos = p0 * (1 - u - v) + p1 * u + p2 * v;
        if (c.align) {
          Vec3 nrm = normalize(cross(p1 - p0, p2 - p0));
          Vec3 x = normalize(p1 - p0);
          Mat4 r;
          Vec3 y = cross(nrm, x);
          for (int k = 0; k < 3; ++k) {
            r.m[k][0] = x[k];
            r.m[k][1] = y[k];
            r.m[k][2] = nrm[k];
          }
          d.rot = r;
          if (withOffset && c.keepOffset) d.rot = d.rot * c.rotationOffset.toMatrix();
        }
        if (withOffset && c.keepOffset) d.pos += c.positionOffset;
        return compose(d);
      }
      case ConstraintType::Surface: {
        if (c.targets.empty()) return w;
        const Node* tn = s_.findNode(c.targets.front().nodeId);
        if (!tn || !tn->object) return w;
        const ParamBlock& p = tn->object->params;
        std::string type = tn->object->type();
        float u = static_cast<float>(c.u / 100.0), v = static_cast<float>(c.v / 100.0);
        Vec3 pos, nrm(0, 0, 1), tu(1, 0, 0);
        if (type == "Sphere" || type == "GeoSphere") {
          float r = p.getFloat("radius"), lon = u * kTwoPi, lat = (v - 0.5f) * kPi;
          float zOff = p.has("basePivot") && p.getBool("basePivot") ? r : 0.0f;
          nrm = Vec3(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
          pos = nrm * r + Vec3(0, 0, zOff);
          tu = Vec3(-std::sin(lon), std::cos(lon), 0);
        } else if (type == "Cylinder" || type == "Cone" || type == "Tube") {
          float r = p.has("radius") ? p.getFloat("radius") : p.getFloat("radius1");
          float a = u * kTwoPi;
          nrm = Vec3(std::cos(a), std::sin(a), 0);
          pos = nrm * r + Vec3(0, 0, v * p.getFloat("height"));
          tu = Vec3(-std::sin(a), std::cos(a), 0);
        } else if (type == "Torus") {
          float R = p.getFloat("radius1"), r = p.getFloat("radius2"), a = u * kTwoPi, b = v * kTwoPi;
          Vec3 ring(std::cos(a), std::sin(a), 0);
          nrm = ring * std::cos(b) + Vec3(0, 0, std::sin(b));
          pos = ring * R + nrm * r;
          tu = Vec3(-std::sin(a), std::cos(a), 0);
        } else {
          Aabb b = tn->triMesh().bounds;
          if (!b.valid()) return w;
          pos = Vec3(lerpf(b.lo.x, b.hi.x, u), lerpf(b.lo.y, b.hi.y, v), b.hi.z);
        }
        Mat4 m = world(tn->id) * tn->objectOffset.matrix();
        d.pos = m.transformPoint(pos);
        if (c.align) {
          Vec3 n2 = normalize(m.transformDir(nrm)), x = normalize(m.transformDir(tu));
          x = normalize(x - n2 * dot(x, n2));
          Vec3 y = cross(n2, x);
          for (int k = 0; k < 3; ++k) {
            d.rot.m[k][0] = x[k];
            d.rot.m[k][1] = y[k];
            d.rot.m[k][2] = n2[k];
          }
          if (withOffset && c.keepOffset) d.rot = d.rot * c.rotationOffset.toMatrix();
        }
        if (withOffset && c.keepOffset) d.pos += c.positionOffset;
        return compose(d);
      }
    }
    return w;
  }

  std::vector<Vec3> worldPolyline(uint32_t shapeId) {
    std::vector<Vec3> out;
    const Node* n = s_.findNode(shapeId);
    if (!n) return out;
    const auto& lines = n->shapeLines();
    if (lines.empty()) return out;
    Mat4 m = world(shapeId) * n->objectOffset.matrix();
    for (const auto& p : lines[0]) out.push_back(m.transformPoint(p));
    return out;
  }

  void solveChain(uint32_t chainNodeId) {
    if (!solved_.insert(chainNodeId).second) return;
    const Node* cn = s_.findNode(chainNodeId);
    if (!cn) return;
    const ParamBlock& p = cn->object->params;
    std::vector<uint32_t> chain = chainBones(static_cast<uint32_t>(p.getInt("startBone")),
                                             static_cast<uint32_t>(p.getInt("endBone")));
    if (chain.size() < 2) return;
    const Node* start = s_.findNode(chain[0]);
    Mat4 parentW = start->parentId ? world(start->parentId) : Mat4::identity();
    // Unsolved pose from the bones' own tracks.
    std::vector<Transform> locals;
    std::vector<Mat4> worlds;
    Mat4 acc = parentW;
    for (uint32_t id : chain) {
      Transform t = localTransform(*s_.findNode(id));
      locals.push_back(t);
      acc = acc * t.matrix();
      worlds.push_back(acc);
    }
    std::vector<Vec3> joints;
    for (const auto& w : worlds) joints.push_back(w.translation());
    std::vector<Vec3> solvedJ = joints;
    // The goal is the IK chain node itself (cache it to avoid re-entering this solve).
    Transform goalLocal = localTransform(*cn);
    Mat4 goalParent = cn->parentId ? world(cn->parentId) : Mat4::identity();
    Vec3 goal = (goalParent * goalLocal.matrix()).translation();
    if (p.getInt("solver") == 1 && joints.size() == 3) {
      Vec3 pole = cross(joints[1] - joints[0], goal - joints[0]);
      solveTwoBone(solvedJ[0], solvedJ[1], solvedJ[2], goal, pole);
    } else {
      solveFabrik(solvedJ, goal, std::max(1, p.getInt("iterations")), std::max(1e-4f, p.getFloat("threshold")));
    }
    applySwivel(solvedJ, p.getFloat("swivel"));
    // Rotate each bone so it points at its solved child joint.
    Mat4 prevWorld = parentW;
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
      Decomposed d = decompose(worlds[i]);
      Vec3 oldDir = joints[i + 1] - joints[i], newDir = solvedJ[i + 1] - solvedJ[i];
      if (length2(oldDir) > 1e-12f && length2(newDir) > 1e-12f) d.rot = rotationBetween(oldDir, newDir) * d.rot;
      d.pos = solvedJ[i];
      Mat4 newWorld = compose(d);
      Transform local = decomposeTransform(prevWorld.inverse() * newWorld);
      local.scale = locals[i].scale;
      ikLocal_[chain[i]] = local;
      prevWorld = prevWorld * local.matrix();
    }
  }

 private:
  const Animation& a_;
  const Scene& s_;
  double frame_;
  std::unordered_map<uint32_t, Mat4> cache_;
  std::unordered_set<uint32_t> visiting_;
  std::unordered_map<uint32_t, uint32_t> boneChain_;
  std::unordered_map<uint32_t, Transform> ikLocal_;
  std::unordered_set<uint32_t> solved_;
};

Mat4 Animation::worldMatrixAt(const Scene& scene, uint32_t nodeId, double frame) const {
  AnimEvaluator ev(*this, scene, frame);
  return ev.world(nodeId);
}

Transform Animation::trackTransformAt(const Scene& scene, const Node& n, double frame) const {
  AnimEvaluator ev(*this, scene, frame);
  return ev.localTransform(n);
}

Mat4 nodeWorldMatrixAt(const Scene& scene, uint32_t nodeId, double frame) {
  if (const Animation* a = Animation::of(scene)) return a->worldMatrixAt(scene, nodeId, frame);
  const Node* n = scene.findNode(nodeId);
  return n ? n->worldMatrix() : Mat4::identity();
}

int Animation::addConstraint(Scene& s, uint32_t nodeId, Constraint c) {
  NodeAnimation& na = nodes[nodeId];
  int index = static_cast<int>(na.constraints.size());
  if (c.keepOffset && c.type != ConstraintType::Link) {
    Node* n = s.findNode(nodeId);
    if (n) {
      AnimEvaluator ev(*this, s, s.time.currentFrame);
      Mat4 current = ev.world(nodeId);
      Mat4 constrained = ev.applyConstraint(nodeId, index, c, current, false);
      Decomposed dc = decompose(current), dk = decompose(constrained);
      c.positionOffset = dc.pos - dk.pos;
      c.rotationOffset = Quat::fromMatrix(dk.rot.transposed() * dc.rot);
    }
  }
  na.constraints.push_back(std::move(c));
  touch();
  return index;
}

void Animation::addLinkTarget(Scene& s, uint32_t nodeId, uint32_t parentId, double frame) {
  NodeAnimation& na = nodes[nodeId];
  Constraint* link = nullptr;
  for (auto& c : na.constraints)
    if (c.type == ConstraintType::Link) link = &c;
  if (!link) {
    Constraint c;
    c.type = ConstraintType::Link;
    na.constraints.push_back(c);
    link = &na.constraints.back();
  }
  // Offset: the node's current world placement relative to the new parent at that frame.
  Mat4 nodeW = worldMatrixAt(s, nodeId, frame);
  Mat4 parentW = parentId ? worldMatrixAt(s, parentId, frame) : Mat4::identity();
  ConstraintTarget t;
  t.nodeId = parentId;
  t.frame = frame;
  t.offset = parentW.inverse() * nodeW;
  link->targets.erase(std::remove_if(link->targets.begin(), link->targets.end(),
                                     [&](const ConstraintTarget& o) { return std::fabs(o.frame - frame) < 1e-6; }),
                      link->targets.end());
  link->targets.push_back(t);
  std::sort(link->targets.begin(), link->targets.end(),
            [](const ConstraintTarget& a, const ConstraintTarget& b) { return a.frame < b.frame; });
  touch();
}

// ---------------------------------------------------------------------------------------------
// Scene evaluation

Animation::EvalResult Animation::evaluate(Scene& scene, double frame) {
  EvalResult r;
  scene.time.currentFrame = frame;
  AnimEvaluator ev(*this, scene, frame);

  // Paths driven by tracks, layers, wires and reactions.
  std::set<std::string> paths;
  for (const auto& kv : tracks) paths.insert(kv.first);
  for (const auto& l : layers)
    for (const auto& kv : l.tracks) paths.insert(kv.first);
  for (const auto& w : wires) paths.insert(w.to);
  for (const auto& rc : reactions) paths.insert(rc.driven);

  // 1. Constraint and custom attribute values first (they feed transforms and wires).
  for (const auto& path : paths) {
    ValuePath p = ValuePath::parse(path);
    if ((p.kind == ValuePath::Kind::Constraint || p.kind == ValuePath::Kind::Custom) && isAnimated(path))
      writeValue(scene, path, valueAt(scene, path, frame));
  }

  // 2. Transforms of animated, constrained and IK-driven nodes.
  std::set<uint32_t> transformNodes;
  for (const auto& path : paths) {
    ValuePath p = ValuePath::parse(path);
    if (p.kind == ValuePath::Kind::Transform && isAnimated(path)) transformNodes.insert(p.id);
  }
  for (const auto& kv : nodes)
    if (!kv.second.constraints.empty() || (kv.second.rotationController == 1 && !kv.second.rotationKeys.empty()))
      transformNodes.insert(kv.first);
  for (const auto& up : scene.nodes)
    if (ev.usesIk(up->id)) transformNodes.insert(up->id);

  // Compute every world matrix before writing anything (constraints read other nodes' worlds).
  std::vector<std::pair<Node*, Transform>> newLocals;
  for (uint32_t id : transformNodes) {
    Node* n = scene.findNode(id);
    if (!n) continue;
    auto na = nodes.find(id);
    bool constrained = na != nodes.end() && !na->second.constraints.empty();
    Transform t;
    if (constrained || ev.usesIk(id)) {
      Mat4 w = ev.world(id);
      Mat4 pw = n->parentId ? ev.world(n->parentId) : Mat4::identity();
      t = decomposeTransform(pw.inverse() * w);
      // Keep the animated scale exactly (decomposition can flip signs between axes).
      Transform tr = ev.localTransform(*n);
      if (!constrained) t.scale = tr.scale;
    } else {
      t = ev.localTransform(*n);
    }
    newLocals.emplace_back(n, t);
  }
  for (auto& [n, t] : newLocals) {
    const Transform& o = n->transform;
    if (o.position == t.position && o.rotation == t.rotation && o.scale == t.scale) continue;
    n->transform = t;
    r.transformChanged.push_back(n->id);
  }

  // 3. Parameters of objects, modifiers, materials and render settings.
  std::set<uint32_t> geomSet, otherSet;
  for (const auto& path : paths) {
    ValuePath p = ValuePath::parse(path);
    if (p.kind == ValuePath::Kind::Transform || p.kind == ValuePath::Kind::Constraint ||
        p.kind == ValuePath::Kind::Custom || p.kind == ValuePath::Kind::Invalid)
      continue;
    if (!isAnimated(path)) continue;
    if (!writeValue(scene, path, valueAt(scene, path, frame))) continue;
    if (p.kind == ValuePath::Kind::MaterialParam) {
      r.materialsChanged = true;
    } else if (p.kind == ValuePath::Kind::RenderParam) {
      r.renderSettingsChanged = true;
    } else if (Node* n = scene.findNode(p.id)) {
      ObjectKind k = n->kind();
      if (k == ObjectKind::Geometry || k == ObjectKind::Shape || p.kind == ValuePath::Kind::ModifierParam)
        geomSet.insert(n->id);
      else
        otherSet.insert(n->id);
    }
  }

  // 4. Time-dependent geometry, and geometry that reads nodes that moved (skin, path deform).
  std::unordered_set<uint32_t> moved(r.transformChanged.begin(), r.transformChanged.end());
  // Children of moved nodes moved too.
  bool grew = true;
  while (grew) {
    grew = false;
    for (const auto& up : scene.nodes)
      if (up->parentId && moved.count(up->parentId) && moved.insert(up->id).second) grew = true;
  }
  for (const auto& up : scene.nodes) {
    Node& n = *up;
    bool dep = n.object && n.object->timeDependent();
    std::vector<uint32_t> refs = n.object ? n.object->referencedNodes() : std::vector<uint32_t>{};
    for (const auto& m : n.modifiers) {
      if (!m) continue;
      dep |= m->timeDependent();
      auto mr = m->referencedNodes();
      refs.insert(refs.end(), mr.begin(), mr.end());
    }
    for (uint32_t ref : refs)
      if (moved.count(ref) || geomSet.count(ref)) dep = true;
    if (dep) {
      n.invalidate();
      geomSet.insert(n.id);
    }
  }
  r.geometryChanged.assign(geomSet.begin(), geomSet.end());
  r.otherChanged.assign(otherSet.begin(), otherSet.end());
  return r;
}

// ---------------------------------------------------------------------------------------------
// Serialization

bool Animation::empty() const {
  if (!tracks.empty() || !wires.empty() || !reactions.empty() || !layers.empty()) return false;
  for (const auto& kv : nodes)
    if (!kv.second.empty()) return false;
  return true;
}

namespace {

json quatToJson(const Quat& q) { return json::array({q.x, q.y, q.z, q.w}); }
Quat quatFromJson(const json& j) {
  if (!j.is_array() || j.size() != 4) return Quat();
  return Quat{j[0].get<double>(), j[1].get<double>(), j[2].get<double>(), j[3].get<double>()}.normalized();
}
json matToJson(const Mat4& m) {
  json a = json::array();
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) a.push_back(m.m[r][c]);
  return a;
}
Mat4 matFromJson(const json& j) {
  Mat4 m;
  if (!j.is_array() || j.size() != 16) return m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.m[r][c] = j[r * 4 + c].get<float>();
  return m;
}

json tracksToJson(const std::map<std::string, Track>& m) {
  json j = json::object();
  for (const auto& kv : m) j[kv.first] = kv.second.toJson();
  return j;
}

std::map<std::string, Track> tracksFromJson(const json& j) {
  std::map<std::string, Track> m;
  if (!j.is_object()) return m;
  for (auto it = j.begin(); it != j.end(); ++it) m[it.key()] = Track::fromJson(it.value());
  return m;
}

}  // namespace

json Animation::toJson() const {
  if (empty()) return json();
  json j;
  j["tracks"] = tracksToJson(tracks);
  json ns = json::object();
  for (const auto& kv : nodes) {
    const NodeAnimation& na = kv.second;
    if (na.empty()) continue;
    json jn;
    if (na.rotationController) jn["rotCtrl"] = na.rotationController;
    if (na.trajectory) jn["trajectory"] = true;
    if (!na.rotationKeys.empty()) {
      json rk = json::array();
      for (const auto& k : na.rotationKeys)
        rk.push_back(json{{"t", k.time},
                          {"q", quatToJson(k.q)},
                          {"tcb", json::array({k.tension, k.continuity, k.bias, k.easeTo, k.easeFrom})}});
      jn["rotKeys"] = std::move(rk);
    }
    if (!na.constraints.empty()) {
      json cs = json::array();
      for (const auto& c : na.constraints) {
        json jc{{"type", static_cast<int>(c.type)},
                {"keepOffset", c.keepOffset},
                {"posOffset", vec3ToJson(c.positionOffset)},
                {"rotOffset", quatToJson(c.rotationOffset)},
                {"percent", c.percent},
                {"follow", c.follow},
                {"bank", c.bank},
                {"loop", c.loop},
                {"constantVelocity", c.constantVelocity},
                {"flip", c.flip},
                {"bankAmount", c.bankAmount},
                {"smoothness", c.smoothness},
                {"axis", c.axis},
                {"upNode", c.upNode},
                {"align", c.align},
                {"u", c.u},
                {"v", c.v}};
        json ts = json::array();
        for (const auto& t : c.targets)
          ts.push_back(json{{"node", t.nodeId},
                            {"weight", t.weight},
                            {"frame", t.frame},
                            {"offset", matToJson(t.offset)},
                            {"face", t.face},
                            {"bu", t.baryU},
                            {"bv", t.baryV}});
        jc["targets"] = std::move(ts);
        cs.push_back(std::move(jc));
      }
      jn["constraints"] = std::move(cs);
    }
    if (!na.custom.empty()) {
      json ca = json::array();
      for (const auto& c : na.custom)
        ca.push_back(json{{"name", c.name},
                          {"type", c.type},
                          {"value", c.value},
                          {"min", c.minValue},
                          {"max", c.maxValue},
                          {"default", c.defaultValue}});
      jn["custom"] = std::move(ca);
    }
    ns[std::to_string(kv.first)] = std::move(jn);
  }
  j["nodes"] = std::move(ns);
  if (!layers.empty()) {
    json ls = json::array();
    for (const auto& l : layers)
      ls.push_back(json{{"name", l.name}, {"weight", l.weight}, {"enabled", l.enabled}, {"tracks", tracksToJson(l.tracks)}});
    j["layers"] = std::move(ls);
    j["activeLayer"] = activeLayer;
  }
  if (!wires.empty()) {
    json ws = json::array();
    for (const auto& w : wires)
      ws.push_back(json{{"from", w.from}, {"to", w.to}, {"exprTo", w.exprTo}, {"exprFrom", w.exprFrom}, {"twoWay", w.twoWay}});
    j["wires"] = std::move(ws);
  }
  if (!reactions.empty()) {
    json rs = json::array();
    for (const auto& r : reactions) {
      json pts = json::array();
      for (const auto& pt : r.points) pts.push_back(json::array({pt.first, pt.second}));
      rs.push_back(json{{"driver", r.driver}, {"driven", r.driven}, {"points", std::move(pts)}, {"smooth", r.smooth}});
    }
    j["reactions"] = std::move(rs);
  }
  return j;
}

void Animation::fromJson(const json& j) {
  tracks.clear();
  nodes.clear();
  layers.clear();
  activeLayer = 0;
  wires.clear();
  reactions.clear();
  touch();
  if (!j.is_object()) return;
  if (j.contains("tracks")) tracks = tracksFromJson(j["tracks"]);
  if (j.contains("nodes") && j["nodes"].is_object())
    for (auto it = j["nodes"].begin(); it != j["nodes"].end(); ++it) {
      uint32_t id;
      if (!parseUInt(it.key(), id)) continue;
      const json& jn = it.value();
      NodeAnimation na;
      na.rotationController = jn.value("rotCtrl", 0);
      na.trajectory = jn.value("trajectory", false);
      if (jn.contains("rotKeys"))
        for (const auto& k : jn["rotKeys"]) {
          RotationKey rk;
          rk.time = k.value("t", 0.0);
          rk.q = quatFromJson(k.value("q", json()));
          if (k.contains("tcb") && k["tcb"].size() == 5) {
            rk.tension = k["tcb"][0].get<double>();
            rk.continuity = k["tcb"][1].get<double>();
            rk.bias = k["tcb"][2].get<double>();
            rk.easeTo = k["tcb"][3].get<double>();
            rk.easeFrom = k["tcb"][4].get<double>();
          }
          na.rotationKeys.push_back(rk);
        }
      if (jn.contains("constraints"))
        for (const auto& jc : jn["constraints"]) {
          Constraint c;
          c.type = static_cast<ConstraintType>(std::clamp(jc.value("type", 2), 0, 6));
          c.keepOffset = jc.value("keepOffset", false);
          c.positionOffset = vec3FromJson(jc.value("posOffset", json()));
          c.rotationOffset = quatFromJson(jc.value("rotOffset", json()));
          c.percent = jc.value("percent", 0.0);
          c.follow = jc.value("follow", false);
          c.bank = jc.value("bank", false);
          c.loop = jc.value("loop", false);
          c.constantVelocity = jc.value("constantVelocity", true);
          c.flip = jc.value("flip", false);
          c.bankAmount = jc.value("bankAmount", 0.5);
          c.smoothness = jc.value("smoothness", 0.5);
          c.axis = jc.value("axis", 0);
          c.upNode = jc.value("upNode", 0u);
          c.align = jc.value("align", true);
          c.u = jc.value("u", 0.0);
          c.v = jc.value("v", 0.0);
          if (jc.contains("targets"))
            for (const auto& jt : jc["targets"]) {
              ConstraintTarget t;
              t.nodeId = jt.value("node", 0u);
              t.weight = jt.value("weight", 100.0);
              t.frame = jt.value("frame", 0.0);
              t.offset = matFromJson(jt.value("offset", json()));
              t.face = jt.value("face", 0);
              t.baryU = jt.value("bu", 1.0 / 3.0);
              t.baryV = jt.value("bv", 1.0 / 3.0);
              c.targets.push_back(t);
            }
          na.constraints.push_back(std::move(c));
        }
      if (jn.contains("custom"))
        for (const auto& jc : jn["custom"])
          na.custom.push_back({jc.value("name", std::string("Attribute")), jc.value("type", 0), jc.value("value", 0.0),
                               jc.value("min", 0.0), jc.value("max", 100.0), jc.value("default", 0.0)});
      nodes[id] = std::move(na);
    }
  if (j.contains("layers"))
    for (const auto& jl : j["layers"]) {
      AnimLayer l;
      l.name = jl.value("name", std::string("Layer"));
      l.weight = jl.value("weight", 100.0);
      l.enabled = jl.value("enabled", true);
      if (jl.contains("tracks")) l.tracks = tracksFromJson(jl["tracks"]);
      layers.push_back(std::move(l));
    }
  activeLayer = std::clamp(j.value("activeLayer", 0), 0, static_cast<int>(layers.size()));
  if (j.contains("wires"))
    for (const auto& jw : j["wires"])
      wires.push_back({jw.value("from", std::string()), jw.value("to", std::string()), jw.value("exprTo", std::string("x")),
                       jw.value("exprFrom", std::string("x")), jw.value("twoWay", false)});
  if (j.contains("reactions"))
    for (const auto& jr : j["reactions"]) {
      Reaction r;
      r.driver = jr.value("driver", std::string());
      r.driven = jr.value("driven", std::string());
      r.smooth = jr.value("smooth", true);
      if (jr.contains("points"))
        for (const auto& pt : jr["points"])
          if (pt.is_array() && pt.size() == 2) r.points.emplace_back(pt[0].get<double>(), pt[1].get<double>());
      reactions.push_back(std::move(r));
    }
}

const char* constraintTypeName(ConstraintType t) {
  switch (t) {
    case ConstraintType::Path: return "Path Constraint";
    case ConstraintType::LookAt: return "Look-At Constraint";
    case ConstraintType::Position: return "Position Constraint";
    case ConstraintType::Orientation: return "Orientation Constraint";
    case ConstraintType::Link: return "Link Constraint";
    case ConstraintType::Attachment: return "Attachment Constraint";
    case ConstraintType::Surface: return "Surface Constraint";
  }
  return "Constraint";
}

void registerAnimationModule() {
  registerSceneModule("animation", [] { return std::make_unique<Animation>(); });
  // Motion blur: the renderer asks where nodes are at the shutter's open and close times.
  setNodeMatrixAtTime([](const Scene& scene, const Node& node, double frame, bool wantPivot, Mat4& out) {
    const Animation* a = Animation::of(scene);
    if (!a || a->empty()) return false;
    out = a->worldMatrixAt(scene, node.id, frame);
    if (!wantPivot) out = out * node.objectOffset.matrix();
    return true;
  });
}

}  // namespace veraa
