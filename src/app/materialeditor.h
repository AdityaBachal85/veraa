// Material Editor: sample slots with rendered previews, presets and the material's parameters.
#pragma once

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QTimer>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "document.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace veraa {

class ParamForm;

// Renders material swatches on a background thread.
class PreviewRenderer : public QObject {
  Q_OBJECT
 public:
  explicit PreviewRenderer(QObject* parent = nullptr);
  ~PreviewRenderer() override;
  // Queues a preview; a newer request for the same id replaces an older pending one.
  void request(uint32_t id, const Material& material, int size, int samples);

 Q_SIGNALS:
  void ready(uint32_t id, const QImage& image);

 private:
  struct Job {
    uint32_t id;
    Material material;
    int size, samples;
  };
  void run();
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  bool quit_ = false;
};

class MaterialEditor : public QDialog {
  Q_OBJECT
 public:
  explicit MaterialEditor(SceneDocument* doc, QWidget* parent = nullptr);
  void selectMaterial(uint32_t id);
  uint32_t currentMaterial() const { return currentId_; }

 private:
  void rebuildSlots();
  void showCurrent();
  void refreshUsage();
  void requestPreview(uint32_t id);
  void applyPreset(int index);
  Material* current() const;

  SceneDocument* doc_;
  PreviewRenderer previews_;
  QListWidget* slots_;
  QLabel* bigPreview_;
  QLineEdit* name_;
  QComboBox* presets_;
  QLabel* usage_;
  ParamForm* form_;
  QPushButton *assign_, *pick_, *del_, *dup_;
  uint32_t currentId_ = 0;
  const Material* formMaterial_ = nullptr;
  QHash<uint32_t, QImage> images_;
  QTimer previewTimer_;
  QList<uint32_t> pendingPreviews_;
};

}  // namespace veraa
