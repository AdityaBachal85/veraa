// Main window: menus, main toolbar, axis-constraint toolbar, viewports, command panel, scene and
// layer explorers, and the two-row status bar (script mini listener, prompt, transform type-in,
// navigation controls).
#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QSet>

#include "document.h"
#include "editorstate.h"
#include "renderer.h"

class QAction;
class QActionGroup;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QTimer;
class QToolBar;
class QToolButton;
class QVBoxLayout;

namespace veraa {

struct ImportOptions;
class CommandPanel;
class FrameBufferWindow;
class LayerExplorer;
class MaterialEditor;
class RenderSetupDialog;
class SceneExplorer;
class ScriptListener;
class UnitSpinBox;
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
  SceneExplorer* sceneExplorer() { return explorer_; }
  LayerExplorer* layerExplorer() { return layerExplorer_; }

  // Opens a .veraa scene; other formats are imported and .py files are run.
  bool openFile(const QString& path);
  // Non-interactive import/export by file extension (see io_formats.h); one undo step.
  bool importPath(const QString& path, QVector<uint32_t>* ids, QString* error);
  bool importPathWith(const QString& path, const ImportOptions& options, QVector<uint32_t>* ids, QString* error);
  bool exportPath(const QString& path, bool selectedOnly, QString* error);
  // Merges objects (all, or only `names`) from a .veraa file; as XRefs they come in frozen.
  QVector<uint32_t> mergeScene(const QString& path, const QStringList& names, bool asXRef, QString* error);
  // Re-merges every XRef scene from its file (File > Reference > Update XRefs).
  void reloadXRefs();
  // Clones the selection in place; asks for Clone Options when interactive.
  void cloneSelection(bool askOptions);
  void render();
  MaterialEditor* materialEditor();
  RenderSetupDialog* renderSetup();
  FrameBufferWindow* frameBuffer();
  // The view the renderer should use (render camera, or the active viewport).
  RenderView renderView(QString* name) const;
  void setPrompt(const QString& text);

  // ---- Extension API (see extension.h) ----
  // Finds or creates a menu by path, e.g. "Tools" or "Create/Shapes". Top-level menus keep the
  // 3ds Max order: File, Edit, Tools, Group, Views, Create, Modifiers, Animation, Graph Editors,
  // Rendering, Customize, Scripting, Help.
  QMenu* menuAt(const QString& path);
  QToolBar* mainToolBar() const { return mainToolBar_; }
  // Adds a widget between the viewports and the status bar (time slider, track bar, ...).
  void addBelowViewports(QWidget* w);
  // Adds a command panel tab. Known titles keep the 3ds Max order: Create, Modify, Hierarchy,
  // Motion, Display, Utilities; others go last.
  void addCommandPanelTab(QWidget* page, const QIcon& icon, const QString& title);
  // Widgets for the status bar's animation area (between the transform type-in and the
  // viewport navigation controls): auto/set key buttons, playback controls, frame spinner.
  void addStatusBarWidget(QWidget* w);
  QLabel* promptLabel() const { return prompt_; }
  // Registers an action under a stable id for the hotkey editor and UI customization.
  void registerCommand(const QString& id, QAction* action);
  QAction* command(const QString& id) const { return commands_.value(id); }
  const QHash<QString, QAction*>& commands() const { return commands_; }

  // ---- Workspaces and UI schemes ----
  QStringList workspaceNames() const;
  void saveWorkspace(const QString& name);
  bool loadWorkspace(const QString& name);
  void deleteWorkspace(const QString& name);
  void resetUiLayout();
  void setUiLocked(bool locked);
  // Hotkeys: id -> key sequence text. Applied to the registered commands.
  QHash<QString, QString> hotkeys() const;
  const QHash<QString, QString>& defaultHotkeys() const { return defaultShortcuts_; }
  void applyHotkeys(const QHash<QString, QString>& keys, bool persist);
  // Re-reads Customize > Preferences (auto backup, undo levels, grid, spinner snap).
  void applyPreferences();

  // ---- Edit operations used by menus, panels and scripts ----
  void hold();
  bool fetch();
  void isolateSelection(bool on);
  bool isIsolated() const { return isolated_; }
  void selectSimilar();
  void selectInstances();
  void openTransformTypeIn();
  void updateTypeIn();
  // Spinner snap: spinners step by the preference value (Customize > Preferences).
  void setSpinnerSnap(bool on);
  bool spinnerSnap() const { return spinnerSnap_; }
  // Navigation controls (bottom right): the checked tool's command id, or empty.
  QString activeNavigationTool() const;

  // ---- Features shown in the interface but not implemented yet ----
  // Shows "<feature> is not available in Veraa yet" (a non-blocking message box and the prompt).
  void showNotAvailable(const QString& feature);
  // The feature named by the last showNotAvailable() call (self-tests).
  QString lastNotAvailable() const { return lastNotAvailable_; }
  // An action that opens the "not available yet" message; it carries the "notAvailable" property.
  QAction* placeholderAction(const QString& text);

 Q_SIGNALS:
  void workspacesChanged();

