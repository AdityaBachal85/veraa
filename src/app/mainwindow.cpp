#include "mainwindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include "commandpanel.h"
#include "dialogs.h"
#include "extension.h"
#include "framebuffer.h"
#include "icons.h"
#include "materialeditor.h"
#include "paramform.h"
#include "rendersetup.h"
#include "sceneexplorer.h"
#include "viewportpanel.h"

namespace veraa {

namespace {
const char* kDefaultPrompt = "Click or click-and-drag to select objects";

QSettings appSettings() { return QSettings("Veraa", "Veraa"); }
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
  createDocks();
  createStatusBar();

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
  connect(state_, &EditorState::showGridChanged, gridAct_, &QAction::setChecked);
  connect(viewports_, &ViewportPanel::activeChanged, this, [this](Viewport* vp) {
    QSignalBlocker b1(shadingAct_), b2(edgedAct_);
    shadingAct_->setChecked(vp->shading() == ShadingMode::Shaded);
    edgedAct_->setChecked(vp->edgedFaces());
  });
  connect(viewports_, &ViewportPanel::contextMenuRequested, this, &MainWindow::showContextMenu);

  // Feature modules add their menus, toolbars and panels (see extension.h).
  runAppExtensions(*this);

  resize(1600, 950);
  QSettings s = appSettings();
  restoreGeometry(s.value("geometry").toByteArray());
  restoreState(s.value("windowState").toByteArray());
  updateTitle();
  updateTypeIn();
  updateRecentMenu();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------------------------

void MainWindow::createActions() {
  auto act = [this](const QString& text, const QKeySequence& key = {}, const QIcon& icon = {}) {
    auto* a = new QAction(icon, text, this);
    if (!key.isEmpty()) a->setShortcut(key);
    addAction(a);  // shortcuts work even when menus are hidden
    return a;
  };

  newAct_ = act(tr("&New Scene"), QKeySequence::New, icons::newFile());
  connect(newAct_, &QAction::triggered, this, [this] {
    if (!maybeSave()) return;
    doc_->newScene();
    viewports_->zoomExtentsAll(false);
  });
  openAct_ = act(tr("&Open..."), QKeySequence::Open, icons::open());
  connect(openAct_, &QAction::triggered, this, [this] {
    if (!maybeSave()) return;
    QString dir = QFileInfo(doc_->filePath()).absolutePath();
    QString f = QFileDialog::getOpenFileName(this, tr("Open Scene"), dir, tr("Veraa scenes (*.veraa);;All files (*)"));
    if (!f.isEmpty()) openFile(f);
  });
  saveAct_ = act(tr("&Save"), QKeySequence::Save, icons::save());
  connect(saveAct_, &QAction::triggered, this, [this] { save(); });
  saveAsAct_ = act(tr("Save &As..."), QKeySequence("Ctrl+Shift+S"));
  connect(saveAsAct_, &QAction::triggered, this, [this] { saveAs(); });
  importAct_ = act(tr("&Import OBJ..."), QKeySequence(), icons::import());
  connect(importAct_, &QAction::triggered, this, &MainWindow::importObjFile);
  exitAct_ = act(tr("E&xit"), QKeySequence::Quit);
  connect(exitAct_, &QAction::triggered, this, &QWidget::close);

  undoAct_ = act(tr("&Undo"), QKeySequence::Undo, icons::undo());
  connect(undoAct_, &QAction::triggered, this, [this] {
    viewports_->active()->cancelInteraction();
    doc_->commitSoftEdit();
    doc_->undoStack()->undo();
  });
  redoAct_ = act(tr("&Redo"), QKeySequence("Ctrl+Y"), icons::redo());
  connect(redoAct_, &QAction::triggered, this, [this] {
    doc_->commitSoftEdit();
    doc_->undoStack()->redo();
  });
  connect(doc_->undoStack(), &QUndoStack::undoTextChanged, this,
          [this](const QString& t) { undoAct_->setToolTip(t.isEmpty() ? tr("Undo") : tr("Undo %1").arg(t)); });
  connect(doc_->undoStack(), &QUndoStack::redoTextChanged, this,
          [this](const QString& t) { redoAct_->setToolTip(t.isEmpty() ? tr("Redo") : tr("Redo %1").arg(t)); });

  deleteAct_ = act(tr("&Delete"), QKeySequence::Delete, icons::trash());
  connect(deleteAct_, &QAction::triggered, this, [this] { doc_->deleteSelected(); });
  cloneAct_ = act(tr("&Clone"), QKeySequence("Ctrl+V"));
  connect(cloneAct_, &QAction::triggered, this, [this] { cloneSelection(true); });
  selectAllAct_ = act(tr("Select &All"), QKeySequence("Ctrl+A"));
  connect(selectAllAct_, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (!n->hidden) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  selectNoneAct_ = act(tr("Select &None"), QKeySequence("Ctrl+D"));
  connect(selectNoneAct_, &QAction::triggered, this, [this] { doc_->clearSelection(); });
  selectInvertAct_ = act(tr("Select &Invert"), QKeySequence("Ctrl+I"));
  connect(selectInvertAct_, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (!n->hidden && !doc_->isSelected(n->id)) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  selectByNameAct_ = act(tr("Select by &Name..."), QKeySequence("H"));
  connect(selectByNameAct_, &QAction::triggered, this, [this] {
    SelectByNameDialog dlg(doc_, this);
    if (dlg.exec() == QDialog::Accepted) doc_->setSelection(dlg.chosen());
  });
  hideAct_ = act(tr("&Hide Selection"));
  connect(hideAct_, &QAction::triggered, this, [this] { doc_->hideSelected(); });
  unhideAct_ = act(tr("&Unhide All"));
  connect(unhideAct_, &QAction::triggered, this, [this] { doc_->unhideAll(); });
  propertiesAct_ = act(tr("Object &Properties..."));
  connect(propertiesAct_, &QAction::triggered, this, [this] {
    if (Node* n = doc_->firstSelected()) {
      ObjectPropertiesDialog dlg(doc_, n, this);
      dlg.exec();
    }
  });

  toolGroup_ = new QActionGroup(this);
  auto toolAct = [&](const QString& text, const QString& key, const QIcon& icon, TransformTool t) {
    QAction* a = act(text, QKeySequence(key), icon);
    a->setCheckable(true);
    a->setToolTip(QString("%1 (%2)").arg(text, key));
    toolGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, t] { state_->setTool(t); });
    return a;
  };
  selectToolAct_ = toolAct(tr("Select Object"), "Q", icons::select(), TransformTool::Select);
  moveToolAct_ = toolAct(tr("Select and Move"), "W", icons::move(), TransformTool::Move);
  rotateToolAct_ = toolAct(tr("Select and Rotate"), "E", icons::rotate(), TransformTool::Rotate);
  scaleToolAct_ = toolAct(tr("Select and Scale"), "R", icons::scale(), TransformTool::Scale);
  selectToolAct_->setChecked(true);

  axisGroup_ = new QActionGroup(this);
  auto axisAct = [&](const QString& label, const QString& key, GizmoHandle h) {
    QAction* a = act(label, QKeySequence(key), icons::axis(label));
    a->setCheckable(true);
    a->setToolTip(tr("Restrict to %1 (%2)").arg(label, key));
    axisGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, h] { state_->setConstraint(h); });
    return a;
  };
  axisXAct_ = axisAct("X", "F5", GizmoHandle::X);
  axisYAct_ = axisAct("Y", "F6", GizmoHandle::Y);
  axisZAct_ = axisAct("Z", "F7", GizmoHandle::Z);
  axisPlaneAct_ = axisAct("XY", "F8", GizmoHandle::XY);
  disconnect(axisPlaneAct_, &QAction::triggered, nullptr, nullptr);
  connect(axisPlaneAct_, &QAction::triggered, this, [this] {
    // F8 cycles XY -> YZ -> ZX like 3ds Max.
    GizmoHandle c = state_->constraint();
    GizmoHandle next = c == GizmoHandle::XY ? GizmoHandle::YZ : c == GizmoHandle::YZ ? GizmoHandle::ZX : GizmoHandle::XY;
    if (handleAxisMask(c) == 1 || handleAxisMask(c) == 2 || handleAxisMask(c) == 4) next = GizmoHandle::XY;
    state_->setConstraint(next);
  });
  axisPlaneAct_->setChecked(true);

