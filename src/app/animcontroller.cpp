#include "animcontroller.h"

#include <QCoreApplication>

#include <cmath>
#include <unordered_map>

#include "mainwindow.h"

namespace veraa {
namespace {

AnimController* g_instance = nullptr;

void appendParamPaths(const ParamBlock& p, const std::string& prefix, std::vector<std::string>& out) {
  for (const auto& d : p.defs()) {
    switch (d.type) {
      case ParamType::Float:
      case ParamType::Int:
      case ParamType::Bool:
      case ParamType::Enum: out.push_back(prefix + d.key); break;
      case ParamType::Color:
        for (const char* c : {".r", ".g", ".b"}) out.push_back(prefix + d.key + c);
        break;
      default: break;
    }
  }
}

}  // namespace

AnimController::AnimController(MainWindow& window)
    : QObject(&window), window_(window), doc_(window.document()), docGuard_(doc_) {
  g_instance = this;
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &AnimController::tick);
  doc_->setCommitHook([this](const SceneSnapshot& before) { onCommit(before); });
  connect(doc_, &SceneDocument::structureChanged, this, [this] {
    Q_EMIT rangeChanged();
    Q_EMIT frameChanged(frame());
    Q_EMIT animationChanged();
  });
  connect(doc_, &SceneDocument::fileChanged, this, [this] {
    stop();
    refresh();
    Q_EMIT rangeChanged();
  });
}

AnimController::~AnimController() {
  if (g_instance == this) g_instance = nullptr;
  if (docGuard_) docGuard_->setCommitHook(nullptr);
}

AnimController* AnimController::instance() { return g_instance; }

// ---------------------------------------------------------------------------------------------
// Time

void AnimController::applyFrame(double f) {
  Scene& s = scene();
  Animation* anim = animation();
  Animation::EvalResult r;
  if (anim) r = anim->evaluate(s, f);
  else s.time.currentFrame = f;
  QVector<uint32_t> t(r.transformChanged.begin(), r.transformChanged.end());
  QVector<uint32_t> g(r.geometryChanged.begin(), r.geometryChanged.end());
  QVector<uint32_t> o(r.otherChanged.begin(), r.otherChanged.end());
  if (!t.isEmpty()) doc_->notifyNodesChanged(t, ChangeTransform);
  if (!g.isEmpty()) doc_->notifyNodesChanged(g, ChangeGeometry);
  if (!o.isEmpty()) doc_->notifyNodesChanged(o, ChangeGeometry);
  if (r.materialsChanged) doc_->notifyMaterialsChanged();
  if (r.renderSettingsChanged) doc_->notifyRenderSettingsChanged();
  Q_EMIT frameChanged(f);
}

void AnimController::setFrame(double f) {
  f = std::clamp(f, static_cast<double>(startFrame()), static_cast<double>(endFrame()));
  doc_->commitSoftEdit();
  applyFrame(f);
}

void AnimController::refresh() {
  applyFrame(frame());
  Q_EMIT animationChanged();
}

void AnimController::setRange(int start, int end) {
  if (end <= start) end = start + 1;
  TimeConfig& t = scene().time;
  if (t.startFrame == start && t.endFrame == end) return;
  t.startFrame = start;
  t.endFrame = end;
  doc_->setModified(true);
  Q_EMIT rangeChanged();
  if (frame() < start || frame() > end) setFrame(std::clamp(frame(), static_cast<double>(start), static_cast<double>(end)));
}

void AnimController::setFps(float fps) {
  fps = std::clamp(fps, 1.0f, 1000.0f);
  if (scene().time.fps == fps) return;
  scene().time.fps = fps;
  doc_->setModified(true);
  if (playing()) {
    stop();
    play();
  }
  Q_EMIT rangeChanged();
  refresh();
}

void AnimController::stepFrame(int delta) {
  if (keyStepMode_) {
    if (delta > 0) nextKey();
    else prevKey();
    return;
  }
  double f = std::round(frame()) + delta;
  if (f > endFrame()) f = startFrame();
  if (f < startFrame()) f = endFrame();
  setFrame(f);
}

void AnimController::goStart() {
  stop();
  setFrame(startFrame());
}

void AnimController::goEnd() {
  stop();
  setFrame(endFrame());
}

