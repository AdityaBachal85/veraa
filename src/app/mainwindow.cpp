#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "commandpanel.h"
#include "commandtabs.h"
#include "createlayout.h"
#include "customize.h"
#include "dialogs.h"
#include "edittools.h"
#include "extension.h"
#include "framebuffer.h"
#include "icons.h"
#include "importjob.h"
#include "icons_ui.h"
#include "interfacedata.h"
#include "layerexplorer.h"
#include "materialeditor.h"
#include "navtools.h"
#include "paramform.h"
#include "project.h"
#include "rendersetup.h"
#include "sceneexplorer.h"
#include "scripting.h"
#include "scriptui.h"
#include "tooldialogs.h"
#include "uistate.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

namespace {
const char* kDefaultPrompt = "Click or click-and-drag to select objects";

QSettings appSettings() { return QSettings("Veraa", "Veraa"); }

const char* const kTopMenus[] = {"&File",      "&Edit",      "&Tools",     "&Group",     "&Views",
                                 "&Create",    "&Modifiers", "&Animation", "&Graph Editors",
                                 "&Rendering", "C&ustomize", "&Scripting", "&Help"};

QString plainMenuTitle(QString t) { return t.remove('&'); }

// Modifiers menu groups in 3ds Max order; unknown categories follow alphabetically.
const char* const kModifierCategories[] = {
    "Selection Modifiers",  "Patch/Spline Editing", "Mesh Editing",        "Conversion",
    "Animation",            "Cloth",                "Hair and Fur",        "UV Coordinates",
    "Cache Tools",          "Subdivision Surfaces", "Free Form Deformers", "Parametric Deformers",
    "Surface",              "NURBS Editing",        "Radiosity",           "Cameras",
    "Rendering"};  // shown as "Veraa" (the renderer's modifiers, in the reference's V-Ray slot)

// Command panel tab order.
int commandTabRank(const QString& title) {
  static const QStringList order = {"Create", "Modify", "Hierarchy", "Motion", "Display", "Utilities"};
  int i = order.indexOf(title);
  return i < 0 ? 100 : i;
}
}  // namespace

MainWindow::MainWindow() {
  doc_ = new SceneDocument(this);
  state_ = new EditorState(this);
  setWindowIcon(icons::app());
  setDockNestingEnabled(true);

  auto* central = new QWidget(this);
  auto* cv = new QVBoxLayout(central);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  viewports_ = new ViewportPanel(doc_, state_, central);
  cv->addWidget(viewports_, 1);
  centralLayout_ = cv;
  setCentralWidget(central);

  createActions();
  createMenus();
  createToolBar();
  createAxisToolBar();
  createDocks();
  createStatusBar();
  createWorkspaceSelector();

  connect(doc_, &SceneDocument::modifiedChanged, this, [this](bool) { updateTitle(); });
  connect(doc_, &SceneDocument::fileChanged, this, [this] { updateTitle(); });
  connect(doc_, &SceneDocument::selectionChanged, this, [this] { updateTypeIn(); });
  connect(doc_, &SceneDocument::structureChanged, this, [this] { updateTypeIn(); });
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int flags) {
    if (flags & ChangeTransform) updateTypeIn();
  });
  connect(state_, &EditorState::toolChanged, this, [this](TransformTool t) {
    QAction* a = t == TransformTool::Select ? selectToolAct_
                 : t == TransformTool::Move ? moveToolAct_
                 : t == TransformTool::Rotate ? rotateToolAct_
                                              : scaleToolAct_;
    a->setChecked(true);
    updateTypeIn();
  });
  connect(state_, &EditorState::constraintChanged, this, [this](GizmoHandle h) {
    int mask = handleAxisMask(h);
    QAction* a = mask == 1 ? axisXAct_ : mask == 2 ? axisYAct_ : mask == 4 ? axisZAct_ : axisPlaneAct_;
    if (a == axisPlaneAct_) {
      axisPlaneAct_->setText(handleName(h));
      axisPlaneAct_->setIcon(icons::axis(handleName(h)));
    }
    a->setChecked(true);
  });
  connect(state_, &EditorState::promptChanged, this, [this](const QString& t) { prompt_->setText(t); });
  connect(state_, &EditorState::createTypeChanged, this, [this](const QString& t) {
    if (t.isEmpty()) prompt_->setText(tr(kDefaultPrompt));
  });
  connect(state_, &EditorState::viewportToolChanged, this, [this] { updateShortcutOverride(); });
  connect(state_, &EditorState::showGridChanged, gridAct_, &QAction::setChecked);
  connect(viewports_, &ViewportPanel::activeChanged, this, [this](Viewport* vp) {
    QSignalBlocker b1(shadingAct_), b2(edgedAct_);
    shadingAct_->setChecked(vp->shading() == ShadingMode::Shaded);
    edgedAct_->setChecked(vp->edgedFaces());
  });
  connect(viewports_, &ViewportPanel::contextMenuRequested, this, &MainWindow::showContextMenu);
  connect(&UiState::instance(), &UiState::changed, this, [this] {
    viewports_->updateAll();
    updateShortcutOverride();
    if (lockSelAct_) {
      QSignalBlocker b(lockSelAct_);
      lockSelAct_->setChecked(UiState::instance().selectionLocked());
    }
  });
  connect(&Units::instance(), &Units::changed, this, [this] {
    applyGridLabel();
    updateTypeIn();
  });

  // Feature modules add their menus, toolbars and panels (see extension.h).
  runAppExtensions(*this);
  resolveProxyCommands();
  arrangeInterface();
  ScriptEngine::instance().attach(this);
  QTimer::singleShot(0, this, [] { ScriptEngine::instance().runStartupScripts(); });
  auto updateEnabled = [this] {
    bool has = !doc_->selection().isEmpty();
    for (const char* id : {"edit.clone", "edit.delete", "edit.selectSimilar", "edit.selectInstances",
                           "edit.objectProperties", "edit.hideSelection", "file.saveSelected", "file.exportSelected"})
      if (QAction* a = command(id)) a->setEnabled(has);
  };
  connect(doc_, &SceneDocument::selectionChanged, this, updateEnabled);
  updateEnabled();
  undoAct_->setEnabled(false);
  redoAct_->setEnabled(false);
  connect(doc_->undoStack(), &QUndoStack::canUndoChanged, undoAct_, &QAction::setEnabled);
  connect(doc_->undoStack(), &QUndoStack::canRedoChanged, redoAct_, &QAction::setEnabled);
  command("edit.fetch")->setEnabled(QFileInfo::exists(QDir(projectSubfolder("autoback")).filePath("veraa_hold.veraa")));
  connect(viewports_, &ViewportPanel::configureRequested, this, [this](int tab) {
    ViewportConfigDialog dlg(viewports_, this, tab);
    dlg.exec();
  });
  if (QAction* adaptive = command("views.adaptiveDegradation"))
    connect(&UiState::instance(), &UiState::changed, adaptive, [adaptive] {
      QSignalBlocker b(adaptive);
      adaptive->setChecked(UiState::instance().adaptiveDegradation());
    });

  // Remember the shipped shortcuts, then apply the user's hotkeys.
  for (auto it = commands_.begin(); it != commands_.end(); ++it)
    defaultShortcuts_.insert(it.key(), it.value()->shortcut().toString(QKeySequence::PortableText));
  {
    QSettings s = appSettings();
    QHash<QString, QString> keys;
    s.beginGroup("hotkeys");
    for (const QString& k : s.childKeys()) keys.insert(k, s.value(k).toString());
    s.endGroup();
    if (!keys.isEmpty()) applyHotkeys(keys, false);
  }

  resize(1600, 950);
  startupState_ = saveState();
  QSettings s = appSettings();
  restoreGeometry(s.value("geometry").toByteArray());
  restoreState(s.value("windowState").toByteArray());
  setUiLocked(s.value("ui/locked", false).toBool());
  if (int px = s.value("ui/toolbarIconSize", 0).toInt(); px >= 16) mainToolBar_->setIconSize(QSize(px, px));

  autoBackupTimer_ = new QTimer(this);
  connect(autoBackupTimer_, &QTimer::timeout, this, &MainWindow::autoBackup);
  int minutes = s.value("prefs/autoBackupMinutes", 5).toInt();
  if (s.value("prefs/autoBackup", true).toBool() && minutes > 0) autoBackupTimer_->start(minutes * 60 * 1000);

  qApp->installEventFilter(this);
  updateTitle();
  updateTypeIn();
  updateRecentMenu();
  applyGridLabel();
}

MainWindow::~MainWindow() { qApp->removeEventFilter(this); }

// ---------------------------------------------------------------------------------------------
// Extension API

QMenu* MainWindow::menuAt(const QString& path) {
  QStringList parts = path.split('/', Qt::SkipEmptyParts);
  if (parts.isEmpty()) return nullptr;
  QMenu* menu = nullptr;
  for (QAction* a : menuBar()->actions())
    if (a->menu() && plainMenuTitle(a->text()) == plainMenuTitle(parts[0])) menu = a->menu();
  if (!menu) {
    // Unknown top-level menu: insert before Help.
    QAction* help = nullptr;
    for (QAction* a : menuBar()->actions())
      if (a->menu() && plainMenuTitle(a->text()) == "Help") help = a;
    menu = new QMenu(parts[0], menuBar());
    menuBar()->insertMenu(help, menu);
  }
  for (int i = 1; i < parts.size(); ++i) {
    QMenu* sub = nullptr;
    for (QAction* a : menu->actions())
      if (a->menu() && plainMenuTitle(a->text()) == plainMenuTitle(parts[i])) sub = a->menu();
    if (!sub) sub = menu->addMenu(parts[i]);
    menu = sub;
  }
  return menu;
}

void MainWindow::addBelowViewports(QWidget* w) {
  w->setParent(centralWidget());
  centralLayout_->addWidget(w, 0);
}

void MainWindow::addCommandPanelTab(QWidget* page, const QIcon& icon, const QString& title) {
  int rank = commandTabRank(title);
  int at = commandPanel_->count();
  for (int i = 0; i < commandPanel_->count(); ++i)
    if (commandTabRank(commandPanel_->tabToolTip(i)) > rank) {
      at = i;
      break;
    }
  commandPanel_->insertTab(at, page, icon, QString());
  commandPanel_->setTabToolTip(at, title);
}