  materialEditorAct_ = act(tr("&Material Editor..."), QKeySequence("M"), icons::materialEditor());
  connect(materialEditorAct_, &QAction::triggered, this, [this] {
    MaterialEditor* me = materialEditor();
    me->show();
    me->raise();
    me->activateWindow();
    if (Node* n = doc_->firstSelected(); n && n->materialId) me->selectMaterial(n->materialId);
  });
  renderSetupAct_ = act(tr("Render &Setup..."), QKeySequence("F10"), icons::renderSetup());
  connect(renderSetupAct_, &QAction::triggered, this, [this] {
    RenderSetupDialog* rs = renderSetup();
    rs->show();
    rs->raise();
    rs->activateWindow();
  });
  renderAct_ = act(tr("&Render Production"), QKeySequence("Shift+Q"), icons::render());
  renderAct_->setShortcuts({QKeySequence("Shift+Q"), QKeySequence("F9")});
  renderAct_->setToolTip(tr("Render Production (Shift+Q)"));
  connect(renderAct_, &QAction::triggered, this, &MainWindow::render);
  frameBufferAct_ = act(tr("Rendered &Frame Window"));
  connect(frameBufferAct_, &QAction::triggered, this, [this] {
    FrameBufferWindow* fb = frameBuffer();
    fb->show();
    fb->raise();
  });

