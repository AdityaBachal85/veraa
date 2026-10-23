#include "params.h"

#include <atomic>
#include <cmath>

namespace veraa {

namespace {
const std::string kEmpty;
}

ParamDef& ParamBlock::add(ParamDef d) {
  d.group = currentGroup_;
  ParamValue v;
  v.number = d.defNumber;
  v.color = d.defColor;
  v.text = d.defString;
  values_[d.key] = v;
  defs_.push_back(std::move(d));
  return defs_.back();
}

ParamDef& ParamBlock::addFloat(const std::string& key, const std::string& label, double def, double minV,
                               double maxV, double step, int decimals) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::Float;
  d.defNumber = def; d.minValue = minV; d.maxValue = maxV; d.step = step; d.decimals = decimals;
  return add(std::move(d));
}

ParamDef& ParamBlock::addInt(const std::string& key, const std::string& label, int def, int minV, int maxV) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::Int;
  d.defNumber = def; d.minValue = minV; d.maxValue = maxV; d.step = 1; d.decimals = 0;
  return add(std::move(d));
}

ParamDef& ParamBlock::addBool(const std::string& key, const std::string& label, bool def) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::Bool;
  d.defNumber = def ? 1.0 : 0.0; d.minValue = 0; d.maxValue = 1;
  return add(std::move(d));
}

ParamDef& ParamBlock::addColor(const std::string& key, const std::string& label, const Vec3& def) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::Color; d.defColor = def;
  return add(std::move(d));
}

ParamDef& ParamBlock::addEnum(const std::string& key, const std::string& label, std::vector<std::string> items,
                              int def) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::Enum;
  d.minValue = 0; d.maxValue = static_cast<double>(items.size()) - 1; d.defNumber = def;
  d.items = std::move(items);
  return add(std::move(d));
}

ParamDef& ParamBlock::addString(const std::string& key, const std::string& label, const std::string& def) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::String; d.defString = def;
  return add(std::move(d));
}

ParamDef& ParamBlock::addFile(const std::string& key, const std::string& label, const std::string& filter) {
  ParamDef d;
  d.key = key; d.label = label; d.type = ParamType::File; d.fileFilter = filter;
  return add(std::move(d));
}

const ParamDef* ParamBlock::def(const std::string& key) const {
  for (const auto& d : defs_)
    if (d.key == key) return &d;
  return nullptr;
}

float ParamBlock::getFloat(const std::string& key) const {
  auto it = values_.find(key);
  return it == values_.end() ? 0.0f : static_cast<float>(it->second.number);
}

int ParamBlock::getInt(const std::string& key) const {
  auto it = values_.find(key);
  return it == values_.end() ? 0 : static_cast<int>(std::lround(it->second.number));
}

bool ParamBlock::getBool(const std::string& key) const {
  auto it = values_.find(key);
  return it != values_.end() && it->second.number != 0.0;
}

Vec3 ParamBlock::getColor(const std::string& key) const {
  auto it = values_.find(key);
  return it == values_.end() ? Vec3() : it->second.color;
}

const std::string& ParamBlock::getString(const std::string& key) const {
  auto it = values_.find(key);
  return it == values_.end() ? kEmpty : it->second.text;
}

bool ParamBlock::setNumber(const std::string& key, double v) {
  auto it = values_.find(key);
  if (it == values_.end()) return false;
  if (const ParamDef* d = def(key)) {
    if (d->type == ParamType::Int || d->type == ParamType::Enum) v = std::round(v);
    if (d->type == ParamType::Bool) v = v != 0.0 ? 1.0 : 0.0;
    v = std::min(std::max(v, d->minValue), d->maxValue);
  }
  if (it->second.number == v) return false;
  it->second.number = v;
  return true;
}

bool ParamBlock::setColor(const std::string& key, const Vec3& c) {
  auto it = values_.find(key);
  if (it == values_.end() || it->second.color == c) return false;
  it->second.color = c;
  return true;
}

bool ParamBlock::setString(const std::string& key, const std::string& s) {
  auto it = values_.find(key);
  if (it == values_.end() || it->second.text == s) return false;
  it->second.text = s;
  return true;
}

json ParamBlock::toJson() const {
  json j = json::object();
  for (const auto& d : defs_) {
    const ParamValue& v = values_.at(d.key);
    switch (d.type) {
      case ParamType::Float: j[d.key] = v.number; break;
      case ParamType::Int:
      case ParamType::Enum: j[d.key] = static_cast<int>(std::lround(v.number)); break;
      case ParamType::Bool: j[d.key] = v.number != 0.0; break;
      case ParamType::Color: j[d.key] = vec3ToJson(v.color); break;
      case ParamType::String:
      case ParamType::File: j[d.key] = v.text; break;
    }
  }
  return j;
}

void ParamBlock::fromJson(const json& j) {
  if (!j.is_object()) return;
  for (const auto& d : defs_) {
    auto it = j.find(d.key);
    if (it == j.end()) continue;
    switch (d.type) {
      case ParamType::Float:
      case ParamType::Int:
      case ParamType::Enum:
        if (it->is_number()) setNumber(d.key, it->get<double>());
        break;
      case ParamType::Bool:
        if (it->is_boolean()) setNumber(d.key, it->get<bool>() ? 1.0 : 0.0);
        else if (it->is_number()) setNumber(d.key, it->get<double>());
        break;
      case ParamType::Color: setColor(d.key, vec3FromJson(*it, getColor(d.key))); break;
      case ParamType::String:
      case ParamType::File:
        if (it->is_string()) setString(d.key, it->get<std::string>());
        break;
    }
  }
}

uint64_t nextRevisionStamp() {
  static std::atomic<uint64_t> counter{1};
  return ++counter;
}

json vec3ToJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

Vec3 vec3FromJson(const json& j, const Vec3& fallback) {
  if (!j.is_array() || j.size() != 3) return fallback;
  for (const auto& e : j)
    if (!e.is_number()) return fallback;
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

}  // namespace veraa