void MainWindow::addStatusBarWidget(QWidget* w) {
  if (!animSlot_) return;
  w->setParent(animSlot_->parentWidget());
  animSlot_->addWidget(w);
}

void MainWindow::registerCommand(const QString& id, QAction* action) {
  action->setObjectName(id);
  action->setProperty("commandId", id);
  commands_.insert(id, action);
  if (!action->parent() || action->parent() != this) addAction(action);  // shortcuts window-wide
}

QAction* MainWindow::cmd(const QString& id, const QString& text, const QKeySequence& key, const QIcon& icon) {
  auto* a = new QAction(icon, text, this);
  if (!key.isEmpty()) a->setShortcut(key);
  addAction(a);  // shortcuts work even when menus are hidden
  registerCommand(id, a);
  return a;
}

void MainWindow::setPrompt(const QString& text) { prompt_->setText(text); }

// ---------------------------------------------------------------------------------------------
// Actions

void MainWindow::createActions() {
  // ---- File ----
  newAct_ = cmd("file.new", tr("&New All"), QKeySequence::New, icons::newFile());
  connect(newAct_, &QAction::triggered, this, [this] { newSceneKeep(0); });
  openAct_ = cmd("file.open", tr("&Open..."), QKeySequence::Open, icons::open());
  connect(openAct_, &QAction::triggered, this, [this] {
    if (!maybeSave()) return;
    QString dir = doc_->filePath().isEmpty() ? projectSubfolder("scenes") : QFileInfo(doc_->filePath()).absolutePath();
    QString f = QFileDialog::getOpenFileName(this, tr("Open Scene"), dir,
                                             tr("Scenes (*.veraa *.max);;Veraa scenes (*.veraa);;3ds Max scenes (*.max);;All files (*)"));
    if (!f.isEmpty()) openFile(f);
  });
  saveAct_ = cmd("file.save", tr("&Save"), QKeySequence::Save, icons::save());
  connect(saveAct_, &QAction::triggered, this, [this] { save(); });
  saveAsAct_ = cmd("file.saveAs", tr("Save &As..."), QKeySequence("Ctrl+Shift+S"));
  connect(saveAsAct_, &QAction::triggered, this, [this] { saveAs(); });
  importAct_ = cmd("file.import", tr("&Import..."), QKeySequence(), icons::import());
  connect(importAct_, &QAction::triggered, this, &MainWindow::importFile);
  exitAct_ = cmd("file.exit", tr("E&xit"), QKeySequence::Quit);
  connect(exitAct_, &QAction::triggered, this, &QWidget::close);

  // ---- Edit ----
  undoAct_ = cmd("edit.undo", tr("&Undo"), QKeySequence::Undo, icons::undo());
  connect(undoAct_, &QAction::triggered, this, [this] {
    viewports_->active()->cancelInteraction();
    doc_->commitSoftEdit();
    doc_->undoStack()->undo();
  });
  redoAct_ = cmd("edit.redo", tr("&Redo"), QKeySequence("Ctrl+Y"), icons::redo());
  connect(redoAct_, &QAction::triggered, this, [this] {
    doc_->commitSoftEdit();
    doc_->undoStack()->redo();
  });
  connect(doc_->undoStack(), &QUndoStack::undoTextChanged, this,
          [this](const QString& t) { undoAct_->setToolTip(t.isEmpty() ? tr("Undo") : tr("Undo %1").arg(t)); });
  connect(doc_->undoStack(), &QUndoStack::redoTextChanged, this,
          [this](const QString& t) { redoAct_->setToolTip(t.isEmpty() ? tr("Redo") : tr("Redo %1").arg(t)); });
  QAction* holdAct = cmd("edit.hold", tr("&Hold"), QKeySequence("Ctrl+H"));
  connect(holdAct, &QAction::triggered, this, &MainWindow::hold);
  QAction* fetchAct = cmd("edit.fetch", tr("&Fetch"), QKeySequence("Alt+Ctrl+F"));
  connect(fetchAct, &QAction::triggered, this, [this] {
    if (QMessageBox::question(this, tr("Fetch"), tr("About to fetch the held scene. OK?")) == QMessageBox::Yes)
      fetch();
  });

  deleteAct_ = cmd("edit.delete", tr("&Delete"), QKeySequence::Delete, icons::trash());
  connect(deleteAct_, &QAction::triggered, this, [this] {
    if (auto tool = state_->viewportToolShared(); tool && tool->deleteSelection()) return;
    doc_->deleteSelected();
  });
  cloneAct_ = cmd("edit.clone", tr("&Clone"), QKeySequence("Ctrl+V"));
  connect(cloneAct_, &QAction::triggered, this, [this] { cloneSelection(true); });
  selectAllAct_ = cmd("edit.selectAll", tr("Select &All"), QKeySequence("Ctrl+A"));
  connect(selectAllAct_, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    const Scene& s = doc_->scene();
    for (const auto& n : s.nodes)
      if (s.isNodeVisible(*n) && !s.isNodeFrozen(*n) && state_->selectionFilterAccepts(*n)) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  selectNoneAct_ = cmd("edit.selectNone", tr("Select &None"), QKeySequence("Ctrl+D"));
  connect(selectNoneAct_, &QAction::triggered, this, [this] { doc_->clearSelection(); });
  selectInvertAct_ = cmd("edit.selectInvert", tr("Select &Invert"), QKeySequence("Ctrl+I"));
  connect(selectInvertAct_, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    const Scene& s = doc_->scene();
    for (const auto& n : s.nodes)
      if (s.isNodeVisible(*n) && !s.isNodeFrozen(*n) && state_->selectionFilterAccepts(*n) &&
          !doc_->isSelected(n->id))
        ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  QAction* similar = cmd("edit.selectSimilar", tr("Select Simi&lar"), QKeySequence("Ctrl+Shift+A"));
  connect(similar, &QAction::triggered, this, &MainWindow::selectSimilar);
  QAction* instances = cmd("edit.selectInstances", tr("Select Ins&tances"));
  connect(instances, &QAction::triggered, this, &MainWindow::selectInstances);
  selectByNameAct_ = cmd("edit.selectByName", tr("Select by &Name..."), QKeySequence("H"), icons::selectByName());
  connect(selectByNameAct_, &QAction::triggered, this, [this] {
    SelectByNameDialog dlg(doc_, this);
    if (dlg.exec() == QDialog::Accepted) doc_->setSelection(dlg.chosen());
  });
  QAction* byColor = cmd("edit.selectByColor", tr("&Color"));
  connect(byColor, &QAction::triggered, this, [this] {
    Node* ref = doc_->firstSelected();
    QColor start = ref ? QColor::fromRgbF(ref->wireColor.x, ref->wireColor.y, ref->wireColor.z) : QColor(Qt::white);
    QColor c = ref ? start : QColorDialog::getColor(start, this, tr("Select by Color"));
    if (!c.isValid()) return;
    Vec3 want(static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF()));
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (length(n->wireColor - want) < 0.01f && doc_->scene().isNodeVisible(*n)) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  QAction* byMaterial = cmd("edit.selectByMaterial", tr("&Material"));
  connect(byMaterial, &QAction::triggered, this, [this] {
    Node* ref = doc_->firstSelected();
    if (!ref) {
      setPrompt(tr("Select an object first; objects with the same material get selected"));
      return;
    }
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (n->materialId == ref->materialId && doc_->scene().isNodeVisible(*n)) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  QAction* byLayer = cmd("edit.selectByLayer", tr("&Layer"));
  connect(byLayer, &QAction::triggered, this, [this] {
    QMenu menu(this);
    for (const Layer& l : doc_->scene().layers) {
      uint32_t id = l.id;
      menu.addAction(QString::fromStdString(l.name), this, [this, id] {
        QVector<uint32_t> ids;
        for (const auto& n : doc_->scene().nodes)
          if (n->layerId == id && doc_->scene().isNodeVisible(*n)) ids.push_back(n->id);
        doc_->setSelection(ids);
      });
    }
    menu.exec(QCursor::pos());
  });
  hideAct_ = cmd("edit.hideSelection", tr("&Hide Selection"));
  connect(hideAct_, &QAction::triggered, this, [this] { doc_->hideSelected(); });
  unhideAct_ = cmd("edit.unhideAll", tr("&Unhide All"));
  connect(unhideAct_, &QAction::triggered, this, [this] { doc_->unhideAll(); });
  propertiesAct_ = cmd("edit.objectProperties", tr("Object &Properties..."));
  connect(propertiesAct_, &QAction::triggered, this, [this] {
    auto nodes = doc_->selectedNodes();
    if (nodes.isEmpty()) return;
    ObjectPropertiesDialog dlg(doc_, nodes[0], this);
    dlg.exec();
  });
  QAction* typeIn = cmd("edit.transformTypeIn", tr("&Transform Type-In..."), QKeySequence("F12"));
  connect(typeIn, &QAction::triggered, this, &MainWindow::openTransformTypeIn);
  QAction* toolbox = cmd("edit.transformToolbox", tr("Transform Toolbo&x..."));
  connect(toolbox, &QAction::triggered, this, [this] {
    auto* tb = findChild<TransformToolbox*>();
    if (!tb) tb = new TransformToolbox(this);
    tb->show();
    tb->raise();
  });
  QAction* sets = cmd("edit.selectionSets", tr("Manage Selection &Sets..."));
  connect(sets, &QAction::triggered, this, [this] {
    SelectionSetsDialog dlg(doc_, this);
    dlg.exec();
  });

  // Transform tools.
  toolGroup_ = new QActionGroup(this);
  auto toolAct = [&](const QString& id, const QString& text, const QString& key, const QIcon& icon, TransformTool t) {
    QAction* a = cmd(id, text, QKeySequence(key), icon);
    a->setCheckable(true);
    a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key));
    toolGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, t] {
      state_->setViewportTool(nullptr);
      state_->setTool(t);
    });
    return a;
  };
  selectToolAct_ = toolAct("edit.select", tr("Select Object"), "Q", icons::select(), TransformTool::Select);
  moveToolAct_ = toolAct("edit.move", tr("Select and Move"), "W", icons::move(), TransformTool::Move);
  rotateToolAct_ = toolAct("edit.rotate", tr("Select and Rotate"), "E", icons::rotate(), TransformTool::Rotate);
  scaleToolAct_ = toolAct("edit.scale", tr("Select and Uniform Scale"), "R", icons::scale(), TransformTool::Scale);
  moveToolAct_->setText(tr("Move"));
  rotateToolAct_->setText(tr("Rotate"));
  scaleToolAct_->setText(tr("Scale"));
  connect(scaleToolAct_, &QAction::triggered, this, [] { UiState::instance().setScaleMode(ScaleMode::Uniform); });
  placeToolAct_ = cmd("edit.placement", tr("Placement"), QKeySequence(), icons::place());
  placeToolAct_->setToolTip(tr("Select and Place"));
  placeToolAct_->setCheckable(true);
  toolGroup_->addAction(placeToolAct_);
  connect(placeToolAct_, &QAction::triggered, this, [this] {
    state_->setTool(TransformTool::Select);
    state_->setViewportTool(makePlacementTool(doc_, state_));
    placeToolAct_->setChecked(true);
  });
  selectToolAct_->setChecked(true);
  for (auto [id, text, mode] : {std::tuple{"edit.scaleNonUniform", tr("Select and Non-uniform Scale"), ScaleMode::NonUniform},
                                std::tuple{"edit.scaleSquash", tr("Select and Squash"), ScaleMode::Squash}}) {
    QAction* a = cmd(id, text, QKeySequence(), mode == ScaleMode::NonUniform ? icons::scaleNonUniform() : icons::squash());
    ScaleMode m = mode;
    connect(a, &QAction::triggered, this, [this, m] {
      state_->setViewportTool(nullptr);
      UiState::instance().setScaleMode(m);
      state_->setTool(TransformTool::Scale);
      scaleToolAct_->setChecked(true);
    });
  }

  // Selection regions.
  auto* regionGroup = new QActionGroup(this);
  const struct {
    const char* id;
    const char* text;
    RegionShape shape;
  } regions[] = {{"edit.regionRectangle", "Rectangular Selection Region", RegionShape::Rectangle},
                 {"edit.regionCircle", "Circular Selection Region", RegionShape::Circle},
                 {"edit.regionFence", "Fence Selection Region", RegionShape::Fence},
                 {"edit.regionLasso", "Lasso Selection Region", RegionShape::Lasso},
                 {"edit.regionPaint", "Paint Selection Region", RegionShape::Paint}};
  for (const auto& r : regions) {
    QAction* a = cmd(r.id, tr(r.text), QKeySequence(), icons::region(static_cast<int>(r.shape)));
    a->setCheckable(true);
    a->setChecked(r.shape == RegionShape::Rectangle);
    regionGroup->addAction(a);
    RegionShape shape = r.shape;
    connect(a, &QAction::triggered, this, [shape] { UiState::instance().setRegionShape(shape); });
  }
  QAction* windowCrossing = cmd("edit.windowCrossing", tr("Window/Crossing"), QKeySequence(), icons::windowCrossing(false));
  windowCrossing->setCheckable(true);
  connect(windowCrossing, &QAction::toggled, this, [windowCrossing](bool on) {
    UiState::instance().setWindowSelection(on);
    windowCrossing->setIcon(icons::windowCrossing(on));
    windowCrossing->setToolTip(on ? tr("Window selection: objects must be fully inside")
                                  : tr("Crossing selection: objects touching the region are selected"));
  });
  lockSelAct_ = cmd("edit.lockSelection", tr("Selection Lock Toggle"), QKeySequence(Qt::Key_Space), icons::lock());
  lockSelAct_->setCheckable(true);
  connect(lockSelAct_, &QAction::toggled, this, [](bool on) { UiState::instance().setSelectionLocked(on); });

  // Axis constraints.
  axisGroup_ = new QActionGroup(this);
  auto axisAct = [&](const QString& id, const QString& label, const QString& key, GizmoHandle h) {
    QAction* a = cmd(id, label, QKeySequence(key), icons::axis(label));
    a->setCheckable(true);
    a->setToolTip(tr("Restrict to %1 (%2)").arg(label, key));
    axisGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, h] { state_->setConstraint(h); });
    return a;
  };
  axisXAct_ = axisAct("axis.x", "X", "F5", GizmoHandle::X);
  axisYAct_ = axisAct("axis.y", "Y", "F6", GizmoHandle::Y);
  axisZAct_ = axisAct("axis.z", "Z", "F7", GizmoHandle::Z);
  axisPlaneAct_ = axisAct("axis.plane", "XY", "", GizmoHandle::XY);
  axisCycleAct_ = cmd("axis.cycle", tr("Cycle Planes"), QKeySequence("F8"), icons::axisCycle());
  axisCycleAct_->setToolTip(tr("Cycle XY / YZ / ZX plane constraints (F8)"));
  connect(axisCycleAct_, &QAction::triggered, this, [this] {
    // F8 cycles XY -> YZ -> ZX like 3ds Max.
    GizmoHandle c = state_->constraint();
    GizmoHandle next = c == GizmoHandle::XY ? GizmoHandle::YZ : c == GizmoHandle::YZ ? GizmoHandle::ZX : GizmoHandle::XY;
    if (handleAxisMask(c) == 1 || handleAxisMask(c) == 2 || handleAxisMask(c) == 4) next = GizmoHandle::XY;
    state_->setConstraint(next);
  });
  axisPlaneAct_->setChecked(true);

  // ---- Rendering ----
  materialEditorAct_ = cmd("render.materialEditor", tr("&Material Editor..."), QKeySequence("M"), icons::materialEditor());
  connect(materialEditorAct_, &QAction::triggered, this, [this] {
    MaterialEditor* me = materialEditor();
    me->show();
    me->raise();
    me->activateWindow();
    if (Node* n = doc_->firstSelected(); n && n->materialId) me->selectMaterial(n->materialId);
  });
  renderSetupAct_ = cmd("render.setup", tr("Render &Setup..."), QKeySequence("F10"), icons::renderSetup());
  connect(renderSetupAct_, &QAction::triggered, this, [this] {
    RenderSetupDialog* rs = renderSetup();
    rs->show();
    rs->raise();
    rs->activateWindow();
  });
  renderAct_ = cmd("render.render", tr("&Render"), QKeySequence("Shift+Q"), icons::render());
  renderAct_->setShortcuts({QKeySequence("Shift+Q"), QKeySequence("F9")});
  renderAct_->setToolTip(tr("Render Production (Shift+Q)"));
  connect(renderAct_, &QAction::triggered, this, &MainWindow::render);
  frameBufferAct_ = cmd("render.frameWindow", tr("Rendered &Frame Window..."), QKeySequence(), icons::frameWindow());
  connect(frameBufferAct_, &QAction::triggered, this, [this] {
    FrameBufferWindow* fb = frameBuffer();
    fb->show();
    fb->raise();
  });

  // ---- Views ----
  zoomExtAct_ = cmd("views.zoomExtentsSelected", tr("Zoom &Extents Selected"), QKeySequence("Z"), icons::zoomExtents());
  connect(zoomExtAct_, &QAction::triggered, this, [this] { viewports_->active()->zoomExtents(true); });
  zoomExtAllAct_ = cmd("views.zoomExtentsAll", tr("Zoom Extents &All"), QKeySequence("Ctrl+Shift+Z"), icons::zoomExtentsAll());
  connect(zoomExtAllAct_, &QAction::triggered, this, [this] { viewports_->zoomExtentsAll(false); });
  undoViewAct_ = cmd("views.undoView", tr("&Undo View Change"), QKeySequence("Shift+Z"));
  connect(undoViewAct_, &QAction::triggered, this, [this] { viewports_->active()->undoViewChange(); });
  QAction* redoView = cmd("views.redoView", tr("&Redo View Change"), QKeySequence("Shift+Y"));
  connect(redoView, &QAction::triggered, this, [this] { viewports_->active()->redoViewChange(); });
  maximizeAct_ = cmd("views.maximize", tr("&Maximize Viewport Toggle"), QKeySequence("Alt+W"), icons::maximize());
  connect(maximizeAct_, &QAction::triggered, viewports_, &ViewportPanel::toggleMaximize);
  shadingAct_ = cmd("views.defaultShading", tr("&Default Shading"), QKeySequence("F3"));
  shadingAct_->setCheckable(true);
  connect(shadingAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    vp->setShading(vp->shading() == ShadingMode::Wireframe ? ShadingMode::Shaded : ShadingMode::Wireframe);
    shadingAct_->setChecked(vp->shading() != ShadingMode::Wireframe);
  });
  edgedAct_ = cmd("views.edgedFaces", tr("&Edged Faces"), QKeySequence("F4"));
  edgedAct_->setCheckable(true);
  connect(edgedAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    vp->setEdgedFaces(!vp->edgedFaces());
    edgedAct_->setChecked(vp->edgedFaces());
  });
  gridAct_ = cmd("views.grid", tr("Show Home &Grid"), QKeySequence("G"));
  gridAct_->setCheckable(true);
  gridAct_->setChecked(true);
  connect(gridAct_, &QAction::triggered, this, [this](bool on) { state_->setShowGrid(on); });
  cameraFromViewAct_ = cmd("views.cameraFromView", tr("Create Physical Camera From &View"), QKeySequence("Ctrl+C"));
  connect(cameraFromViewAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    if (vp->camera().type != ViewType::Perspective) {
      setPrompt(tr("Create Camera From View needs an active Perspective viewport"));
      return;
    }
    vp->createCameraFromView();
  });

  // Viewport view switching (T, B, F, L, P, U, C).
  const struct {
    const char* key;
    const char* id;
    ViewType type;
  } views[] = {{"T", "views.top", ViewType::Top},          {"B", "views.bottom", ViewType::Bottom},
               {"F", "views.front", ViewType::Front},      {"L", "views.left", ViewType::Left},
               {"P", "views.perspective", ViewType::Perspective}, {"U", "views.orthographic", ViewType::User}};
  for (const auto& v : views) {
    QAction* a = cmd(v.id, viewTypeName(v.type) + tr(" View"), QKeySequence(v.key));
    ViewType t = v.type;
    connect(a, &QAction::triggered, this, [this, t] { viewports_->active()->setViewType(t); });
  }
  QAction* camView = cmd("views.camera", tr("Camera View"), QKeySequence("C"));
  connect(camView, &QAction::triggered, this, [this] {
    std::vector<Node*> cams;
    for (const auto& n : doc_->scene().nodes)
      if (n->camera()) cams.push_back(n.get());
    Node* sel = doc_->firstSelected();
    if (sel && sel->camera()) {
      viewports_->active()->setCameraView(sel->id);
    } else if (cams.size() == 1) {
      viewports_->active()->setCameraView(cams[0]->id);
    } else if (cams.empty()) {
      setPrompt(tr("There are no cameras in the scene (create one with Ctrl+C)"));
    } else {
      QMenu menu(this);
      for (Node* c : cams) {
        uint32_t id = c->id;
        menu.addAction(QString::fromStdString(c->name), this, [this, id] { viewports_->active()->setCameraView(id); });
      }
      menu.exec(QCursor::pos());
    }
  });

  QAction* esc = cmd("edit.cancel", tr("Cancel"), QKeySequence(Qt::Key_Escape));
  connect(esc, &QAction::triggered, this, [this] {
    bool cancelled = false;
    for (int i = 0; i < viewports_->count(); ++i) cancelled |= viewports_->viewport(i)->cancelInteraction();
    if (!cancelled && state_->viewportTool()) {
      state_->setViewportTool(nullptr);
      cancelled = true;
    }
    if (!cancelled && state_->creating()) state_->setCreateType({});
  });
}