  zoomExtAct_ = act(tr("Zoom &Extents Selected"), QKeySequence("Z"), icons::zoomExtents());
  connect(zoomExtAct_, &QAction::triggered, this, [this] { viewports_->active()->zoomExtents(true); });
  zoomExtAllAct_ = act(tr("Zoom Extents &All"), QKeySequence("Ctrl+Shift+Z"));
  connect(zoomExtAllAct_, &QAction::triggered, this, [this] { viewports_->zoomExtentsAll(false); });
  undoViewAct_ = act(tr("&Undo View Change"), QKeySequence("Shift+Z"));
  connect(undoViewAct_, &QAction::triggered, this, [this] { viewports_->active()->undoViewChange(); });
  maximizeAct_ = act(tr("&Maximize Viewport Toggle"), QKeySequence("Alt+W"), icons::maximize());
  connect(maximizeAct_, &QAction::triggered, viewports_, &ViewportPanel::toggleMaximize);
  shadingAct_ = act(tr("&Default Shading"), QKeySequence("F3"));
  shadingAct_->setCheckable(true);
  connect(shadingAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    vp->setShading(vp->shading() == ShadingMode::Shaded ? ShadingMode::Wireframe : ShadingMode::Shaded);
    shadingAct_->setChecked(vp->shading() == ShadingMode::Shaded);
  });
  edgedAct_ = act(tr("&Edged Faces"), QKeySequence("F4"));
  edgedAct_->setCheckable(true);
  connect(edgedAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    vp->setEdgedFaces(!vp->edgedFaces());
    edgedAct_->setChecked(vp->edgedFaces());
  });
  gridAct_ = act(tr("Show &Grid"), QKeySequence("G"));
  gridAct_->setCheckable(true);
  gridAct_->setChecked(true);
  connect(gridAct_, &QAction::triggered, this, [this](bool on) { state_->setShowGrid(on); });
  cameraFromViewAct_ = act(tr("Create &Camera From View"), QKeySequence("Ctrl+C"));
  connect(cameraFromViewAct_, &QAction::triggered, this, [this] {
    Viewport* vp = viewports_->active();
    if (vp->camera().type != ViewType::Perspective) {
      prompt_->setText(tr("Create Camera From View needs an active Perspective viewport"));
      return;
    }
    vp->createCameraFromView();
  });

  // Viewport view switching (T, B, F, L, P, U, C).
  const struct {
    const char* key;
    ViewType type;
  } views[] = {{"T", ViewType::Top},   {"B", ViewType::Bottom},      {"F", ViewType::Front},
               {"L", ViewType::Left},  {"P", ViewType::Perspective}, {"U", ViewType::User}};
  for (const auto& v : views) {
    QAction* a = act(viewTypeName(v.type) + tr(" View"), QKeySequence(v.key));
    ViewType t = v.type;
    connect(a, &QAction::triggered, this, [this, t] { viewports_->active()->setViewType(t); });
    a->setObjectName(QString("view_%1").arg(v.key));
  }
  QAction* camView = act(tr("Camera View"), QKeySequence("C"));
  camView->setObjectName("view_C");
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
      prompt_->setText(tr("There are no cameras in the scene (create one with Ctrl+C)"));
    } else {
      QMenu menu(this);
      for (Node* c : cams) {
        uint32_t id = c->id;
        menu.addAction(QString::fromStdString(c->name), this, [this, id] { viewports_->active()->setCameraView(id); });
      }
      menu.exec(QCursor::pos());
    }
  });

  QAction* esc = act(tr("Cancel"), QKeySequence(Qt::Key_Escape));
  connect(esc, &QAction::triggered, this, [this] {
    bool cancelled = false;
    for (int i = 0; i < viewports_->count(); ++i) cancelled |= viewports_->viewport(i)->cancelInteraction();
    if (!cancelled && state_->creating()) state_->setCreateType({});
  });
}

