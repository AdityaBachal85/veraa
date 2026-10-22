// Main window: menus, toolbar, viewports, command panel, scene explorer and status bar.
#pragma once

#include <QMainWindow>
#include <QPointer>

#include "document.h"
#include "editorstate.h"
#include "renderer.h"

class QAction;
class QActionGroup;
class QDoubleSpinBox;
class QLabel;
class QMenu;

namespace veraa {

class CommandPanel;
class FrameBufferWindow;
class MaterialEditor;
class RenderSetupDialog;
class SceneExplorer;
class Viewport;
class ViewportPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  ~MainWindow() override;

  SceneDocument* document() { return doc_; }
  EditorState* editorState() { return state_; }
  ViewportPanel* viewports() { return viewports_; }
  CommandPanel* commandPanel() { return commandPanel_; }

  bool openFile(const QString& path);
  void render();
  MaterialEditor* materialEditor();
  RenderSetupDialog* renderSetup();
  FrameBufferWindow* frameBuffer();
  // The view the renderer should use (render camera, or the active viewport).
  RenderView renderView(QString* name) const;

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  void createActions();
  void createMenus();
  void createToolBar();
  void createDocks();
  void createStatusBar();
  void updateTitle();
  void updateTypeIn();
  void applyTypeIn(int axis, double value);
  void updateRecentMenu();
  void addRecent(const QString& path);
  bool maybeSave();
  bool save();
  bool saveAs();
  void importObjFile();
  void showContextMenu(Viewport* vp, const QPoint& pos);

  SceneDocument* doc_;
  EditorState* state_;
  ViewportPanel* viewports_ = nullptr;
  CommandPanel* commandPanel_ = nullptr;
  SceneExplorer* explorer_ = nullptr;
  QPointer<MaterialEditor> materialEditor_;
  QPointer<RenderSetupDialog> renderSetup_;
  QPointer<FrameBufferWindow> frameBuffer_;

  QAction *newAct_, *openAct_, *saveAct_, *saveAsAct_, *importAct_, *exitAct_;
  QAction *undoAct_, *redoAct_, *deleteAct_, *cloneAct_, *selectAllAct_, *selectNoneAct_, *selectInvertAct_,
      *selectByNameAct_, *hideAct_, *unhideAct_, *propertiesAct_;
  QAction *selectToolAct_, *moveToolAct_, *rotateToolAct_, *scaleToolAct_;
  QAction *axisXAct_, *axisYAct_, *axisZAct_, *axisPlaneAct_;
  QAction *materialEditorAct_, *renderSetupAct_, *renderAct_, *frameBufferAct_;
  QAction *zoomExtAct_, *zoomExtAllAct_, *undoViewAct_, *maximizeAct_, *shadingAct_, *edgedAct_, *gridAct_,
      *cameraFromViewAct_;
  QActionGroup *toolGroup_, *axisGroup_;
  QMenu* recentMenu_ = nullptr;

  QLabel* prompt_ = nullptr;
  QLabel* typeInLabel_ = nullptr;
  QDoubleSpinBox* typeIn_[3] = {};
  QLabel* selInfo_ = nullptr;
  QLabel* gridInfo_ = nullptr;
  bool updatingTypeIn_ = false;
};

}  // namespace veraa