// ---------------------------------------------------------------------------------------------
// Menus

void MainWindow::createMenus() {
  for (const char* title : kTopMenus) menuBar()->addMenu(tr(title));
  createFileMenu(menuAt("File"));

  // ---- Edit (screenshot order) ----
  QMenu* edit = menuAt("Edit");
  edit->addAction(undoAct_);
  edit->addAction(redoAct_);
  edit->addSeparator();
  edit->addAction(command("edit.hold"));
  edit->addAction(command("edit.fetch"));
  edit->addSeparator();
  edit->addAction(deleteAct_);
  edit->addAction(cloneAct_);
  edit->addSeparator();
  edit->addAction(moveToolAct_);
  edit->addAction(rotateToolAct_);
  edit->addAction(scaleToolAct_);
  edit->addAction(placeToolAct_);
  edit->addAction(command("edit.transformTypeIn"));
  edit->addAction(command("edit.transformToolbox"));
  edit->addSeparator();
  edit->addAction(selectAllAct_);
  edit->addAction(selectNoneAct_);
  edit->addAction(selectInvertAct_);
  edit->addSeparator();
  edit->addAction(command("edit.selectSimilar"));
  edit->addAction(command("edit.selectInstances"));
  QMenu* selBy = edit->addMenu(tr("Select &By"));
  selBy->addAction(command("edit.selectByColor"));
  selBy->addAction(selectByNameAct_);
  selBy->addAction(command("edit.selectByLayer"));
  selBy->addAction(command("edit.selectByMaterial"));
  QMenu* region = edit->addMenu(tr("Selection &Region"));
  for (const char* id : {"edit.regionRectangle", "edit.regionCircle", "edit.regionFence", "edit.regionLasso",
                         "edit.regionPaint"})
    region->addAction(command(id));
  region->addSeparator();
  region->addAction(command("edit.windowCrossing"));
  edit->addSeparator();
  edit->addAction(command("edit.selectionSets"));
  edit->addSeparator();
  edit->addAction(propertiesAct_);

  // ---- Tools (screenshot order) ----
  QMenu* tools = menuAt("Tools");
  QAction* explorerAct = cmd("tools.sceneExplorer", tr("Scene E&xplorer..."), QKeySequence(), icons::sceneExplorer());
  connect(explorerAct, &QAction::triggered, this, [this] {
    explorerDock_->show();
    explorerDock_->raise();
  });
  tools->addAction(explorerAct);
  QAction* layerAct = cmd("tools.layerExplorer", tr("&Layer Explorer..."), QKeySequence(), icons::layerExplorer());
  connect(layerAct, &QAction::triggered, this, [this] {
    layerDock_->show();
    layerDock_->raise();
  });
  tools->addAction(layerAct);
  tools->addSeparator();
  isolateAct_ = cmd("tools.isolate", tr("&Isolate Selection"), QKeySequence("Alt+Q"));
  isolateAct_->setCheckable(true);
  connect(isolateAct_, &QAction::triggered, this, [this](bool on) { isolateSelection(on); });
  tools->addAction(isolateAct_);
  QAction* endIsolate = cmd("tools.endIsolate", tr("End Isolate"));
  connect(endIsolate, &QAction::triggered, this, [this] { isolateSelection(false); });
  tools->addAction(endIsolate);
  tools->addSeparator();
  QAction* floater = cmd("tools.displayFloater", tr("&Display Floater..."));
  connect(floater, &QAction::triggered, this, [this] {
    auto* f = findChild<DisplayFloater*>();
    if (!f) f = new DisplayFloater(this);
    f->show();
    f->raise();
  });
  tools->addAction(floater);
  tools->addSeparator();
  QAction* states = cmd("tools.sceneStates", tr("Manage Scene &States..."));
  connect(states, &QAction::triggered, this, [this] {
    SceneStatesDialog dlg(doc_, this);
    dlg.exec();
  });
  tools->addAction(states);
  tools->addSeparator();
  QAction* lister = cmd("tools.lightLister", tr("Light Lis&ter..."));
  connect(lister, &QAction::triggered, this, [this] {
    auto* l = findChild<LightListerDialog*>();
    if (!l) l = new LightListerDialog(doc_, this);
    l->show();
    l->raise();
  });
  tools->addAction(lister);
  tools->addSeparator();
  tools->addAction(proxyCommand("edit.mirror", tr("&Mirror..."), icons::mirror()));
  tools->addAction(proxyCommand("tools.array", tr("A&rray..."), icons::array()));
  QMenu* align = tools->addMenu(tr("Align"));
  align->addAction(proxyCommand("tools.align", tr("Align..."), icons::align()));
  align->addAction(proxyCommand("tools.quickAlign", tr("Quick Align"), icons::align()));
  align->addAction(proxyCommand("tools.normalAlign", tr("Normal Align..."), icons::align()));
  align->addAction(proxyCommand("tools.alignToView", tr("Align to View..."), icons::align()));
  tools->addAction(proxyCommand("tools.snapshot", tr("Sna&pshot..."), QIcon()));
  tools->addAction(proxyCommand("tools.spacing", tr("Spacing Tool..."), QIcon()));
  tools->addSeparator();
  QAction* rename = cmd("tools.renameObjects", tr("Rena&me Objects..."));
  connect(rename, &QAction::triggered, this, [this] {
    RenameObjectsDialog dlg(doc_, this);
    dlg.exec();
  });
  tools->addAction(rename);
  tools->addSeparator();
  QAction* clipboard = cmd("tools.colorClipboard", tr("C&olor Clipboard..."));
  connect(clipboard, &QAction::triggered, this, [this] {
    auto* c = findChild<ColorClipboard*>();
    if (!c) c = new ColorClipboard(doc_, this);
    c->show();
    c->raise();
  });
  tools->addAction(clipboard);
  tools->addSeparator();
  QMenu* preview = tools->addMenu(tr("Preview - Grab Viewport"));
  preview->addAction(proxyCommand("tools.previewGrab", tr("Create Preview Animation..."), QIcon()));
  QAction* grabImage = cmd("tools.grabViewport", tr("Capture Viewport to Image..."));
  connect(grabImage, &QAction::triggered, this, [this] {
    QImage img = viewports_->active()->grabFramebuffer();
    QString f = QFileDialog::getSaveFileName(this, tr("Capture Viewport"), projectSubfolder("renderoutput"),
                                             tr("PNG image (*.png);;JPEG image (*.jpg)"));
    if (!f.isEmpty() && !img.save(f)) QMessageBox::warning(this, tr("Capture Viewport"), tr("Could not save %1").arg(f));
  });
  preview->addAction(grabImage);
  QMenu* grids = tools->addMenu(tr("Grids and Snaps"));
  grids->addAction(proxyCommand("snaps.toggle", tr("Snaps Toggle"), icons::snap(3)));
  grids->addAction(proxyCommand("snaps.angle", tr("Angle Snap Toggle"), icons::angleSnap()));
  grids->addAction(proxyCommand("snaps.percent", tr("Percent Snap Toggle"), icons::percentSnap()));
  grids->addAction(proxyCommand("snaps.settings", tr("Grid and Snap Settings..."), QIcon()));
  QAction* measure = cmd("tools.measureDistance", tr("Measure Distance"));
  connect(measure, &QAction::triggered, this, [this] {
    state_->setTool(TransformTool::Select);
    state_->setViewportTool(makeMeasureTool(doc_, state_));
  });
  tools->addAction(measure);
  tools->addSeparator();
  QAction* inspector = cmd("tools.meshInspector", tr("Mesh Inspector..."));
  connect(inspector, &QAction::triggered, this, [this] {
    MeshInspectorDialog dlg(doc_, this);
    dlg.exec();
  });
  tools->addAction(inspector);
  tools->addSeparator();
  QAction* convert = cmd("tools.convertToPoly", tr("Convert to Editable Poly"));
  connect(convert, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    for (Node* n : doc_->selectedNodes())
      if (n->kind() == ObjectKind::Geometry || (n->kind() == ObjectKind::Shape && !n->mesh().empty()))
        ids.push_back(n->id);
    if (ids.isEmpty()) return;
    doc_->beginEdit(tr("Convert to Editable Poly"));
    for (uint32_t id : ids) {
      Node* n = doc_->scene().findNode(id);
      auto poly = std::make_unique<EditablePolyObject>();
      poly->setMesh(n->mesh());
      n->object = std::move(poly);
      n->modifiers.clear();
      n->invalidate();
    }
    doc_->notifyStructureChanged();  // first: panels drop references to the replaced objects
    doc_->notifyNodesChanged(ids, ChangeGeometry);
    doc_->endEdit();
  });
  tools->addAction(convert);

  // ---- Group (commands owned by the modeling module) ----
  QMenu* group = menuAt("Group");
  group->addAction(proxyCommand("group.group", tr("&Group..."), QIcon()));
  group->addAction(proxyCommand("group.ungroup", tr("&Ungroup"), QIcon()));
  group->addSeparator();
  group->addAction(proxyCommand("group.open", tr("&Open"), QIcon()));
  group->addAction(proxyCommand("group.openRecursively", tr("Open &Recursively"), QIcon()));
  group->addAction(proxyCommand("group.close", tr("&Close"), QIcon()));
  group->addSeparator();
  group->addAction(proxyCommand("group.attach", tr("&Attach"), QIcon()));
  group->addAction(proxyCommand("group.detach", tr("&Detach"), QIcon()));
  group->addSeparator();
  group->addAction(proxyCommand("group.explode", tr("&Explode"), QIcon()));

  // ---- Views ----
  QMenu* views = menuAt("Views");
  views->addAction(undoViewAct_);
  views->addAction(command("views.redoView"));
  views->addSeparator();
  QAction* vpConfig = cmd("views.viewportConfiguration", tr("Viewport &Configuration..."));
  connect(vpConfig, &QAction::triggered, this, [this] {
    ViewportConfigDialog dlg(viewports_, this);
    dlg.exec();
  });
  views->addAction(vpConfig);
  QMenu* setActive = views->addMenu(tr("Set Active Viewport"));
  for (const char* id : {"views.perspective", "views.orthographic", "views.top", "views.bottom", "views.front",
                         "views.left", "views.camera"})
    setActive->addAction(command(id));
  QAction* saveView = cmd("views.saveActiveView", tr("&Save Active View"));
  connect(saveView, &QAction::triggered, this, [this] { viewports_->active()->saveNamedView(); });
  QAction* restoreView = cmd("views.restoreActiveView", tr("&Restore Active View"));
  connect(restoreView, &QAction::triggered, this, [this] { viewports_->active()->restoreNamedView(); });
  views->addAction(saveView);
  views->addAction(restoreView);
  views->addSeparator();
  QMenu* gridsMenu = views->addMenu(tr("&Grids"));
  gridsMenu->addAction(gridAct_);
  QAction* bg = cmd("views.background", tr("Viewport &Background..."), QKeySequence("Alt+B"));
  connect(bg, &QAction::triggered, this, [this] {
    ViewportConfigDialog dlg(viewports_, this, 2);
    dlg.exec();
  });
  views->addAction(bg);
  QAction* showGizmo = cmd("views.showGizmo", tr("Show Transform &Gizmo"), QKeySequence("X"));
  showGizmo->setCheckable(true);
  showGizmo->setChecked(true);
  connect(showGizmo, &QAction::toggled, this, [](bool on) { UiState::instance().setShowTransformGizmo(on); });
  views->addAction(showGizmo);
  QAction* shadeSel = cmd("views.shadeSelected", tr("S&hade Selected"));
  shadeSel->setCheckable(true);
  connect(shadeSel, &QAction::toggled, this, [](bool on) { UiState::instance().setShadeSelected(on); });
  views->addAction(shadeSel);
  QAction* viewCube = cmd("views.showViewCube", tr("Show &ViewCube"), QKeySequence("Alt+Ctrl+V"));
  viewCube->setCheckable(true);
  viewCube->setChecked(true);
  connect(viewCube, &QAction::toggled, this, [](bool on) { UiState::instance().setShowViewCube(on); });
  views->addAction(viewCube);
  QAction* safeFrame = cmd("views.safeFrame", tr("Show Safe &Frame"), QKeySequence("Shift+F"));
  connect(safeFrame, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    vp->setSafeFrame(!vp->safeFrame());
  });
  views->addAction(safeFrame);
  views->addSeparator();
  views->addAction(cameraFromViewAct_);
  views->addSeparator();
  views->addAction(shadingAct_);
  views->addAction(edgedAct_);
  views->addSeparator();
  QAction* redraw = cmd("views.redrawAll", tr("Redraw All &Views"), QKeySequence(Qt::Key_QuoteLeft));
  connect(redraw, &QAction::triggered, viewports_, &ViewportPanel::updateAll);
  views->addAction(redraw);
  views->addAction(zoomExtAct_);
  views->addAction(zoomExtAllAct_);
  views->addAction(maximizeAct_);
  views->addSeparator();
  expertAct_ = cmd("views.expertMode", tr("E&xpert Mode"), QKeySequence("Ctrl+X"));
  expertAct_->setCheckable(true);
  connect(expertAct_, &QAction::toggled, this, [this](bool on) {
    menuBar()->setVisible(!on);
    statusBar()->setVisible(!on);
    for (QToolBar* tb : findChildren<QToolBar*>()) tb->setVisible(!on);
    for (QDockWidget* d : findChildren<QDockWidget*>()) d->setVisible(!on);
    setPrompt(on ? tr("Expert mode: press Ctrl+X to restore the interface") : QString());
  });
  views->addAction(expertAct_);

  // ---- Create and Modifiers: every registered type, rebuilt when opened (3ds Max grouping) ----
  QMenu* create = menuAt("Create");
  connect(create, &QMenu::aboutToShow, this, [this, create] {
    create->clear();
    // Lists of each category by name; the menu takes them in 3ds Max order and appends the rest.
    QHash<QString, std::vector<CreateList>> lists;
    QSet<QString> taken;
    for (const QString& cat : createCategories()) lists.insert(cat, createLists(cat));
    auto listOf = [&](const QString& cat, const QString& name) -> const CreateList* {
      for (const CreateList& l : lists[cat])
        if (l.name == name) {
          taken.insert(cat + '/' + name);
          return &l;
        }
      return nullptr;
    };
    auto addButtons = [this](QMenu* m, const CreateList& l, const QString& suffix) {
      for (const CreateButton& b : l.buttons) {
        if (b.type.isEmpty()) continue;
        QString text = suffix.isEmpty() || b.label.endsWith(suffix) ? b.label : b.label + ' ' + suffix;
        QString type = b.type, label = b.label;
        m->addAction(text, this, [this, type, label] {
          commandPanel_->showCreate();
          commandPanel_->beginCreate(type, label);
        });
      }
    };
    auto addMenu = [&](const QString& title, const std::vector<std::pair<const CreateList*, QString>>& parts) {
      QMenu* m = create->addMenu(title);
      for (const auto& [l, suffix] : parts) {
        if (!l || !l->available()) continue;
        if (!m->isEmpty()) m->addSeparator();
        addButtons(m, *l, suffix);
      }
      if (m->isEmpty()) {
        QAction* a = placeholderAction(title);
        a->setParent(m);  // rebuilt with the menu
        m->addAction(a);
      }
      return m;
    };
    addMenu(tr("Standard Primitives"), {{listOf("Geometry", "Standard Primitives"), {}}});
    addMenu(tr("Extended Primitives"), {{listOf("Geometry", "Extended Primitives"), {}}});
    addMenu(tr("AEC Objects"), {{listOf("Geometry", "AEC Extended"), {}},
                                {listOf("Geometry", "Doors"), tr("Door")},
                                {listOf("Geometry", "Stairs"), {}},
                                {listOf("Geometry", "Windows"), tr("Window")}});
    addMenu(tr("Compound"), {{listOf("Geometry", "Compound Objects"), {}}});
    addMenu(tr("Particles"), {{listOf("Geometry", "Particle Systems"), {}}});
    addMenu(tr("Patch Grids"), {{listOf("Geometry", "Patch Grids"), {}}});
    addMenu(tr("NURBS"), {{listOf("Geometry", "NURBS Surfaces"), {}}, {listOf("Shapes", "NURBS Curves"), {}}});
    addMenu(tr("Point Cloud"), {{listOf("Geometry", "Point Cloud Objects"), {}}});
    addMenu(tr("Dynamics"), {{listOf("Geometry", "Dynamics Objects"), {}}});
    {
      // Veraa's renderer objects take the reference's V-Ray slot.
      std::vector<std::pair<const CreateList*, QString>> renderer;
      for (const QString& cat : createCategories()) renderer.push_back({listOf(cat, "Veraa"), {}});
      addMenu(tr("Veraa"), renderer);
    }
    create->addSeparator();
    addMenu(tr("Shapes"), {{listOf("Shapes", "Splines"), {}}});
    addMenu(tr("Extended Shapes"), {{listOf("Shapes", "Extended Splines"), {}}});
    create->addSeparator();
    QMenu* lights = create->addMenu(tr("Lights"));
    for (const char* l : {"Photometric", "Standard"}) {
      QMenu* m = lights->addMenu(tr("%1 Lights").arg(tr(l)));
      if (const CreateList* list = listOf("Lights", l)) addButtons(m, *list, {});
    }
    QMenu* cameras = addMenu(tr("Cameras"), {{listOf("Cameras", "Standard"), tr("Camera")}});
    cameras->addSeparator();
    cameras->addAction(cameraFromViewAct_);
    create->addSeparator();
    addMenu(tr("Helpers"), {{listOf("Helpers", "Standard"), {}}});
    addMenu(tr("SpaceWarps"), {{listOf("Space Warps", "Forces"), {}},
                               {listOf("Space Warps", "Deflectors"), {}},
                               {listOf("Space Warps", "Geometric/Deformable"), {}}});
    addMenu(tr("Systems"), {{listOf("Systems", "Standard"), {}}});
    create->addSeparator();
    addMenu(tr("Fluids"), {{listOf("Geometry", "Fluids"), {}}});
    // Lists from modules the layout does not know.
    bool first = true;
    for (const QString& cat : createCategories())
      for (const CreateList& l : lists[cat]) {
        if (taken.contains(cat + '/' + l.name) || !l.available()) continue;
        if (first) create->addSeparator();
        first = false;
        addMenu(cat + " - " + l.name, {{&l, {}}});
      }
  });

  QMenu* mods = menuAt("Modifiers");
  connect(mods, &QMenu::aboutToShow, this, [this, mods] {
    mods->clear();
    std::vector<std::string> cats(std::begin(kModifierCategories), std::end(kModifierCategories));
    std::vector<std::string> extra;
    for (const auto& t : modifierTypeInfos())
      if (std::find(cats.begin(), cats.end(), t.category) == cats.end() &&
          std::find(extra.begin(), extra.end(), t.category) == extra.end())
        extra.push_back(t.category);
    std::sort(extra.begin(), extra.end());
    cats.insert(cats.end(), extra.begin(), extra.end());
    for (const auto& cat : cats) {
      std::vector<const ModifierTypeInfo*> list;
      for (const auto& t : modifierTypeInfos())
        if (t.category == cat) list.push_back(&t);
      std::sort(list.begin(), list.end(),
                [](const ModifierTypeInfo* a, const ModifierTypeInfo* b) { return a->displayName < b->displayName; });
      // The renderer's modifiers take the reference's V-Ray slot.
      QString title = QString::fromStdString(cat.empty() ? std::string("Other") : cat == "Rendering" ? std::string("Veraa") : cat);
      QMenu* m = mods->addMenu(title);
      for (const ModifierTypeInfo* t : list) {
        QString type = QString::fromStdString(t->type);
        m->addAction(QString::fromStdString(t->displayName), this, [this, type] { commandPanel_->addModifier(type); });
      }
      // Groups of the reference layout without modifiers yet.
      if (list.empty()) {
        QAction* a = placeholderAction(title + tr(" Modifiers"));
        a->setText(tr("Not Available Yet"));
        a->setParent(m);  // rebuilt with the menu
        m->addAction(a);
      }
    }
  });

  // ---- Graph Editors (owned by the animation module) ----
  QMenu* graph = menuAt("Graph Editors");
  graph->addAction(proxyCommand("graph.curveEditor", tr("Track View - &Curve Editor..."), icons::curveEditor()));
  graph->addAction(proxyCommand("graph.dopeSheet", tr("Track View - &Dope Sheet..."), QIcon()));
  graph->addSeparator();
  graph->addAction(proxyCommand("graph.schematic", tr("&Schematic View..."), icons::schematic()));

  // ---- Rendering ----
  QMenu* rendering = menuAt("Rendering");
  rendering->addAction(renderAct_);
  rendering->addAction(renderSetupAct_);
  rendering->addAction(frameBufferAct_);
  rendering->addSeparator();
  rendering->addAction(proxyCommand("render.animation", tr("Render &Animation..."), QIcon()));
  rendering->addSeparator();
  rendering->addAction(materialEditorAct_);

  // ---- Customize (screenshot order) ----
  QMenu* customize = menuAt("Customize");
  QAction* cui = cmd("customize.ui", tr("&Customize User Interface..."));
  connect(cui, &QAction::triggered, this, [this] {
    CustomizeUiDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(cui);
  QAction* hotkeyEd = cmd("customize.hotkeys", tr("Hot&key Editor..."));
  connect(hotkeyEd, &QAction::triggered, this, [this] {
    HotkeyEditorDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(hotkeyEd);
  customize->addSeparator();
  QAction* loadUi = cmd("customize.loadScheme", tr("&Load Custom UI Scheme..."));
  connect(loadUi, &QAction::triggered, this, [this] { loadUiScheme(this); });
  QAction* saveUi = cmd("customize.saveScheme", tr("&Save Custom UI Scheme..."));
  connect(saveUi, &QAction::triggered, this, [this] { saveUiScheme(this); });
  customize->addAction(loadUi);
  customize->addAction(saveUi);
  customize->addSeparator();
  QAction* defaults = cmd("customize.defaultsSwitcher", tr("Custom &Defaults Switcher..."));
  connect(defaults, &QAction::triggered, this, [this] {
    CustomDefaultsDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(defaults);
  customize->addSeparator();
  QAction* revert = cmd("customize.revertUi", tr("&Revert to Startup UI Layout"));
  connect(revert, &QAction::triggered, this, &MainWindow::resetUiLayout);
  customize->addAction(revert);
  lockUiAct_ = cmd("customize.lockUi", tr("Loc&k UI Layout"), QKeySequence("Alt+0"));
  lockUiAct_->setCheckable(true);
  connect(lockUiAct_, &QAction::toggled, this, [this](bool on) { setUiLocked(on); });
  customize->addAction(lockUiAct_);
  customize->addSeparator();
  menuAt("Customize/Show UI");
  menuAt("Customize/Show Editors");
  menuAt("Customize/Show Explorers");
  customize->addSeparator();
  QAction* projPaths = cmd("customize.projectPaths", tr("Configure &Project Paths..."));
  connect(projPaths, &QAction::triggered, this, [this] {
    PathsDialog dlg(this, false);
    dlg.exec();
  });
  QAction* userPaths = cmd("customize.userPaths", tr("Configure &User and System Paths..."));
  connect(userPaths, &QAction::triggered, this, [this] {
    PathsDialog dlg(this, true);
    dlg.exec();
  });
  customize->addAction(projPaths);
  customize->addAction(userPaths);
  customize->addSeparator();
  QAction* units = cmd("customize.units", tr("U&nits Setup..."));
  connect(units, &QAction::triggered, this, [this] {
    UnitsSetupDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(units);
  customize->addSeparator();
  QAction* plugins = cmd("customize.pluginManager", tr("Plu&g-in Manager..."));
  connect(plugins, &QAction::triggered, this, [this] {
    PluginManagerDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(plugins);
  customize->addSeparator();
  QAction* prefs = cmd("customize.preferences", tr("&Preferences..."));
  connect(prefs, &QAction::triggered, this, [this] {
    PreferencesDialog dlg(this);
    dlg.exec();
  });
  customize->addAction(prefs);

  // ---- Scripting ----
  installScriptingMenu(*this, menuAt("Scripting"));

  // ---- Customize > Show Editors / Show Explorers (filled now that every command exists) ----
  QMenu* editors = menuAt("Customize/Show Editors");
  for (const char* id : {"render.materialEditor", "render.setup", "render.frameWindow", "graph.curveEditor",
                         "graph.dopeSheet", "graph.schematic", "scripting.listener", "scripting.editor"}) {
    QAction* a = command(id);
    for (QAction* p : proxies_)
      if (p->property("proxyId").toString() == id) a = p;
    if (a) editors->addAction(a);
  }
  QMenu* explorers = menuAt("Customize/Show Explorers");
  explorers->addAction(command("tools.sceneExplorer"));
  explorers->addAction(command("tools.layerExplorer"));

  // ---- Help ----
  QMenu* help = menuAt("Help");
  QAction* keys = cmd("help.shortcuts", tr("&Keyboard Shortcuts"), QKeySequence::HelpContents);
  connect(keys, &QAction::triggered, this, [this] { showShortcutsDialog(this); });
  help->addAction(keys);
  QAction* guide = cmd("help.userGuide", tr("&User Guide"));
  connect(guide, &QAction::triggered, this, [this] {
    QString readme = QDir(QApplication::applicationDirPath()).filePath("README.md");
    if (!QFileInfo::exists(readme)) readme = QDir(QApplication::applicationDirPath()).filePath("../../README.md");
    if (QFileInfo::exists(readme)) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(readme).absoluteFilePath()));
    else showUserGuide(this);
  });
  help->addAction(guide);
  QAction* whatsNew = cmd("help.whatsNew", tr("&What's New"));
  connect(whatsNew, &QAction::triggered, this, [this] { showWhatsNew(this); });
  help->addAction(whatsNew);
  help->addSeparator();
  QAction* about = cmd("help.about", tr("&About Veraa"));
  connect(about, &QAction::triggered, this, [this] {
    QMessageBox::about(
        this, tr("About Veraa"),
        tr("<h3>Veraa %1</h3>"
           "<p>3D modeling and physically based rendering.</p>"
           "<p>Veraa is an independent open-source project inspired by 3ds Max and V-Ray workflows. "
           "It is not affiliated with, endorsed by or connected to Autodesk or Chaos.</p>"
           "<p>Rendering uses Intel Embree%2.</p>")
            .arg(VERAA_VERSION)
            .arg(Renderer::denoiserAvailable() ? tr(" and Intel Open Image Denoise") : QString()));
  });
  help->addAction(about);
}

// ---------------------------------------------------------------------------------------------
// Docks

void MainWindow::createDocks() {
  commandPanel_ = new CommandPanel(doc_, state_, this);
  connect(commandPanel_, &CommandPanel::featureNotAvailable, this, &MainWindow::showNotAvailable);
  commandPanel_->setTabToolTip(0, "Create");
  commandPanel_->setTabToolTip(1, "Modify");
  commandPanel_->setTabIcon(0, icons::create());
  commandPanel_->setTabIcon(1, icons::modify());
  commandPanel_->setTabText(0, QString());
  commandPanel_->setTabText(1, QString());
  commandPanel_->setUsesScrollButtons(false);
  commandPanel_->tabBar()->setExpanding(true);
  commandPanel_->tabBar()->setStyleSheet("QTabBar::tab { min-width: 0px; padding: 5px 4px; }");
  commandDock_ = new QDockWidget(tr("Command Panel"), this);
  commandDock_->setObjectName("CommandPanelDock");
  commandDock_->setWidget(commandPanel_);
  commandDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
  commandDock_->setTitleBarWidget(new QWidget(commandDock_));
  commandDock_->setMinimumWidth(280);
  addDockWidget(Qt::RightDockWidgetArea, commandDock_);
  addCommandPanelTab(new HierarchyPanel(this), icons::hierarchy(), "Hierarchy");
  addCommandPanelTab(new DisplayPanel(this), icons::display(), "Display");
  addCommandPanelTab(new UtilitiesPanel(this), icons::utilities(), "Utilities");

  explorer_ = new SceneExplorer(doc_, this);
  explorerDock_ = new QDockWidget(tr("Scene Explorer"), this);
  explorerDock_->setObjectName("SceneExplorerDock");
  explorerDock_->setWidget(explorer_);
  explorerDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable |
                             QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::LeftDockWidgetArea, explorerDock_);

  layerExplorer_ = new LayerExplorer(doc_, this);
  layerDock_ = new QDockWidget(tr("Layer Explorer"), this);
  layerDock_->setObjectName("LayerExplorerDock");
  layerDock_->setWidget(layerExplorer_);
  layerDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable |
                          QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::LeftDockWidgetArea, layerDock_);
  tabifyDockWidget(explorerDock_, layerDock_);
  explorerDock_->raise();
  resizeDocks({explorerDock_, commandDock_}, {240, 300}, Qt::Horizontal);

  QMenu* showUi = menuAt("Customize/Show UI");
  QAction* cp = commandDock_->toggleViewAction();
  cp->setText(tr("Show Command Panel"));
  registerCommand("ui.commandPanel", cp);
  showUi->addAction(cp);
  QAction* se = explorerDock_->toggleViewAction();
  se->setText(tr("Show Scene Explorer"));
  registerCommand("ui.sceneExplorer", se);
  showUi->addAction(se);
  QAction* le = layerDock_->toggleViewAction();
  le->setText(tr("Show Layer Explorer"));
  registerCommand("ui.layerExplorer", le);
  showUi->addAction(le);
}

// ---------------------------------------------------------------------------------------------
// Title, type-in, recent files

void MainWindow::updateTitle() {
  QString name = doc_->filePath().isEmpty() ? tr("Untitled") : QFileInfo(doc_->filePath()).fileName();
  setWindowTitle(QString("%1%2 - Veraa").arg(name, doc_->isModified() ? " *" : ""));
}

void MainWindow::updateRecentMenu() {
  recentMenu_->clear();
  QStringList files = appSettings().value("recentFiles").toStringList();
  for (const QString& f : files) recentMenu_->addAction(QFileInfo(f).fileName(), this, [this, f] {
                                     if (maybeSave()) openFile(f);
                                   })->setToolTip(f);
  recentMenu_->setEnabled(!files.isEmpty());
}

void MainWindow::addRecent(const QString& path) {
  QSettings s = appSettings();
  QStringList files = s.value("recentFiles").toStringList();
  QString abs = QFileInfo(path).absoluteFilePath();
  files.removeAll(abs);
  files.prepend(abs);
  while (files.size() > 10) files.removeLast();
  s.setValue("recentFiles", files);
  updateRecentMenu();
}

// ---------------------------------------------------------------------------------------------
// Edit operations

void MainWindow::cloneSelection(bool askOptions) {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  QString name;
  if (askOptions) {
    QString def = nodes.size() == 1 ? QString::fromStdString(doc_->scene().uniqueNodeName(
                                          [&] {
                                            std::string b = nodes[0]->name;
                                            while (!b.empty() && b.back() >= '0' && b.back() <= '9') b.pop_back();
                                            return b.empty() ? std::string("Object") : b;
                                          }()))
                                    : QString();
    CloneOptionsDialog dlg(def, false, this);
    dlg.setMode(cloneMode_);
    if (nodes.size() > 1) dlg.setWindowTitle(tr("Clone Options (%1 objects)").arg(nodes.size()));
    if (dlg.exec() != QDialog::Accepted) return;
    name = dlg.name();
    cloneMode_ = dlg.mode();
  }
  CloneOptionsDialog::Mode mode = CloneOptionsDialog::Copy;
  if (askOptions) mode = static_cast<CloneOptionsDialog::Mode>(cloneMode_);
  doc_->beginEdit(tr("Clone"));
  QVector<uint32_t> sources = doc_->selection();
  QVector<uint32_t> ids = doc_->cloneSelected(true);
  for (int i = 0; i < ids.size() && i < sources.size(); ++i) applyCloneMode(doc_->scene(), sources[i], ids[i], mode);
  if (ids.size() == 1 && !name.isEmpty())
    if (Node* n = doc_->scene().findNode(ids[0])) {
      n->name = name.toStdString();
      doc_->notifyNodeChanged(n, ChangeName);
    }
  doc_->endEdit();
  state_->setTool(TransformTool::Move);
  setPrompt(tr("Clones created and selected; drag them to a new place"));
}

void MainWindow::hold() {
  doc_->commitSoftEdit();
  held_ = doc_->scene().toJson();
  hasHold_ = true;
  // Also keep a copy on disk, so a crash does not lose it.
  QString dir = projectSubfolder("autoback");
  QDir().mkpath(dir);
  QFile f(QDir(dir).filePath("veraa_hold.veraa"));
  if (f.open(QIODevice::WriteOnly)) f.write(QByteArray::fromStdString(held_.dump()));
  command("edit.fetch")->setEnabled(true);
  setPrompt(tr("Scene held (Edit > Fetch restores it)"));
}

bool MainWindow::fetch() {
  if (!hasHold_) {
    QFile f(QDir(projectSubfolder("autoback")).filePath("veraa_hold.veraa"));
    if (f.open(QIODevice::ReadOnly)) {
      try {
        held_ = json::parse(f.readAll().toStdString());
        hasHold_ = true;
      } catch (...) {
      }
    }
  }
  if (!hasHold_) {
    setPrompt(tr("Nothing has been held yet (Edit > Hold)"));
    return false;
  }
  doc_->beginEdit(tr("Fetch"));
  std::string keepPath = doc_->scene().filePath;
  std::string err;
  bool ok = doc_->scene().fromJson(held_, &err);
  doc_->scene().filePath = keepPath;
  doc_->clearSelection();
  doc_->notifyStructureChanged();
  doc_->notifyMaterialsChanged();
  doc_->notifyRenderSettingsChanged();
  doc_->endEdit();
  viewports_->updateAll();
  setPrompt(ok ? tr("Fetched the held scene") : tr("Fetch failed: %1").arg(QString::fromStdString(err)));
  return ok;
}

void MainWindow::isolateSelection(bool on) {
  if (on == isolated_) {
    if (isolateAct_) isolateAct_->setChecked(on);
    return;
  }
  if (on && doc_->selection().isEmpty()) {
    setPrompt(tr("Select the objects to isolate first"));
    if (isolateAct_) isolateAct_->setChecked(false);
    return;
  }
  doc_->beginEdit(on ? tr("Isolate Selection") : tr("End Isolate"));
  QVector<uint32_t> changed;
  if (on) {
    isolatedHidden_.clear();
    for (const auto& n : doc_->scene().nodes)
      if (!doc_->isSelected(n->id) && !n->hidden) {
        n->hidden = true;
        isolatedHidden_.push_back(n->id);
        changed.push_back(n->id);
      }
  } else {
    for (uint32_t id : isolatedHidden_)
      if (Node* n = doc_->scene().findNode(id)) {
        n->hidden = false;
        changed.push_back(id);
      }
    isolatedHidden_.clear();
  }
  doc_->notifyNodesChanged(changed, ChangeDisplay);
  doc_->endEdit();
  isolated_ = on;
  if (isolateAct_) isolateAct_->setChecked(on);
  if (on) viewports_->active()->zoomExtents(true);
  setPrompt(on ? tr("Isolated %1 object(s); Alt+Q or Tools > End Isolate to exit").arg(doc_->selection().size())
               : tr("Isolation ended"));
}

void MainWindow::selectSimilar() {
  QSet<QString> types;
  for (Node* n : doc_->selectedNodes())
    if (n->object) types.insert(QString::fromStdString(n->object->type()));
  if (types.isEmpty()) return;
  QVector<uint32_t> ids;
  const Scene& s = doc_->scene();
  for (const auto& n : s.nodes)
    if (n->object && types.contains(QString::fromStdString(n->object->type())) && s.isNodeVisible(*n) &&
        !s.isNodeFrozen(*n))
      ids.push_back(n->id);
  doc_->setSelection(ids);
}

void MainWindow::selectInstances() {
  QVector<uint32_t> ids;
  for (Node* n : doc_->selectedNodes())
    for (Node* inst : doc_->scene().instancesOf(*n))
      if (!ids.contains(inst->id)) ids.push_back(inst->id);
  doc_->setSelection(ids);
}

void MainWindow::openTransformTypeIn() {
  auto* d = findChild<TransformTypeInDialog*>();
  if (!d) d = new TransformTypeInDialog(this);
  d->show();
  d->raise();
}

// ---------------------------------------------------------------------------------------------
// Files

bool MainWindow::openFile(const QString& path) {
  QString ext = QFileInfo(path).suffix().toLower();
  if (ext == "py") {
    QString out;
    bool ok = ScriptEngine::instance().runFile(path, &out);
    setPrompt(ok ? tr("Ran %1").arg(QFileInfo(path).fileName()) : tr("Script %1 failed").arg(QFileInfo(path).fileName()));
    if (!ok) QMessageBox::warning(this, tr("Run Script"), out.right(4000));
    return ok;
  }
  if (ext != "veraa" && !ext.isEmpty()) {
    // A 3ds Max scene replaces the current scene (like opening it in 3ds Max); other formats
    // are merged in.
    if (ext == "max") doc_->newScene();
    QString err;
    QVector<uint32_t> ids;
    if (!importPath(path, &ids, &err)) {
      QMessageBox::warning(this, tr("Import"), tr("Could not import %1:\n%2").arg(path, err));
      return false;
    }
    viewports_->zoomExtentsAll(false);
    if (ext == "max" && !lastImportWarnings_.isEmpty()) {
      // Import summary and warnings; shown without blocking (scripts may call openFile).
      auto* box = new QMessageBox(QMessageBox::Information, tr("Open 3ds Max Scene"),
                                  lastImportWarnings_.mid(0, 25).join('\n'), QMessageBox::Ok, this);
      box->setAttribute(Qt::WA_DeleteOnClose);
      box->open();
    }
    return true;
  }
  QString err;
  if (!doc_->load(path, &err)) {
    QMessageBox::warning(this, tr("Open Scene"), tr("Could not open %1:\n%2").arg(path, err));
    return false;
  }
  isolated_ = false;
  isolatedHidden_.clear();
  addRecent(path);
  reloadXRefs();
  viewports_->zoomExtentsAll(false);
  setPrompt(tr("Opened %1").arg(QFileInfo(path).fileName()));
  return true;
}

bool MainWindow::maybeSave() {
  doc_->commitSoftEdit();
  if (!doc_->isModified()) return true;
  auto r = QMessageBox::question(this, tr("Veraa"), tr("The scene has been modified.\nDo you want to save your changes?"),
                                 QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
  if (r == QMessageBox::Save) return save();
  return r == QMessageBox::Discard;
}

bool MainWindow::save() {
  if (doc_->filePath().isEmpty()) return saveAs();
  QString err;
  if (!doc_->save(doc_->filePath(), &err)) {
    QMessageBox::warning(this, tr("Save Scene"), err);
    return false;
  }
  addRecent(doc_->filePath());
  setPrompt(tr("Saved %1").arg(QFileInfo(doc_->filePath()).fileName()));
  return true;
}

bool MainWindow::saveAs() {
  QString start = doc_->filePath().isEmpty() ? QDir(projectSubfolder("scenes")).filePath(tr("untitled.veraa"))
                                             : doc_->filePath();
  QString f = QFileDialog::getSaveFileName(this, tr("Save Scene As"), start, tr("Veraa scenes (*.veraa)"));
  if (f.isEmpty()) return false;
  if (QFileInfo(f).suffix().isEmpty()) f += ".veraa";
  QString err;
  if (!doc_->save(f, &err)) {
    QMessageBox::warning(this, tr("Save Scene"), err);
    return false;
  }
  addRecent(f);
  return true;
}

void MainWindow::importObjFile() {
  QSettings s = appSettings();
  QString f = QFileDialog::getOpenFileName(this, tr("Import OBJ"), s.value("lastImportDir", projectSubfolder("import")).toString(),
                                           tr("Wavefront OBJ (*.obj);;All files (*)"));
  if (f.isEmpty()) return;
  s.setValue("lastImportDir", QFileInfo(f).absolutePath());
  importObjFileFrom(f);
}

void MainWindow::importObjFileFrom(const QString& f) {
  ObjImportDialog opts(this);
  if (opts.exec() != QDialog::Accepted) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  doc_->beginEdit(tr("Import OBJ"));
  ObjImportResult res;
  std::string err;
  bool ok = importInBackground(
      this, tr("Importing %1...").arg(QFileInfo(f).fileName()), doc_->scene(),
      [&](Scene& into, ImportProgress& progress, std::string& e) {
        ObjImportOptions o = opts.options();
        o.progress = &progress;
        return importObj(into, f.toStdString(), o, &res, &e);
      },
      &res.nodeIds, &err);
  QApplication::restoreOverrideCursor();
  if (!ok) {
    doc_->cancelEdit();
    QMessageBox::warning(this, tr("Import OBJ"), QString::fromStdString(err));
    return;
  }
  doc_->notifyStructureChanged();
  doc_->notifyMaterialsChanged();
  doc_->setSelection(QVector<uint32_t>(res.nodeIds.begin(), res.nodeIds.end()));
  doc_->endEdit();
  viewports_->zoomExtentsAll(false);
  setPrompt(tr("Imported %1 object(s), %2 triangles").arg(res.nodeIds.size()).arg(res.triangles));
  if (!res.warnings.empty()) {
    QStringList w;
    for (const auto& x : res.warnings) w << QString::fromStdString(x);
    QMessageBox::information(this, tr("Import OBJ"), w.mid(0, 20).join("\n"));
  }
}

// ---------------------------------------------------------------------------------------------
// Quad-style context menu

void MainWindow::showContextMenu(Viewport* vp, const QPoint& pos) {
  QMenu menu(this);
  bool hasSel = !doc_->selection().isEmpty();
  menu.addSection(tr("Display"));
  menu.addAction(command("tools.isolate"));
  menu.addAction(tr("Unfreeze All"), this, [this] {
    doc_->beginEdit(tr("Unfreeze All"));
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (n->frozen) {
        n->frozen = false;
        ids.push_back(n->id);
      }
    doc_->notifyNodesChanged(ids, ChangeDisplay);
    doc_->endEdit();
  });
  menu.addAction(tr("Freeze Selection"), this, [this] {
    doc_->beginEdit(tr("Freeze Selection"));
    QVector<uint32_t> ids;
    for (Node* n : doc_->selectedNodes()) {
      n->frozen = true;
      ids.push_back(n->id);
    }
    doc_->clearSelection();
    doc_->notifyNodesChanged(ids, ChangeDisplay);
    doc_->endEdit();
  })->setEnabled(hasSel);
  menu.addAction(unhideAct_);
  menu.addAction(tr("Hide Unselected"), this, [this] {
    doc_->beginEdit(tr("Hide Unselected"));
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (!doc_->isSelected(n->id) && !n->hidden) {
        n->hidden = true;
        ids.push_back(n->id);
      }
    doc_->notifyNodesChanged(ids, ChangeDisplay);
    doc_->endEdit();
  })->setEnabled(hasSel);
  menu.addAction(hideAct_);
  menu.addSection(tr("Transform"));
  menu.addAction(moveToolAct_);
  menu.addAction(rotateToolAct_);
  menu.addAction(scaleToolAct_);
  menu.addAction(placeToolAct_);
  menu.addAction(selectToolAct_);
  menu.addAction(command("edit.selectSimilar"));
  menu.addAction(cloneAct_);
  menu.addAction(propertiesAct_);
  menu.addAction(tr("Zoom Extents Selected"), this, [vp] { vp->zoomExtents(true); });
  menu.addSeparator();
  QMenu* convert = menu.addMenu(tr("Convert To:"));
  convert->addAction(command("tools.convertToPoly"));
  if (QAction* spline = command("convertToEditableSpline")) convert->addAction(spline);
  // Feature modules mark actions for this menu with the "quadMenu" property.
  for (QAction* t : findChildren<QAction*>())
    if (t->property("quadMenu").toBool() && t->isVisible()) menu.addAction(t);
  menu.addAction(materialEditorAct_);
  menu.addAction(deleteAct_);
  menu.exec(pos);
}

// ---------------------------------------------------------------------------------------------
// Rendering windows

MaterialEditor* MainWindow::materialEditor() {
  if (!materialEditor_) materialEditor_ = new MaterialEditor(doc_, this);
  return materialEditor_;
}

RenderSetupDialog* MainWindow::renderSetup() {
  if (!renderSetup_) {
    renderSetup_ = new RenderSetupDialog(doc_, this);
    connect(renderSetup_, &RenderSetupDialog::renderRequested, this, &MainWindow::render);
  }
  return renderSetup_;
}

FrameBufferWindow* MainWindow::frameBuffer() {
  if (!frameBuffer_) {
    frameBuffer_ = new FrameBufferWindow(this);
    connect(frameBuffer_, &FrameBufferWindow::renderAgainRequested, this, &MainWindow::render);
  }
  return frameBuffer_;
}

RenderView MainWindow::renderView(QString* name) const {
  const Scene& s = doc_->scene();
  float aspect = static_cast<float>(s.renderSettings.getInt("width")) / std::max(1, s.renderSettings.getInt("height"));
  std::string camName = s.renderSettings.getString("camera");
  if (!camName.empty())
    if (Node* n = s.findNodeByName(camName); n && n->camera()) {
      if (name) *name = QString::fromStdString(camName);
      return viewFromCamera(*n, aspect);
    }
  Viewport* vp = viewports_->active();
  vp->updateCamera();
  const ViewCamera& c = vp->camera();
  if (c.type == ViewType::Camera)
    if (Node* n = s.findNode(c.cameraNodeId); n && n->camera()) {
      if (name) *name = QString::fromStdString(n->name);
      return viewFromCamera(*n, aspect);
    }
  RenderView v;
  v.position = c.eye();
  v.up = c.up();
  if (c.orthographic()) {
    v.orthographic = true;
    v.orthoHeight = c.orthoHalfHeight() * 2.0f;
    v.target = c.eye() + c.forward() * 100.0f;
  } else {
    v.fovYDeg = c.verticalFov();
    v.target = c.eye() + c.forward() * std::max(c.distance, 1.0f);
  }
  if (name) *name = viewTypeName(c.type);
  return v;
}

void MainWindow::render() {
  doc_->commitSoftEdit();
  QString name;
  RenderView view = renderView(&name);
  FrameBufferWindow* fb = frameBuffer();
  fb->show();
  fb->raise();
  fb->startRender(doc_->scene(), view, name);
}

// ---------------------------------------------------------------------------------------------
// Shortcut override: while a viewport tool is active, single-key global shortcuts step aside so
// the tool receives those keys (Keyboard Shortcut Override toggle on the main toolbar).

void MainWindow::updateShortcutOverride() {
  bool want = UiState::instance().shortcutOverride() && state_->viewportTool();
  if (want && overriddenKeys_.isEmpty()) {
    for (auto it = commands_.begin(); it != commands_.end(); ++it) {
      QKeySequence k = it.value()->shortcut();
      if (k.count() == 1 && (k[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier)) == 0 &&
          k[0].key() >= Qt::Key_A && k[0].key() <= Qt::Key_Z) {
        overriddenKeys_.insert(it.key(), k);
        it.value()->setShortcut(QKeySequence());
      }
    }
  } else if (!want && !overriddenKeys_.isEmpty()) {
    for (auto it = overriddenKeys_.begin(); it != overriddenKeys_.end(); ++it)
      if (QAction* a = commands_.value(it.key())) a->setShortcut(it.value());
    overriddenKeys_.clear();
  }
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
  return QMainWindow::eventFilter(obj, e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (!maybeSave()) {
    e->ignore();
    return;
  }
  if (frameBuffer_) frameBuffer_->stopRender();
  QSettings s = appSettings();
  s.setValue("geometry", saveGeometry());
  s.setValue("windowState", saveState());
  e->accept();
  QApplication::closeAllWindows();
}

}  // namespace veraa
