// Auto-generated editors for a ParamBlock, grouped into collapsible rollouts like 3ds Max.
#pragma once

#include <QFrame>
#include <QPushButton>
#include <QWidget>

#include <functional>
#include <string>

#include "params.h"

class QVBoxLayout;
class QFormLayout;
class QToolButton;

namespace veraa {

// Collapsible titled section.
class Rollout : public QWidget {
  Q_OBJECT
 public:
  explicit Rollout(const QString& title, QWidget* parent = nullptr);
  QWidget* body() const { return body_; }
  QFormLayout* form() const { return form_; }
  void setExpanded(bool on);
  bool isExpanded() const;

 private:
  QToolButton* header_;
  QFrame* body_;
  QFormLayout* form_;
};

// Button showing a color; opens a color picker with live preview.
class ColorSwatch : public QPushButton {
  Q_OBJECT
 public:
  explicit ColorSwatch(QWidget* parent = nullptr);
  void setColor(const Vec3& c);
  Vec3 color() const { return color_; }

 Q_SIGNALS:
  void colorEdited(const Vec3& c);  // live while the picker is open

 protected:
  void paintEvent(QPaintEvent* e) override;

 private:
  void pick();
  Vec3 color_{0.5f, 0.5f, 0.5f};
};

// Lets users drag up/down on spinner arrows to scrub values (and right-click arrows to reset).
void installSpinnerScrub(QWidget* spinBox);

class ParamForm : public QWidget {
  Q_OBJECT
 public:
  explicit ParamForm(QWidget* parent = nullptr);

  // Builds editors for block (not owned; pass nullptr to clear). Keys in exclude are skipped.
  void setBlock(ParamBlock* block, const QStringList& exclude = {});
  ParamBlock* block() const { return block_; }
  // Re-reads all values from the block without rebuilding.
  void refresh();

  // Called before a value is written (begin undo here) and after (notify here).
  std::function<void(const std::string& key)> beforeChange;
  std::function<void(const std::string& key)> afterChange;
  // Base directory for file browsing.
  QString browseDir;

 private:
  void rebuild();
  void apply(const std::string& key, const std::function<bool(ParamBlock&)>& set);

  ParamBlock* block_ = nullptr;
  QStringList exclude_;
  QVBoxLayout* layout_;
  std::vector<std::function<void()>> refreshers_;
  bool refreshing_ = false;
};

}  // namespace veraa
