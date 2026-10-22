// Scene time, playback and keying (Auto Key, Set Key) for the animation UI. One controller
// exists per main window; the time slider, track bar, Motion panel and graph editors use it.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <functional>

#include "animation.h"
#include "document.h"

namespace veraa {

class MainWindow;

// Set Key filters (Key Filters dialog).
enum KeyFilter : int {
  KeyPosition = 1,
  KeyRotation = 2,
  KeyScale = 4,
  KeyObjectParams = 8,
  KeyModifiers = 16,
  KeyMaterials = 32,
  KeyCustom = 64,
  KeyOther = 128,
  KeyTransform = KeyPosition | KeyRotation | KeyScale,
};

class AnimController : public QObject {
  Q_OBJECT
 public:
  explicit AnimController(MainWindow& window);
  ~AnimController() override;
  // The controller of the main window (null before the animation module is installed).
  static AnimController* instance();

  MainWindow& window() const { return window_; }
  SceneDocument* document() const { return doc_; }
  Scene& scene() const { return doc_->scene(); }
  Animation* animation() const { return Animation::of(doc_->scene()); }

  // ---- Time ----
  double frame() const { return doc_->scene().time.currentFrame; }
  int startFrame() const { return doc_->scene().time.startFrame; }
  int endFrame() const { return doc_->scene().time.endFrame; }
  float fps() const { return doc_->scene().time.fps; }
  void setFrame(double f);
  void setRange(int start, int end);
  void setFps(float fps);
  void stepFrame(int delta);  // next/previous frame (or key in key mode), wrapping in the range
  void goStart();
  void goEnd();
  void nextKey();
  void prevKey();
  bool keyStepMode() const { return keyStepMode_; }
  void setKeyStepMode(bool on);
  // 0 frames, 1 SMPTE (min:sec:frame), 2 FRAME:TICKS, 3 MM:SS:TICKS
  int timeDisplay() const { return timeDisplay_; }
  void setTimeDisplay(int mode);
  QString formatTime(double frame) const;

  // ---- Playback ----
  bool playing() const { return timer_.isActive(); }
  void play();
  void stop();
  void togglePlay();
  bool realTime = true;
  bool loop = true;
  double speed = 1.0;  // 0.25 .. 4
  int direction = 1;   // 1 forward, -1 reverse, 0 ping-pong

  // ---- Keying ----
  bool autoKey() const { return autoKey_; }
  void setAutoKey(bool on);
  bool setKeyMode() const { return setKeyMode_; }
  void setSetKeyMode(bool on);
  int keyFilters() const { return keyFilters_; }
  void setKeyFilters(int f) { keyFilters_ = f; }
  // Keys the filtered channels of the selected nodes at the current frame (Set Keys, K).
  void setKeys();
  // Nodes whose keys the track bar shows: 0 the selection, 1 every node.
  int trackBarSource() const { return trackBarSource_; }
  void setTrackBarSource(int s);
  QVector<uint32_t> trackBarNodes() const;
  // Animatable paths of a node that pass the key filters.
  std::vector<std::string> filteredPaths(uint32_t nodeId, int filters) const;

  // ---- Editing animation data ----
  // Runs fn as one undoable step without Auto Key reacting, then re-evaluates the scene.
  void editAnimation(const QString& label, const std::function<void()>& fn);
  // The same as a begin/end pair, for edits spanning several events (dragging keys).
  void beginAnimationEdit(const QString& label);
  void endAnimationEdit();
  // Applies live changes during an animation edit (re-evaluates without recording undo).
  void previewAnimation();
  // Re-evaluates the animation at the current frame and refreshes views.
  void refresh();
  // Ghosting (viewport display of the selection at nearby frames).
  bool ghosting = false;
  int ghostFrames = 3;
  int ghostStep = 5;

 Q_SIGNALS:
  void frameChanged(double frame);
  void rangeChanged();
  void autoKeyChanged(bool on);
  void setKeyModeChanged(bool on);
  void playingChanged(bool on);
  void keyStepModeChanged(bool on);
  void animationChanged();  // tracks or keys changed
  void timeDisplayChanged();
  void trackBarSourceChanged();

 private:
  void applyFrame(double f);
  void onCommit(const json& before);
  void tick();

  MainWindow& window_;
  SceneDocument* doc_;
  QTimer timer_;
  QElapsedTimer clock_;
  double playStartFrame_ = 0.0;
  bool autoKey_ = false;
  bool setKeyMode_ = false;
  bool keyStepMode_ = false;
  int keyFilters_ = KeyTransform;
  int trackBarSource_ = 0;
  int timeDisplay_ = 0;
  int suppress_ = 0;
  uint64_t lastRevision_ = 0;
};

}  // namespace veraa