namespace {
const char* const kTopMenus[] = {"&File",       "&Edit",          "&Tools",     "&Group",     "&Views",
                                 "&Create",     "&Modifiers",     "&Animation", "&Graph Editors",
                                 "&Rendering",  "C&ustomize",     "&Scripting", "&Help"};

QString plainMenuTitle(QString t) { return t.remove('&'); }

// Modifiers menu groups in 3ds Max order; unknown categories follow alphabetically.
const char* const kModifierCategories[] = {
    "Selection Modifiers",  "Patch/Spline Editing", "Mesh Editing",         "Conversion",
    "Animation",            "Cloth",                "Hair and Fur",         "UV Coordinates",
    "Cache Tools",          "Subdivision Surfaces", "Free Form Deformers",  "Parametric Deformers",
    "Surface",              "NURBS Editing",        "Radiosity",            "Cameras",
    "Rendering"};

// Create menu sections: (category, title used when the category has no subcategories).
const char* const kCreateCategories[] = {"Geometry", "Shapes", "Lights", "Cameras", "Helpers", "Space Warps",
                                         "Systems"};
}  // namespace

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
  commandPanel_->addTab(page, icon, QString());
  commandPanel_->setTabToolTip(commandPanel_->count() - 1, title);
}

void MainWindow::registerCommand(const QString& id, QAction* action) {
  action->setObjectName(id);
  commands_.insert(id, action);
}