 protected:
  void closeEvent(QCloseEvent* e) override;
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  int cloneMode_ = 0;  // last Clone Options choice (CloneOptionsDialog::Mode)
  void createActions();
  void createMenus();
  void createToolBar();
  void createAxisToolBar();
  void createDocks();
  void createStatusBar();
  void createNavigationControls(QGridLayout* grid);
  void createWorkspaceSelector();
  void resolveProxyCommands();
  QAction* proxyCommand(const QString& id, const QString& text, const QIcon& icon);
  // Mirrors `real` (enabled, checked, shortcut, triggers) on `proxy`, which shows `text`.
  void bindProxy(QAction* proxy, QAction* real, const QString& text);
  // Final 3ds Max order of the menu bar, main toolbar and axis toolbar (uilayout.cpp). Runs once
  // after every extension is installed.
  void arrangeInterface();
  void createLayoutCommands();
  void arrangeMenus();
  void arrangeMainToolBar();
  void arrangeAxisToolBar();
  // An action showing `text` that forwards to command `id` (for menus that label it differently).
  QAction* aliasAction(const QString& id, const QString& text);
  // Adds `a` to a toolbar, honoring the "toolMenu" (flyout menu) and "contextCommand" (right-click)
  // properties feature modules set on their actions.
  void addToolBarAction(QToolBar* tb, QAction* a);
  QAction* cmd(const QString& id, const QString& text, const QKeySequence& key = {}, const QIcon& icon = {});
  void updateTitle();
  void applyTypeIn(int axis, double value);
  void updateRecentMenu();
  void addRecent(const QString& path);
  bool maybeSave();
  bool save();
  bool saveAs();
  void importObjFile();
  void importObjFileFrom(const QString& path);
  void showContextMenu(Viewport* vp, const QPoint& pos);
  void updateShortcutOverride();
  void applyGridLabel();
  // File menu helpers (fileops.cpp).
  void createFileMenu(QMenu* file);
  void newSceneKeep(int mode);  // 0 new all, 1 keep objects, 2 keep objects and hierarchy
  void resetScene();
  bool saveCopyAs();
  bool saveSelected();
  bool archiveScene();
  void mergeFile(bool replace);
  void importFile();
  void exportFile(bool selectedOnly);
  void xrefScene();
  void viewImageFile();
  void showSummaryInfo();
  void showFileProperties();
  void setProjectFolder(bool create);
  void autoBackup();

  SceneDocument* doc_;
  EditorState* state_;
  ViewportPanel* viewports_ = nullptr;
  CommandPanel* commandPanel_ = nullptr;
  SceneExplorer* explorer_ = nullptr;
  LayerExplorer* layerExplorer_ = nullptr;
  QDockWidget* explorerDock_ = nullptr;
  QDockWidget* layerDock_ = nullptr;
  QDockWidget* commandDock_ = nullptr;
  QPointer<MaterialEditor> materialEditor_;
  QPointer<RenderSetupDialog> renderSetup_;
  QPointer<FrameBufferWindow> frameBuffer_;

  QAction *newAct_, *openAct_, *saveAct_, *saveAsAct_, *importAct_, *exitAct_;
  QAction *undoAct_, *redoAct_, *deleteAct_, *cloneAct_, *selectAllAct_, *selectNoneAct_, *selectInvertAct_,
      *selectByNameAct_, *hideAct_, *unhideAct_, *propertiesAct_;
  QAction *selectToolAct_, *moveToolAct_, *rotateToolAct_, *scaleToolAct_, *placeToolAct_;
  QAction *axisXAct_, *axisYAct_, *axisZAct_, *axisPlaneAct_, *axisCycleAct_;
  QAction *materialEditorAct_, *renderSetupAct_, *renderAct_, *frameBufferAct_;
  QAction *zoomExtAct_, *zoomExtAllAct_, *undoViewAct_, *maximizeAct_, *shadingAct_, *edgedAct_, *gridAct_,
      *cameraFromViewAct_;
  QAction *isolateAct_ = nullptr, *lockSelAct_ = nullptr, *expertAct_ = nullptr, *lockUiAct_ = nullptr;
  QActionGroup *toolGroup_, *axisGroup_;
  QMenu* recentMenu_ = nullptr;
  QToolBar* mainToolBar_ = nullptr;
  QToolBar* axisToolBar_ = nullptr;
  QVBoxLayout* centralLayout_ = nullptr;
  QHash<QString, QAction*> commands_;
  QHash<QString, QString> defaultShortcuts_;  // id -> shortcut at startup
  QList<QAction*> proxies_;
  QComboBox* filterCombo_ = nullptr;
  QComboBox* refCoordCombo_ = nullptr;
  QComboBox* selSetCombo_ = nullptr;
  QComboBox* workspaceCombo_ = nullptr;

  // Status bar.
  ScriptListener* miniListener_ = nullptr;
  QLabel* prompt_ = nullptr;
  QLabel* typeInLabel_ = nullptr;
  UnitSpinBox* typeIn_[3] = {};
  QToolButton* absOffsetBtn_ = nullptr;
  QLabel* selInfo_ = nullptr;
  QLabel* gridInfo_ = nullptr;
  QHBoxLayout* animSlot_ = nullptr;
  bool updatingTypeIn_ = false;
  bool spinnerSnap_ = false;
  bool pivotAuto_ = false;
  QActionGroup* navGroup_ = nullptr;

  // Edit state.
  json held_;
  bool hasHold_ = false;
  bool isolated_ = false;
  QVector<uint32_t> isolatedHidden_;
  QByteArray startupState_;
  QTimer* autoBackupTimer_ = nullptr;
  QHash<QString, QKeySequence> overriddenKeys_;
  QStringList lastImportWarnings_;
  QString lastNotAvailable_;
  QHash<QString, QAction*> aliases_;  // "id\ntext" -> alias action
};

}  // namespace veraa