void AnimController::nextKey() {
  Animation* anim = animation();
  if (!anim) return;
  std::vector<double> times;
  QVector<uint32_t> ids = trackBarNodes();
  anim->keyTimesForNodes(std::vector<uint32_t>(ids.begin(), ids.end()), times);
  double cur = frame();
  for (double t : times)
    if (t > cur + 1e-6 && t <= endFrame()) {
      setFrame(t);
      return;
    }
  for (double t : times)
    if (t >= startFrame()) {  // wrap around
      setFrame(t);
      return;
    }
}

void AnimController::prevKey() {
  Animation* anim = animation();
  if (!anim) return;
  std::vector<double> times;
  QVector<uint32_t> ids = trackBarNodes();
  anim->keyTimesForNodes(std::vector<uint32_t>(ids.begin(), ids.end()), times);
  double cur = frame();
  for (auto it = times.rbegin(); it != times.rend(); ++it)
    if (*it < cur - 1e-6 && *it >= startFrame()) {
      setFrame(*it);
      return;
    }
  for (auto it = times.rbegin(); it != times.rend(); ++it)
    if (*it <= endFrame()) {
      setFrame(*it);
      return;
    }
}

void AnimController::setKeyStepMode(bool on) {
  if (keyStepMode_ == on) return;
  keyStepMode_ = on;
  Q_EMIT keyStepModeChanged(on);
}

void AnimController::setTimeDisplay(int mode) {
  timeDisplay_ = std::clamp(mode, 0, 3);
  Q_EMIT timeDisplayChanged();
}

QString AnimController::formatTime(double f) const {
  double rate = std::max(1.0f, fps());
  double ticksPerFrame = 4800.0 / rate;
  bool whole = std::fabs(f - std::round(f)) < 1e-6;
  switch (timeDisplay_) {
    case 1: {
      long fr = std::lround(std::floor(f));
      long perSec = std::lround(rate);
      long sec = fr / std::max(1L, perSec), frames = fr % std::max(1L, perSec);
      return QStringLiteral("%1:%2:%3").arg(sec / 60).arg(sec % 60).arg(frames);
    }
    case 2: {
      double fl = std::floor(f);
      return QStringLiteral("%1:%2").arg(static_cast<long>(fl)).arg(std::lround((f - fl) * ticksPerFrame));
    }
    case 3: {
      double secs = f / rate;
      long whole_s = static_cast<long>(std::floor(secs));
      long ticks = std::lround((secs - whole_s) * 4800.0);
      return QStringLiteral("%1:%2:%3").arg(whole_s / 60).arg(whole_s % 60, 2, 10, QChar('0')).arg(ticks, 4, 10, QChar('0'));
    }
    default: return whole ? QString::number(std::lround(f)) : QString::number(f, 'f', 1);
  }
}

// ---------------------------------------------------------------------------------------------
// Playback

void AnimController::play() {
  if (playing()) return;
  doc_->commitSoftEdit();
  if (endFrame() <= startFrame()) return;
  if (direction >= 0 && frame() >= endFrame() && !loop) setFrame(startFrame());
  playStartFrame_ = frame();
  clock_.start();
  double interval = 1000.0 / std::max(1.0, fps() * speed);
  timer_.start(std::max(1, static_cast<int>(interval)));
  Q_EMIT playingChanged(true);
}

void AnimController::stop() {
  if (!timer_.isActive()) return;
  timer_.stop();
  // Settle on a whole frame.
  if (std::fabs(frame() - std::round(frame())) > 1e-6) setFrame(std::round(frame()));
  Q_EMIT playingChanged(false);
}

void AnimController::togglePlay() {
  if (playing()) stop();
  else play();
}

void AnimController::tick() {
  double start = startFrame(), end = endFrame(), len = end - start;
  if (len <= 0.0) {
    stop();
    return;
  }
  double next;
  if (realTime) {
    double adv = clock_.elapsed() / 1000.0 * fps() * speed;
    if (direction == 0) {
      double period = 2.0 * len;
      double t = std::fmod(playStartFrame_ - start + adv, period);
      next = start + (t <= len ? t : period - t);
    } else {
      double raw = playStartFrame_ + direction * adv;
      if (raw > end || raw < start) {
        if (!loop) {
          setFrame(raw > end ? end : start);
          stop();
          return;
        }
        double span = len + 1.0;
        next = start + std::fmod(std::fmod(raw - start, span) + span, span);
      } else {
        next = raw;
      }
    }
    next = std::floor(next + 1e-9);
  } else {
    int dir = direction == 0 ? (playStartFrame_ <= frame() ? 1 : -1) : direction;
    next = std::round(frame()) + dir;
    if (direction == 0) {
      // Ping-pong: remember the travel direction in playStartFrame_.
      if (next > end || next < start) {
        dir = -dir;
        next = std::round(frame()) + dir;
      }
      playStartFrame_ = dir > 0 ? frame() - 1.0 : frame() + 1.0;
    } else if (next > end || next < start) {
      if (!loop) {
        stop();
        return;
      }
      next = next > end ? start : end;
    }
  }
  if (std::fabs(next - frame()) > 1e-9) applyFrame(std::clamp(next, start, end));
}

