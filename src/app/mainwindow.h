// Main window: menus, toolbar, viewports, command panel, scene explorer and status bar.
#pragma once

#include <QHash>
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
class QToolBar;
class QVBoxLayout;

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
  // Clones the selection in place; asks for Clone Options when interactive.
  void cloneSelection(bool askOptions);
  void render();
  MaterialEditor* materialEditor();
  RenderSetupDialog* renderSetup();
  FrameBufferWindow* frameBuffer();
  // The view the renderer should use (render camera, or the active viewport).
  RenderView renderView(QString* name) const;

  // ---- Extension API (see extension.h) ----
  // Finds or creates a menu by path, e.g. "Tools" or "Create/Shapes". Top-level menus keep the
  // 3ds Max order: File, Edit, Tools, Group, Views, Create, Modifiers, Animation, Graph Editors,
  // Rendering, Customize, Scripting, Help.
  QMenu* menuAt(const QString& path);
  QToolBar* mainToolBar() const { return mainToolBar_; }
  // Adds a widget between the viewports and the status bar (time slider, track bar, ...).
  void addBelowViewports(QWidget* w);
  void addCommandPanelTab(QWidget* page, const QIcon& icon, const QString& title);
  QLabel* promptLabel() const { return prompt_; }
  // Registers an action under a stable id for the hotkey editor and UI customization.
  void registerCommand(const QString& id, QAction* action);
  QAction* command(const QString& id) const { return commands_.value(id); }
  const QHash<QString, QAction*>& commands() const { return commands_; }

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  int cloneMode_ = 0;  // last Clone Options choice (CloneOptionsDialog::Mode)
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
  QToolBar* mainToolBar_ = nullptr;
  QVBoxLayout* centralLayout_ = nullptr;
  QHash<QString, QAction*> commands_;

  QLabel* prompt_ = nullptr;
  QLabel* typeInLabel_ = nullptr;
  QDoubleSpinBox* typeIn_[3] = {};
  QLabel* selInfo_ = nullptr;
  QLabel* gridInfo_ = nullptr;
  bool updatingTypeIn_ = false;
};

}  // namespace veraa