void MainWindow::createMenus() {
  for (const char* title : kTopMenus) menuBar()->addMenu(tr(title));
  QMenu* file = menuAt("File");
  file->addAction(newAct_);
  file->addAction(openAct_);
  recentMenu_ = file->addMenu(tr("Open &Recent"));
  file->addSeparator();
  file->addAction(saveAct_);
  file->addAction(saveAsAct_);
  file->addSeparator();
  file->addAction(importAct_);
  file->addSeparator();
  file->addAction(exitAct_);

  QMenu* edit = menuAt("Edit");
  edit->addAction(undoAct_);
  edit->addAction(redoAct_);
  edit->addSeparator();
  edit->addAction(deleteAct_);
  edit->addAction(cloneAct_);
  edit->addSeparator();
  edit->addAction(selectAllAct_);
  edit->addAction(selectNoneAct_);
  edit->addAction(selectInvertAct_);
  edit->addAction(selectByNameAct_);
  edit->addSeparator();
  edit->addAction(selectToolAct_);
  edit->addAction(moveToolAct_);
  edit->addAction(rotateToolAct_);
  edit->addAction(scaleToolAct_);
  QMenu* axes = edit->addMenu(tr("Axis Constraints"));
  axes->addAction(axisXAct_);
  axes->addAction(axisYAct_);
  axes->addAction(axisZAct_);
  axes->addAction(axisPlaneAct_);
  edit->addSeparator();
  edit->addAction(propertiesAct_);

  QMenu* tools = menuAt("Tools");
  tools->addAction(selectByNameAct_);
  tools->addAction(cloneAct_);
  tools->addSeparator();
  tools->addAction(hideAct_);
  tools->addAction(unhideAct_);
  tools->addSeparator();
  QAction* convert = tools->addAction(tr("Convert to Editable Poly"));
  connect(convert, &QAction::triggered, this, [this] {
    QVector<uint32_t> ids;
    for (Node* n : doc_->selectedNodes())
      if (n->kind() == ObjectKind::Geometry) ids.push_back(n->id);
    if (ids.isEmpty()) return;
    doc_->beginEdit(tr("Convert to Editable Poly"));
    for (uint32_t id : ids) {
      Node* n = doc_->scene().findNode(id);
      auto poly = std::make_unique<EditablePolyObject>();
      poly->mesh = n->mesh();
      n->object = std::move(poly);
      n->modifiers.clear();
    }
    doc_->notifyNodesChanged(ids, ChangeGeometry);
    doc_->notifyStructureChanged();
    doc_->endEdit();
  });
  tools->addSeparator();
  tools->addAction(materialEditorAct_);

  // Create and Modifiers menus list every registered type; rebuilt when opened.
  QMenu* create = menuAt("Create");
  connect(create, &QMenu::aboutToShow, this, [this, create] {
    create->clear();
    auto addType = [this](QMenu* m, const ObjectTypeInfo* t) {
      QString type = QString::fromStdString(t->type);
      QString name = QString::fromStdString(t->displayName);
      m->addAction(name, this, [this, type, name] {
        state_->setCreateType(type);
        commandPanel_->showCreate();
        prompt_->setText(tr("Click and drag in a viewport to create a %1").arg(name));
      });
    };
    for (const char* cat : kCreateCategories) {
      std::vector<std::string> subs = objectSubcategories(cat);
      if (subs.empty()) continue;
      if (std::string(cat) != "Geometry") create->addSeparator();
      for (const std::string& sub : subs) {
        QString title = sub.empty() || std::string(cat) != "Geometry" ? QString::fromStdString(cat)
                                                                     : QString::fromStdString(sub);
        if (std::string(cat) != "Geometry" && subs.size() > 1 && !sub.empty())
          title = QString::fromStdString(cat) + " - " + QString::fromStdString(sub);
        QMenu* m = create->addMenu(title);
        for (const ObjectTypeInfo* t : objectTypesIn(cat, sub)) addType(m, t);
      }
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
      if (list.empty()) continue;
      std::sort(list.begin(), list.end(),
                [](const ModifierTypeInfo* a, const ModifierTypeInfo* b) { return a->displayName < b->displayName; });
      QMenu* m = mods->addMenu(QString::fromStdString(cat.empty() ? std::string("Other") : cat));
      for (const ModifierTypeInfo* t : list) {
        QString type = QString::fromStdString(t->type);
        m->addAction(QString::fromStdString(t->displayName), this, [this, type] { commandPanel_->addModifier(type); });
      }
    }
  });

  QMenu* rendering = menuAt("Rendering");
  rendering->addAction(renderAct_);
  rendering->addAction(renderSetupAct_);
  rendering->addAction(frameBufferAct_);
  rendering->addSeparator();
  rendering->addAction(materialEditorAct_);

  QMenu* views = menuAt("Views");
  views->addAction(undoViewAct_);
  views->addSeparator();
  for (const char* k : {"view_P", "view_U", "view_T", "view_B", "view_F", "view_L", "view_C"})
    views->addAction(findChild<QAction*>(k));
  views->addAction(cameraFromViewAct_);
  views->addSeparator();
  views->addAction(shadingAct_);
  views->addAction(edgedAct_);
  views->addAction(gridAct_);
  views->addSeparator();
  views->addAction(zoomExtAct_);
  views->addAction(zoomExtAllAct_);
  views->addAction(maximizeAct_);

  QMenu* help = menuAt("Help");
  QAction* keys = help->addAction(tr("&Keyboard Shortcuts"), this, [this] { showShortcutsDialog(this); });
  keys->setShortcut(QKeySequence::HelpContents);
  addAction(keys);
  help->addAction(tr("&About Veraa"), this, [this] {
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
}

void MainWindow::createToolBar() {
  QToolBar* tb = addToolBar(tr("Main Toolbar"));
  mainToolBar_ = tb;
  tb->setObjectName("MainToolbar");
  tb->setIconSize(QSize(22, 22));
  tb->setMovable(false);
  tb->addAction(newAct_);
  tb->addAction(openAct_);
  tb->addAction(saveAct_);
  tb->addSeparator();
  tb->addAction(undoAct_);
  tb->addAction(redoAct_);
  tb->addSeparator();
  tb->addAction(selectToolAct_);
  tb->addAction(moveToolAct_);
  tb->addAction(rotateToolAct_);
  tb->addAction(scaleToolAct_);
  tb->addSeparator();
  tb->addAction(axisXAct_);
  tb->addAction(axisYAct_);
  tb->addAction(axisZAct_);
  tb->addAction(axisPlaneAct_);
  tb->addSeparator();
  tb->addAction(zoomExtAct_);
  tb->addAction(maximizeAct_);
  tb->addSeparator();
  tb->addAction(materialEditorAct_);
  tb->addAction(renderSetupAct_);
  tb->addAction(renderAct_);
}

void MainWindow::createDocks() {
  commandPanel_ = new CommandPanel(doc_, state_, this);
  auto* right = new QDockWidget(tr("Command Panel"), this);
  right->setObjectName("CommandPanelDock");
  right->setWidget(commandPanel_);
  right->setFeatures(QDockWidget::DockWidgetMovable);
  right->setTitleBarWidget(new QWidget(right));
  right->setMinimumWidth(270);
  addDockWidget(Qt::RightDockWidgetArea, right);

  explorer_ = new SceneExplorer(doc_, this);
  auto* left = new QDockWidget(tr("Scene Explorer"), this);
  left->setObjectName("SceneExplorerDock");
  left->setWidget(explorer_);
  left->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::LeftDockWidgetArea, left);
  resizeDocks({left, right}, {230, 290}, Qt::Horizontal);

  QMenu* showUi = menuAt("Customize/Show UI");
  showUi->addAction(left->toggleViewAction());
  showUi->addAction(right->toggleViewAction());
}

void MainWindow::createStatusBar() {
  QStatusBar* sb = statusBar();
  sb->setSizeGripEnabled(true);
  prompt_ = new QLabel(tr(kDefaultPrompt), sb);
  prompt_->setObjectName("Prompt");
  prompt_->setMinimumWidth(260);
  sb->addWidget(prompt_, 1);
  selInfo_ = new QLabel(sb);
  sb->addPermanentWidget(selInfo_);
  typeInLabel_ = new QLabel(sb);
  sb->addPermanentWidget(typeInLabel_);
  const char* names[3] = {"X:", "Y:", "Z:"};
  for (int i = 0; i < 3; ++i) {
    auto* l = new QLabel(names[i], sb);
    sb->addPermanentWidget(l);
    auto* s = new QDoubleSpinBox(sb);
    s->setRange(-1e7, 1e7);
    s->setDecimals(3);
    s->setKeyboardTracking(false);
    s->setFixedWidth(96);
    s->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    installSpinnerScrub(s);
    typeIn_[i] = s;
    sb->addPermanentWidget(s);
    connect(s, &QDoubleSpinBox::valueChanged, this, [this, i](double v) {
      if (!updatingTypeIn_) applyTypeIn(i, v);
    });
  }
  gridInfo_ = new QLabel(tr("Grid = 10.0"), sb);
  gridInfo_->setMinimumWidth(80);
  sb->addPermanentWidget(gridInfo_);
}

// ---------------------------------------------------------------------------------------------

void MainWindow::updateTitle() {
  QString name = doc_->filePath().isEmpty() ? tr("Untitled") : QFileInfo(doc_->filePath()).fileName();
  setWindowTitle(QString("Veraa - %1%2").arg(name, doc_->isModified() ? " *" : ""));
}

void MainWindow::updateTypeIn() {
  updatingTypeIn_ = true;
  auto nodes = doc_->selectedNodes();
  TransformTool tool = state_->tool();
  bool single = nodes.size() == 1;
  bool enabled = !nodes.isEmpty() && (single || tool == TransformTool::Move || tool == TransformTool::Select);
  Vec3 v;
  QString label = tr("Position");
  if (single && tool == TransformTool::Rotate) {
    v = nodes[0]->transform.rotation;
    label = tr("Rotation");
  } else if (single && tool == TransformTool::Scale) {
    v = nodes[0]->transform.scale * 100.0f;
    label = tr("Scale %");
  } else if (!nodes.isEmpty()) {
    v = doc_->selectionCenter();
  }
  typeInLabel_->setText(label);
  for (int i = 0; i < 3; ++i) {
    QSignalBlocker b(typeIn_[i]);
    typeIn_[i]->setEnabled(enabled);
    typeIn_[i]->setValue(enabled ? v[i] : 0.0);
  }
  if (nodes.isEmpty()) selInfo_->setText(tr("None Selected"));
  else if (single) selInfo_->setText(tr("1 Object Selected"));
  else selInfo_->setText(tr("%1 Objects Selected").arg(nodes.size()));
  updatingTypeIn_ = false;
}

void MainWindow::applyTypeIn(int axis, double value) {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  TransformTool tool = state_->tool();
  doc_->softEdit(tr("Transform Type-In"), QString("typein/%1").arg(axis));
  QVector<uint32_t> ids;
  if (nodes.size() == 1 && tool == TransformTool::Rotate) {
    nodes[0]->transform.rotation[axis] = static_cast<float>(value);
  } else if (nodes.size() == 1 && tool == TransformTool::Scale) {
    nodes[0]->transform.scale[axis] = static_cast<float>(value / 100.0);
  } else {
    float delta = static_cast<float>(value) - doc_->selectionCenter()[axis];
    for (Node* n : nodes) n->transform.position[axis] += delta;
  }
  for (Node* n : nodes) ids.push_back(n->id);
  doc_->notifyNodesChanged(ids, ChangeTransform);
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
  while (files.size() > 8) files.removeLast();
  s.setValue("recentFiles", files);
  updateRecentMenu();
}

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
    if (nodes.size() > 1) dlg.setWindowTitle(tr("Clone Options (%1 objects)").arg(nodes.size()));
    if (dlg.exec() != QDialog::Accepted) return;
    name = dlg.name();
  }
  doc_->beginEdit(tr("Clone"));
  QVector<uint32_t> ids = doc_->cloneSelected(true);
  if (ids.size() == 1 && !name.isEmpty())
    if (Node* n = doc_->scene().findNode(ids[0])) {
      n->name = name.toStdString();
      doc_->notifyNodeChanged(n, ChangeName);
    }
  doc_->endEdit();
  state_->setTool(TransformTool::Move);
  prompt_->setText(tr("Clones created and selected; drag them to a new place"));
}

