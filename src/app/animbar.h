// The animation bar below the viewports: time slider, track bar and the playback / keying
// controls (Auto Key, Set Key, key filters, transport, current frame, Time Configuration).
#pragma once

#include <QDialog>
#include <QWidget>

#include <set>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;
class QSpinBox;
class QToolButton;

namespace veraa {

class AnimController;

// Maps frames to x positions; shared by the time slider and the track bar so they line up.
struct TimeAxis {
  double start = 0, end = 100;
  double left = 0, right = 100;
  double x(double frame) const;
  double frame(double x) const;
};
// Axis for a widget of width w (the slider's arrows and half thumb on each side).
TimeAxis timeAxisFor(const AnimController& c, int width);

class TimeSlider : public QWidget {
  Q_OBJECT
 public:
  explicit TimeSlider(AnimController* c, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 22); }
  static constexpr int kArrow = 18, kThumb = 70;

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;

 private:
  AnimController* c_;
  bool dragging_ = false;
  double grabOffset_ = 0;
};

class TrackBar : public QWidget {
  Q_OBJECT
 public:
  explicit TrackBar(AnimController* c, QWidget* parent = nullptr);
  QSize sizeHint() const override { return QSize(400, 30); }
  // 0 all keys, 1 transform keys, 2 position, 3 rotation, 4 scale
  int filter = 0;
  const std::set<double>& selectedTimes() const { return selected_; }
  void selectTimes(const std::set<double>& t) {
    selected_ = t;
    update();
  }
  // Key times shown (after the filter) with a bit mask: 1 position, 2 rotation, 4 scale, 8 other.
  std::vector<std::pair<double, int>> keyMarks() const;
  void deleteSelectedKeys();

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;

 private:
  bool pathPassesFilter(const std::string& path) const;
  double keyAt(double x) const;  // key time near x, or NaN
  AnimController* c_;
  std::set<double> selected_;
  bool dragging_ = false, copyDrag_ = false, moved_ = false;
  double dragStartFrame_ = 0, dragDelta_ = 0;
};

class AnimControlBar : public QWidget {
  Q_OBJECT
 public:
  explicit AnimControlBar(AnimController* c, QWidget* parent = nullptr);

 private:
  void sync();
  AnimController* c_;
  QToolButton *autoKey_, *setKey_, *setKeys_, *filters_, *play_, *keyMode_;
  QComboBox* source_;
  QDoubleSpinBox* frame_;
  QLabel* timeLabel_;
};

class KeyFiltersDialog : public QDialog {
  Q_OBJECT
 public:
  KeyFiltersDialog(AnimController* c, QWidget* parent = nullptr);
};

class TimeConfigDialog : public QDialog {
  Q_OBJECT
 public:
  TimeConfigDialog(AnimController* c, QWidget* parent = nullptr);
  void apply();

 private:
  AnimController* c_;
  QRadioButton* rate_[4];
  QDoubleSpinBox* fps_;
  QRadioButton* display_[4];
  QCheckBox *realTime_, *loop_;
  QComboBox *speed_, *direction_;
  QSpinBox *start_, *end_, *length_, *current_;
  QCheckBox* rescale_;
};

}  // namespace veraa
