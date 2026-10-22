// Rendering dialogs: Environment and Effects (8), Batch Render and the Print Size Assistant.
#pragma once

#include <QDialog>
#include <QTimer>

#include <memory>
#include <string>
#include <vector>

#include "document.h"
#include "renderer.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

namespace veraa {

// Creates a Sun Positioner (unless the scene already has a sun) and switches the scene to the
// physical sky with physical exposure. Call inside an open edit. Returns the sun node.
Node* addSunAndSky(SceneDocument* doc);

class EnvironmentDialog : public QDialog {
  Q_OBJECT
 public:
  explicit EnvironmentDialog(SceneDocument* doc, QWidget* parent = nullptr);
  QTabWidget* tabs() const { return tabs_; }

 private:
  SceneDocument* doc_;
  QTabWidget* tabs_;
};

// One batch render job (stored in Scene::extensions["batchRender"]).
struct BatchJob {
  bool enabled = true;
  std::string name = "View01";
  std::string camera;      // empty = the scene's render camera (or the first camera)
  int width = 0, height = 0;  // 0 = render setup size
  int samples = 0;          // 0 = render setup samples
  std::string output;       // image path (any supported format)
  bool saveElements = false;  // also writes render elements next to the image
};

std::vector<BatchJob> batchJobsFromScene(const Scene& scene);
void batchJobsToScene(Scene& scene, const std::vector<BatchJob>& jobs);

class BatchRenderDialog : public QDialog {
  Q_OBJECT
 public:
  explicit BatchRenderDialog(SceneDocument* doc, QWidget* parent = nullptr);
  ~BatchRenderDialog() override;

  // Renders all enabled jobs one after another (asynchronously).
  void startBatch();
  void stopBatch();
  bool isRunning() const { return running_; }
  // Blocks (pumping events) until the batch is done. Returns false on timeout.
  bool waitForFinish(int timeoutMs);
  // Status line of a job after a batch run ("Done (12.3 s)", "Failed: ...").
  QString jobStatus(int index) const;
  int jobCount() const { return static_cast<int>(jobs_.size()); }
  void addJob(const BatchJob& job);

 private:
  void reload();
  void store(const QString& label);
  void refreshTable();
  void loadEditor();
  void editorChanged();
  void rebuildCameras();
  void nextJob();
  void tick();
  void finishJob();
  void browseOutput();

  SceneDocument* doc_;
  std::vector<BatchJob> jobs_;
  std::vector<QString> status_;
  QTableWidget* table_;
  QLineEdit* name_;
  QComboBox* camera_;
  QSpinBox *width_, *height_, *samples_;
  QLineEdit* output_;
  QCheckBox* elements_;
  QPushButton *renderBtn_, *stopBtn_;
  QLabel* progress_;
  bool loading_ = false;

  std::unique_ptr<Renderer> renderer_;
  QTimer timer_;
  bool running_ = false;
  int current_ = -1;
  double started_ = 0.0;
};

class PrintSizeDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PrintSizeDialog(SceneDocument* doc, QWidget* parent = nullptr);
  // Computed output size in pixels.
  int pixelWidth() const;
  int pixelHeight() const;
  void setPaper(const QString& name, bool landscape, int dpi);
  void apply();

 Q_SIGNALS:
  void renderRequested();

 private:
  void paperChanged();
  void updateResult();
  SceneDocument* doc_;
  QComboBox* paper_;
  QComboBox* orientation_;
  QDoubleSpinBox *widthMm_, *heightMm_;
  QSpinBox* dpi_;
  QLabel* result_;
  bool loading_ = false;
};

}  // namespace veraa