bool MainWindow::openFile(const QString& path) {
  QString err;
  if (!doc_->load(path, &err)) {
    QMessageBox::warning(this, tr("Open Scene"), tr("Could not open %1:\n%2").arg(path, err));
    return false;
  }
  addRecent(path);
  viewports_->zoomExtentsAll(false);
  prompt_->setText(tr("Opened %1").arg(QFileInfo(path).fileName()));
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
  prompt_->setText(tr("Saved %1").arg(QFileInfo(doc_->filePath()).fileName()));
  return true;
}

bool MainWindow::saveAs() {
  QString start = doc_->filePath().isEmpty() ? tr("untitled.veraa") : doc_->filePath();
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
  QString f = QFileDialog::getOpenFileName(this, tr("Import OBJ"), s.value("lastImportDir").toString(),
                                           tr("Wavefront OBJ (*.obj);;All files (*)"));
  if (f.isEmpty()) return;
  s.setValue("lastImportDir", QFileInfo(f).absolutePath());
  ObjImportDialog opts(this);
  if (opts.exec() != QDialog::Accepted) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  doc_->beginEdit(tr("Import OBJ"));
  ObjImportResult res;
  std::string err;
  bool ok = importObj(doc_->scene(), f.toStdString(), opts.options(), &res, &err);
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
  prompt_->setText(tr("Imported %1 object(s), %2 triangles").arg(res.nodeIds.size()).arg(res.triangles));
  if (!res.warnings.empty()) {
    QStringList w;
    for (const auto& x : res.warnings) w << QString::fromStdString(x);
    QMessageBox::information(this, tr("Import OBJ"), w.mid(0, 20).join("\n"));
  }
}