// ---------------------------------------------------------------------------------------------
// Keying

void AnimController::setAutoKey(bool on) {
  if (autoKey_ == on) return;
  autoKey_ = on;
  if (on && setKeyMode_) setSetKeyMode(false);
  Q_EMIT autoKeyChanged(on);
}

void AnimController::setSetKeyMode(bool on) {
  if (setKeyMode_ == on) return;
  setKeyMode_ = on;
  if (on && autoKey_) setAutoKey(false);
  Q_EMIT setKeyModeChanged(on);
}

void AnimController::setTrackBarSource(int s) {
  trackBarSource_ = s;
  Q_EMIT trackBarSourceChanged();
}

QVector<uint32_t> AnimController::trackBarNodes() const {
  if (trackBarSource_ == 1) {
    QVector<uint32_t> all;
    for (const auto& n : scene().nodes) all.push_back(n->id);
    return all;
  }
  return doc_->selection();
}

std::vector<std::string> AnimController::filteredPaths(uint32_t nodeId, int filters) const {
  std::vector<std::string> out;
  Animation* anim = animation();
  const Node* n = scene().findNode(nodeId);
  if (!anim || !n) return out;
  for (const auto& path : anim->animatablePaths(scene(), nodeId)) {
    ValuePath p = ValuePath::parse(path);
    int bit = KeyOther;
    switch (p.kind) {
      case ValuePath::Kind::Transform: bit = p.channel < 3 ? KeyPosition : p.channel < 6 ? KeyRotation : KeyScale; break;
      case ValuePath::Kind::ObjectParam: bit = KeyObjectParams; break;
      case ValuePath::Kind::ModifierParam: bit = KeyModifiers; break;
      case ValuePath::Kind::Custom: bit = KeyCustom; break;
      default: bit = KeyOther; break;
    }
    if (filters & bit) out.push_back(path);
  }
  if ((filters & KeyMaterials) && n->materialId)
    if (const Material* m = scene().findMaterial(n->materialId))
      appendParamPaths(m->params, "material:" + std::to_string(m->id) + "/", out);
  return out;
}

void AnimController::setKeys() {
  Animation* anim = animation();
  if (!anim || doc_->selection().isEmpty()) return;
  editAnimation(tr("Set Keys"), [&] {
    Scene& s = scene();
    double f = frame();
    for (Node* n : doc_->selectedNodes())
      for (const auto& path : filteredPaths(n->id, keyFilters_)) {
        double v;
        if (!Animation::readValue(s, path, v)) continue;
        // A new track only gets a key at this frame.
        anim->setKey(s, path, f, v, anim->isAnimated(path) ? startFrame() : f);
      }
  });
}

void AnimController::editAnimation(const QString& label, const std::function<void()>& fn) {
  beginAnimationEdit(label);
  fn();
  endAnimationEdit();
}

void AnimController::beginAnimationEdit(const QString& label) {
  doc_->commitSoftEdit();
  doc_->beginEdit(label);
  ++suppress_;
}

void AnimController::endAnimationEdit() {
  if (Animation* anim = animation()) anim->touch();
  applyFrame(frame());
  doc_->endEdit();
  --suppress_;
  Q_EMIT animationChanged();
}

void AnimController::previewAnimation() {
  if (Animation* anim = animation()) anim->touch();
  applyFrame(frame());
}