void MainWindow::showContextMenu(Viewport* vp, const QPoint& pos) {
  QMenu menu(this);
  menu.addSection(tr("Transform"));
  menu.addAction(moveToolAct_);
  menu.addAction(rotateToolAct_);
  menu.addAction(scaleToolAct_);
  menu.addAction(selectToolAct_);
  menu.addAction(cloneAct_);
  menu.addAction(propertiesAct_);
  menu.addSection(tr("Display"));
  menu.addAction(hideAct_);
  menu.addAction(unhideAct_);
  menu.addAction(tr("Zoom Extents Selected"), this, [vp] { vp->zoomExtents(true); });
  menu.addSection(tr("Edit"));
  menu.addAction(deleteAct_);
  for (QAction* a : menuBar()->actions())
    if (a->menu() && a->text() == tr("&Tools"))
      for (QAction* t : a->menu()->actions())
        if (t->text() == tr("Convert to Editable Poly")) menu.addAction(t);
  menu.addAction(materialEditorAct_);
  bool hasSel = !doc_->selection().isEmpty();
  cloneAct_->setEnabled(hasSel);
  hideAct_->setEnabled(hasSel);
  deleteAct_->setEnabled(hasSel);
  propertiesAct_->setEnabled(hasSel);
  menu.exec(pos);
  cloneAct_->setEnabled(true);
  hideAct_->setEnabled(true);
  deleteAct_->setEnabled(true);
  propertiesAct_->setEnabled(true);
}

// ---------------------------------------------------------------------------------------------

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