void AnimController::onCommit(const SceneSnapshot& before) {
  if (suppress_ > 0) return;
  Animation* anim = animation();
  if (!anim) return;
  Scene& s = scene();
  bool keying = autoKey_ && !setKeyMode_;
  // Quick exit: nothing animated and not keying means nothing to do.
  if (!keying && anim->empty()) return;
  Scene old;
  old.restore(before);

  struct Change {
    std::string path;
    double oldV, newV;
  };
  std::vector<Change> changes;
  auto collect = [&](const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
      double nv, ov;
      if (!Animation::readValue(s, path, nv) || !Animation::readValue(old, path, ov)) continue;
      if (std::fabs(nv - ov) <= 1e-6 * std::max(1.0, std::fabs(ov))) continue;
      changes.push_back({path, ov, nv});
    }
  };
  for (const auto& up : s.nodes) {
    const Node& n = *up;
    const Node* o = old.findNode(n.id);
    if (!o || !o->object || !n.object || o->object->type() != n.object->type()) continue;
    bool same = n.transform.position == o->transform.position && n.transform.rotation == o->transform.rotation &&
                n.transform.scale == o->transform.scale && n.object->params.toJson() == o->object->params.toJson() &&
                n.modifiers.size() == o->modifiers.size();
    if (same)
      for (size_t i = 0; i < n.modifiers.size() && same; ++i)
        same = n.modifiers[i] && o->modifiers[i] && n.modifiers[i]->type() == o->modifiers[i]->type() &&
               n.modifiers[i]->params.toJson() == o->modifiers[i]->params.toJson();
    std::vector<std::string> paths = anim->animatablePaths(s, n.id);
    if (same) {
      // Only constraint and custom attribute values (kept in the animation data) can differ.
      paths.erase(std::remove_if(paths.begin(), paths.end(),
                                 [](const std::string& p) {
                                   auto k = ValuePath::parse(p).kind;
                                   return k != ValuePath::Kind::Constraint && k != ValuePath::Kind::Custom;
                                 }),
                  paths.end());
      if (paths.empty()) continue;
    }
    // Modifier params only when the stack itself did not change.
    if (n.modifiers.size() != o->modifiers.size())
      paths.erase(std::remove_if(paths.begin(), paths.end(),
                                 [](const std::string& p) { return ValuePath::parse(p).kind == ValuePath::Kind::ModifierParam; }),
                  paths.end());
    collect(paths);
  }
  for (const auto& m : s.materials) {
    const Material* om = old.findMaterial(m->id);
    if (!om || om->type() != m->type() || m->params.toJson() == om->params.toJson()) continue;
    std::vector<std::string> paths;
    appendParamPaths(m->params, "material:" + std::to_string(m->id) + "/", paths);
    collect(paths);
  }
  if (s.renderSettings.toJson() != old.renderSettings.toJson()) {
    std::vector<std::string> paths;
    appendParamPaths(s.renderSettings, "render/", paths);
    collect(paths);
  }
  if (changes.empty()) return;

  double f = frame();
  int start = startFrame();
  bool touched = false;
  // Wired values: editing the slave of a two-way wire drives its master; other edits of wired
  // slaves are overridden by the wire.
  std::vector<Change> driven;
  for (const auto& w : anim->wires)
    for (auto it = changes.begin(); it != changes.end();) {
      if (it->path != w.to) {
        ++it;
        continue;
      }
      if (w.twoWay) {
        Expression e;
        double ov;
        if (e.parse(w.exprFrom) && Animation::readValue(old, w.from, ov)) {
          double x = it->newV;
          double nv = e.evaluate([&](const std::string& n) { return n == "x" ? x : n == "F" ? f : 0.0; });
          Animation::writeValue(s, w.from, nv);
          driven.push_back({w.from, ov, nv});
          touched = true;
        }
      }
      it = changes.erase(it);
    }
  changes.insert(changes.end(), driven.begin(), driven.end());
  for (auto& c : changes) {
    ValuePath p = ValuePath::parse(c.path);
    if (p.kind == ValuePath::Kind::Transform && p.channel >= 3 && p.channel < 6)
      c.newV += 360.0 * std::round((c.oldV - c.newV) / 360.0);  // keep Euler angles continuous
    bool animated = anim->isAnimated(c.path);
    if (keying) {
      // At the start frame, edits of unanimated values change the base value.
      if (!animated && std::fabs(f - start) < 1e-6) continue;
      anim->setKey(s, c.path, f, c.newV, start, c.oldV);
      touched = true;
    } else if (animated && !setKeyMode_) {
      // Editing an animated value without Auto Key moves its whole animation.
      Track* t = nullptr;
      if (anim->activeLayer > 0 && anim->activeLayer <= static_cast<int>(anim->layers.size())) {
        auto& L = anim->layers[static_cast<size_t>(anim->activeLayer - 1)].tracks;
        auto it = L.find(c.path);
        if (it != L.end()) t = &it->second;
      }
      if (!t) t = anim->findTrack(c.path);
      if (t && t->hasKeys()) {
        t->offsetValues(c.newV - c.oldV);
        touched = true;
      }
    }
  }
  if (!touched) return;
  anim->touch();
  applyFrame(f);
  Q_EMIT animationChanged();
}

}  // namespace veraa
