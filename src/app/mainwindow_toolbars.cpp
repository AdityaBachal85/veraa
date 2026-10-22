// Main toolbar (undo, link, selection filter, selection tools, transforms, reference coordinate
// system, pivot center, manipulate, shortcut override, snaps, named selection sets, mirror and
// align, explorers, graph editors, rendering), the axis constraint toolbar, workspaces and
// commands owned by other modules (proxies).
#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "commandpanel.h"
#include "edittools.h"
#include "icons.h"
#include "icons_ui.h"
#include "mainwindow.h"
#include "tooldialogs.h"
#include "uistate.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

QSettings appSettings() { return QSettings("Veraa", "Veraa"); }

// Selection filter categories (bit per object kind).
enum FilterBits { kGeometry = 1, kShapes = 2, kLights = 4, kCameras = 8, kHelpers = 16, kWarps = 32, kAll = 63 };

int kindBit(ObjectKind k) {
  switch (k) {
    case ObjectKind::Geometry: return kGeometry;
    case ObjectKind::Shape: return kShapes;
    case ObjectKind::Light: return kLights;
    case ObjectKind::Camera: return kCameras;
    case ObjectKind::Helper: return kHelpers;
    case ObjectKind::SpaceWarp: return kWarps;
  }
  return kAll;
}

QString filterName(int bits) {
  static const struct {
    int bit;
    const char* name;
  } names[] = {{kGeometry, "Geometry"}, {kShapes, "Shapes"}, {kLights, "Lights"},
               {kCameras, "Cameras"},   {kHelpers, "Helpers"}, {kWarps, "Warps"}};
  if (bits == kAll) return QObject::tr("All");
  QStringList parts;
  for (const auto& n : names)
    if (bits & n.bit) parts << QObject::tr(n.name);
  return parts.join('+');
}

// A tool button whose main click repeats the last action picked from its menu (3ds Max flyout).
QToolButton* flyout(QWidget* parent, const QList<QAction*>& actions, QAction* initial = nullptr) {
  auto* b = new QToolButton(parent);
  auto* menu = new QMenu(b);
  for (QAction* a : actions) menu->addAction(a);
  b->setMenu(menu);
  b->setPopupMode(QToolButton::MenuButtonPopup);
  b->setDefaultAction(initial ? initial : actions.front());
  QObject::connect(menu, &QMenu::triggered, b, [b](QAction* a) { b->setDefaultAction(a); });
  b->setAutoRaise(true);
  return b;
}

}  // namespace

QAction* MainWindow::proxyCommand(const QString& id, const QString& text, const QIcon& icon) {
  // Reuse one placeholder per id, so menus and toolbars share it.
  for (QAction* p : proxies_)
    if (p->property("proxyId").toString() == id) return p;
  auto* a = new QAction(icon, text, this);
  a->setProperty("proxyId", id);
  a->setProperty("proxyText", text);
  a->setEnabled(false);
  a->setToolTip(tr("%1 (not available)").arg(text));
  proxies_.push_back(a);
  connect(a, &QAction::triggered, this, [this, a](bool checked) {
    QAction* real = command(a->property("proxyId").toString());
    if (!real || real == a) return;
    if (real->isCheckable()) real->setChecked(checked);
    else real->trigger();
  });
  return a;
}

void MainWindow::resolveProxyCommands() {
  // Fallback implementations for commands whose modules may not be installed.
  auto ensure = [this](const QString& id, const std::function<QAction*()>& make) {
    if (!command(id)) make();
  };
  ensure("tools.unlink", [this] {
    QAction* a = cmd("tools.unlink", tr("Unlink Selection"), QKeySequence(), icons::unlink());
    connect(a, &QAction::triggered, this, [this] {
      if (doc_->selection().isEmpty()) return;
      doc_->beginEdit(tr("Unlink"));
      for (uint32_t id : doc_->selection()) doc_->scene().setParent(id, 0);
      doc_->notifyNodesChanged(doc_->selection(), ChangeTransform);
      doc_->notifyStructureChanged();
      doc_->endEdit();
    });
    return a;
  });
  ensure("tools.selectAndLink", [this] {
    QAction* a = cmd("tools.selectAndLink", tr("Select and Link"), QKeySequence(), icons::link());
    connect(a, &QAction::triggered, this, [this] {
      state_->setTool(TransformTool::Select);
      state_->setViewportTool(makeLinkTool(doc_, state_));
    });
    return a;
  });
  ensure("snaps.angle", [this] {
    QAction* a = cmd("snaps.angle", tr("Angle Snap Toggle"), QKeySequence("A"), icons::angleSnap());
    a->setCheckable(true);
    connect(a, &QAction::toggled, this, [this](bool on) { state_->setAngleSnap(on, state_->angleSnapStep()); });
    connect(state_, &EditorState::snapSettingsChanged, a, [this, a] {
      QSignalBlocker b(a);
      a->setChecked(state_->angleSnap());
    });
    return a;
  });
  ensure("snaps.percent", [this] {
    QAction* a = cmd("snaps.percent", tr("Percent Snap Toggle"), QKeySequence("Shift+Ctrl+P"), icons::percentSnap());
    a->setCheckable(true);
    connect(a, &QAction::toggled, this, [this](bool on) { state_->setPercentSnap(on, state_->percentSnapStep()); });
    connect(state_, &EditorState::snapSettingsChanged, a, [this, a] {
      QSignalBlocker b(a);
      a->setChecked(state_->percentSnap());
    });
    return a;
  });
  ensure("snaps.toggle", [this] {
    QAction* a = cmd("snaps.toggle", tr("Snaps Toggle (Grid Points)"), QKeySequence("S"), icons::snap(3));
    a->setCheckable(true);
    connect(a, &QAction::toggled, this, [this](bool on) { installGridSnapper(state_, on); });
    return a;
  });
  ensure("snaps.spinner", [this] {
    QAction* a = cmd("snaps.spinner", tr("Spinner Snap Toggle"), QKeySequence(), icons::spinnerSnap());
    a->setCheckable(true);
    connect(a, &QAction::toggled, this, [this](bool on) { setSpinnerSnap(on); });
    return a;
  });
  ensure("edit.mirror", [this] {
    QAction* a = cmd("edit.mirror", tr("Mirror..."), QKeySequence(), icons::mirror());
    connect(a, &QAction::triggered, this, [this] {
      if (doc_->selection().isEmpty()) {
        setPrompt(tr("Select objects to mirror first"));
        return;
      }
      MirrorDialog dlg(doc_, this);
      dlg.exec();
    });
    return a;
  });

  for (QAction* p : proxies_) {
    QString id = p->property("proxyId").toString();
    QString text = p->property("proxyText").toString();
    QAction* real = command(id);
    if (!real) {
      p->setEnabled(false);
      continue;
    }
    // If the module already put its action in the same menu, drop the placeholder there.
    for (QObject* o : p->associatedObjects())
      if (auto* m = qobject_cast<QMenu*>(o); m && m->actions().contains(real) && m->actions().contains(p))
        m->removeAction(p);
    auto sync = [p, real, text] {
      p->setEnabled(real->isEnabled());
      p->setCheckable(real->isCheckable());
      if (real->isCheckable()) {
        QSignalBlocker b(p);
        p->setChecked(real->isChecked());
      }
      QString key = real->shortcut().toString(QKeySequence::NativeText);
      p->setText(key.isEmpty() ? text : text + '\t' + key);
      p->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key));
      if (p->icon().isNull()) p->setIcon(real->icon());
    };
    sync();
    connect(real, &QAction::changed, p, sync);
    connect(real, &QAction::toggled, p, [p](bool on) {
      QSignalBlocker b(p);
      p->setChecked(on);
    });
  }
}

void MainWindow::setSpinnerSnap(bool on) {
  // Spinner snap: arrow clicks and scrubbing step by the preference value instead of the
  // parameter's own step.
  double step = appSettings().value("prefs/spinnerSnap", 1.0).toDouble();
  for (QDoubleSpinBox* sb : findChildren<QDoubleSpinBox*>()) {
    if (on) {
      if (!sb->property("veraaStep").isValid()) sb->setProperty("veraaStep", sb->singleStep());
      sb->setSingleStep(step);
    } else if (sb->property("veraaStep").isValid()) {
      sb->setSingleStep(sb->property("veraaStep").toDouble());
      sb->setProperty("veraaStep", QVariant());
    }
  }
  spinnerSnap_ = on;
}

void MainWindow::createToolBar() {
  mainToolBar_ = addToolBar(tr("Main Toolbar"));
  mainToolBar_->setObjectName("MainToolBar");
  mainToolBar_->setIconSize(QSize(22, 22));
  QToolBar* tb = mainToolBar_;

  tb->addAction(undoAct_);
  tb->addAction(redoAct_);
  tb->addSeparator();
  tb->addAction(proxyCommand("tools.selectAndLink", tr("Select and Link"), icons::link()));
  tb->addAction(proxyCommand("tools.unlink", tr("Unlink Selection"), icons::unlink()));
  tb->addAction(proxyCommand("tools.bindSpaceWarp", tr("Bind to Space Warp"), icons::bindSpaceWarp()));
  tb->addSeparator();

  // Selection filter.
  filterCombo_ = new QComboBox(tb);
  filterCombo_->setToolTip(tr("Selection Filter"));
  filterCombo_->setMinimumContentsLength(7);
  for (int bits : {int(kAll), int(kGeometry), int(kShapes), int(kLights), int(kCameras), int(kHelpers), int(kWarps)})
    filterCombo_->addItem(filterName(bits), bits);
  for (const QVariant& v : appSettings().value("ui/filterCombos").toList())
    if (filterCombo_->findData(v.toInt()) < 0) filterCombo_->addItem(filterName(v.toInt()), v.toInt());
  filterCombo_->insertSeparator(filterCombo_->count());
  filterCombo_->addItem(tr("Combos..."), -1);
  connect(filterCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
    int bits = filterCombo_->itemData(index).toInt();
    if (bits == -1) {
      // Combos: build a combination of categories.
      QDialog dlg(this);
      dlg.setWindowTitle(tr("Filter Combinations"));
      auto* v = new QVBoxLayout(&dlg);
      QList<QPair<QCheckBox*, int>> boxes;
      for (int bit : {int(kGeometry), int(kShapes), int(kLights), int(kCameras), int(kHelpers), int(kWarps)}) {
        auto* c = new QCheckBox(filterName(bit), &dlg);
        v->addWidget(c);
        boxes.push_back({c, bit});
      }
      auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
      v->addWidget(box);
      connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
      connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
      int chosen = 0;
      if (dlg.exec() == QDialog::Accepted)
        for (const auto& b : boxes)
          if (b.first->isChecked()) chosen |= b.second;
      if (!chosen) {
        filterCombo_->setCurrentIndex(0);
        bits = kAll;
      } else {
        int at = filterCombo_->findData(chosen);
        if (at < 0) {
          at = filterCombo_->count() - 2;  // before the separator
          filterCombo_->insertItem(at, filterName(chosen), chosen);
          QVariantList saved = appSettings().value("ui/filterCombos").toList();
          saved.push_back(chosen);
          QSettings s = appSettings();
          s.setValue("ui/filterCombos", saved);
        }
        filterCombo_->setCurrentIndex(at);
        bits = chosen;
      }
    }
    if (bits == kAll) state_->setSelectionFilter({});
    else state_->setSelectionFilter([bits](const Node& n) { return (kindBit(n.kind()) & bits) != 0; });
    // Drop selected objects the filter no longer accepts.
    QVector<uint32_t> keep;
    for (Node* n : doc_->selectedNodes())
      if (state_->selectionFilterAccepts(*n)) keep.push_back(n->id);
    if (keep.size() != doc_->selection().size()) doc_->setSelection(keep);
    setPrompt(tr("Selection filter: %1").arg(filterName(bits)));
  });
  tb->addWidget(filterCombo_);

  // Selection tools.
  tb->addAction(selectToolAct_);
  tb->addAction(selectByNameAct_);
  QList<QAction*> regionActs;
  for (const char* id : {"edit.regionRectangle", "edit.regionCircle", "edit.regionFence", "edit.regionLasso",
                         "edit.regionPaint"})
    regionActs << command(id);
  QToolButton* regionBtn = flyout(tb, regionActs);
  regionBtn->setObjectName("RegionFlyout");
  tb->addWidget(regionBtn);
  connect(&UiState::instance(), &UiState::changed, regionBtn, [regionBtn, regionActs] {
    QAction* a = regionActs.value(static_cast<int>(UiState::instance().regionShape()));
    if (a && regionBtn->defaultAction() != a) {
      a->setChecked(true);
      regionBtn->setDefaultAction(a);
    }
  });
  tb->addAction(command("edit.windowCrossing"));
  tb->addSeparator();

  // Transforms.
  tb->addAction(moveToolAct_);
  tb->addAction(rotateToolAct_);
  QToolButton* scaleBtn = flyout(tb, {scaleToolAct_, command("edit.scaleNonUniform"), command("edit.scaleSquash")});
  scaleBtn->setObjectName("ScaleFlyout");
  tb->addWidget(scaleBtn);
  tb->addAction(placeToolAct_);

  // Reference coordinate system.
  refCoordCombo_ = new QComboBox(tb);
  refCoordCombo_->setToolTip(tr("Reference Coordinate System"));
  for (RefCoordSys s : {RefCoordSys::View, RefCoordSys::Screen, RefCoordSys::World, RefCoordSys::Parent,
                        RefCoordSys::Local, RefCoordSys::Gimbal, RefCoordSys::Grid, RefCoordSys::Working,
                        RefCoordSys::LocalAligned, RefCoordSys::Pick})
    refCoordCombo_->addItem(refCoordSysName(s), static_cast<int>(s));
  connect(refCoordCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
    QVariant data = refCoordCombo_->itemData(index);
    if (data.toString().startsWith("node:")) {
      // A previously picked object.
      UiState::instance().setPickCoordNode(data.toString().mid(5).toUInt());
      UiState::instance().setRefCoordSys(RefCoordSys::Pick);
      return;
    }
    auto sys = static_cast<RefCoordSys>(data.toInt());
    if (sys == RefCoordSys::Pick) {
      setPrompt(tr("Click an object to use its coordinate system"));
      state_->setViewportTool(makePickObjectTool(doc_, tr("Pick the coordinate system object"), [this](uint32_t id) {
        Node* n = doc_->scene().findNode(id);
        if (!n) return;
        QString key = QString("node:%1").arg(id);
        int at = refCoordCombo_->findData(key);
        if (at < 0) {
          refCoordCombo_->addItem(QString::fromStdString(n->name), key);
          at = refCoordCombo_->count() - 1;
        }
        refCoordCombo_->setCurrentIndex(at);
        UiState::instance().setPickCoordNode(id);
        UiState::instance().setRefCoordSys(RefCoordSys::Pick);
        state_->setViewportTool(nullptr);
        setPrompt(tr("Transforms use the axes of %1").arg(QString::fromStdString(n->name)));
      }));
      return;
    }
    UiState::instance().setRefCoordSys(sys);
  });
  connect(&UiState::instance(), &UiState::changed, refCoordCombo_, [this] {
    const UiState& ui = UiState::instance();
    QVariant want = ui.refCoordSys() == RefCoordSys::Pick && ui.pickCoordNode()
                        ? QVariant(QString("node:%1").arg(ui.pickCoordNode()))
                        : QVariant(static_cast<int>(ui.refCoordSys()));
    int at = refCoordCombo_->findData(want);
    if (at >= 0 && at != refCoordCombo_->currentIndex()) {
      QSignalBlocker b(refCoordCombo_);
      refCoordCombo_->setCurrentIndex(at);
    }
  });
  tb->addWidget(refCoordCombo_);

  // Pivot center flyout.
  auto* pivotGroup = new QActionGroup(this);
  QList<QAction*> pivotActs;
  const struct {
    const char* id;
    const char* text;
    PivotCenter c;
  } pivots[] = {{"edit.pivotPointCenter", "Use Pivot Point Center", PivotCenter::PivotPoint},
                {"edit.selectionCenter", "Use Selection Center", PivotCenter::Selection},
                {"edit.coordinateCenter", "Use Transform Coordinate Center", PivotCenter::Coordinate}};
  for (const auto& pc : pivots) {
    QAction* a = cmd(pc.id, tr(pc.text), QKeySequence(), icons::pivotCenter(static_cast<int>(pc.c)));
    a->setCheckable(true);
    a->setChecked(UiState::instance().pivotCenter() == pc.c);
    pivotGroup->addAction(a);
    PivotCenter c = pc.c;
    connect(a, &QAction::triggered, this, [this, c] {
      pivotAuto_ = false;
      UiState::instance().setPivotCenter(c);
    });
    pivotActs << a;
  }
  QToolButton* pivotBtn = flyout(tb, pivotActs);
  pivotBtn->setObjectName("PivotFlyout");
  tb->addWidget(pivotBtn);
  connect(&UiState::instance(), &UiState::changed, pivotBtn, [pivotBtn, pivotActs] {
    QAction* a = pivotActs.value(static_cast<int>(UiState::instance().pivotCenter()));
    if (a && pivotBtn->defaultAction() != a) {
      a->setChecked(true);
      pivotBtn->setDefaultAction(a);
    }
  });
  // Like 3ds Max: several selected objects switch to the selection center automatically.
  connect(doc_, &SceneDocument::selectionChanged, this, [this] {
    UiState& ui = UiState::instance();
    if (doc_->selection().size() > 1 && ui.pivotCenter() == PivotCenter::PivotPoint) {
      pivotAuto_ = true;
      ui.setPivotCenter(PivotCenter::Selection);
    } else if (doc_->selection().size() <= 1 && pivotAuto_ && ui.pivotCenter() == PivotCenter::Selection) {
      pivotAuto_ = false;
      ui.setPivotCenter(PivotCenter::PivotPoint);
    }
  });
  tb->addSeparator();

  // Select and Manipulate, keyboard shortcut override.
  QAction* manip = cmd("edit.manipulate", tr("Select and Manipulate"), QKeySequence(), icons::manipulate());
  manip->setCheckable(true);
  connect(manip, &QAction::toggled, this, [this](bool on) {
    if (on) {
      state_->setTool(TransformTool::Select);
      state_->setViewportTool(makeManipulateTool(doc_, state_));
    } else if (state_->viewportTool() && state_->viewportTool()->prompt().startsWith(tr("Manipulate"))) {
      state_->setViewportTool(nullptr);
    }
  });
  connect(state_, &EditorState::viewportToolChanged, manip, [this, manip] {
    bool on = state_->viewportTool() && state_->viewportTool()->prompt().startsWith(tr("Manipulate"));
    QSignalBlocker b(manip);
    manip->setChecked(on);
  });
  tb->addAction(manip);
  QAction* override = cmd("customize.shortcutOverride", tr("Keyboard Shortcut Override Toggle"), QKeySequence(),
                          icons::keyboardOverride());
  override->setCheckable(true);
  override->setToolTip(tr("Keyboard Shortcut Override Toggle: active tools receive their own keys"));
  connect(override, &QAction::toggled, this, [](bool on) { UiState::instance().setShortcutOverride(on); });
  tb->addAction(override);
  tb->addSeparator();

  // Snaps.
  tb->addAction(proxyCommand("snaps.toggle", tr("Snaps Toggle"), icons::snap(3)));
  tb->addAction(proxyCommand("snaps.angle", tr("Angle Snap Toggle"), icons::angleSnap()));
  tb->addAction(proxyCommand("snaps.percent", tr("Percent Snap Toggle"), icons::percentSnap()));
  tb->addAction(proxyCommand("snaps.spinner", tr("Spinner Snap Toggle"), icons::spinnerSnap()));
  tb->addSeparator();

  // Named selection sets.
  QAction* editSets = cmd("edit.editNamedSets", tr("Edit Named Selection Sets"), QKeySequence(), icons::namedSets());
  connect(editSets, &QAction::triggered, command("edit.selectionSets"), &QAction::trigger);
  tb->addAction(editSets);
  selSetCombo_ = new QComboBox(tb);
  selSetCombo_->setEditable(true);
  selSetCombo_->setInsertPolicy(QComboBox::NoInsert);
  selSetCombo_->setMinimumContentsLength(12);
  selSetCombo_->setToolTip(tr("Create Selection Set: type a name and press Enter to save the selection"));
  selSetCombo_->lineEdit()->setPlaceholderText(tr("Create Selection Set"));
  auto refreshSets = [this] {
    QSignalBlocker b(selSetCombo_);
    QString cur = selSetCombo_->currentText();
    selSetCombo_->clear();
    for (const QString& name : selectionSetNames(doc_->scene())) selSetCombo_->addItem(name);
    selSetCombo_->setCurrentIndex(-1);
    selSetCombo_->setEditText(QString());
  };
  connect(doc_, &SceneDocument::structureChanged, selSetCombo_, refreshSets);
  connect(doc_, &SceneDocument::fileChanged, selSetCombo_, refreshSets);
  connect(selSetCombo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
    QString name = selSetCombo_->itemText(index);
    QVector<uint32_t> ids = selectionSetMembers(doc_->scene(), name);
    doc_->setSelection(ids);
    setPrompt(tr("Selection set \"%1\": %2 object(s)").arg(name).arg(ids.size()));
  });
  connect(selSetCombo_->lineEdit(), &QLineEdit::returnPressed, this, [this, refreshSets] {
    QString name = selSetCombo_->currentText().trimmed();
    if (name.isEmpty()) return;
    if (selectionSetNames(doc_->scene()).contains(name)) {
      doc_->setSelection(selectionSetMembers(doc_->scene(), name));
      return;
    }
    if (doc_->selection().isEmpty()) {
      setPrompt(tr("Select objects before creating a selection set"));
      return;
    }
    setSelectionSet(doc_, name, doc_->selection());
    refreshSets();
    setPrompt(tr("Created selection set \"%1\"").arg(name));
  });
  tb->addWidget(selSetCombo_);
  tb->addSeparator();

  // Mirror, align.
  tb->addAction(proxyCommand("edit.mirror", tr("Mirror..."), icons::mirror()));
  QToolButton* alignBtn = flyout(tb, {proxyCommand("tools.align", tr("Align..."), icons::align()),
                                      proxyCommand("tools.quickAlign", tr("Quick Align"), icons::align()),
                                      proxyCommand("tools.normalAlign", tr("Normal Align..."), icons::align()),
                                      proxyCommand("tools.alignToView", tr("Align to View..."), icons::align())});
  alignBtn->setObjectName("AlignFlyout");
  tb->addWidget(alignBtn);
  tb->addSeparator();

  // Explorers.
  QAction* toggleScene = cmd("ui.toggleSceneExplorer", tr("Toggle Scene Explorer"), QKeySequence(), icons::sceneExplorer());
  toggleScene->setCheckable(true);
  toggleScene->setChecked(true);
  connect(toggleScene, &QAction::triggered, this, [this](bool on) {
    explorerDock_->setVisible(on);
    if (on) explorerDock_->raise();
  });
  tb->addAction(toggleScene);
  QAction* toggleLayer = cmd("ui.toggleLayerExplorer", tr("Toggle Layer Explorer"), QKeySequence(), icons::layerExplorer());
  toggleLayer->setCheckable(true);
  connect(toggleLayer, &QAction::triggered, this, [this](bool on) {
    layerDock_->setVisible(on);
    if (on) layerDock_->raise();
  });
  tb->addAction(toggleLayer);
  tb->addSeparator();

  // Graph editors.
  tb->addAction(proxyCommand("graph.curveEditor", tr("Curve Editor"), icons::curveEditor()));
  tb->addAction(proxyCommand("graph.schematic", tr("Schematic View"), icons::schematic()));
  tb->addSeparator();

  // Rendering.
  tb->addAction(materialEditorAct_);
  tb->addAction(renderSetupAct_);
  tb->addAction(frameBufferAct_);
  tb->addAction(renderAct_);
}

void MainWindow::createAxisToolBar() {
  axisToolBar_ = new QToolBar(tr("Axis Constraints"), this);
  axisToolBar_->setObjectName("AxisConstraintsToolBar");
  axisToolBar_->setIconSize(QSize(20, 20));
  axisToolBar_->setOrientation(Qt::Vertical);
  addToolBar(Qt::LeftToolBarArea, axisToolBar_);
  axisToolBar_->addAction(axisXAct_);
  axisToolBar_->addAction(axisYAct_);
  axisToolBar_->addAction(axisZAct_);
  // Plane constraint flyout XY / YZ / ZX.
  QList<QAction*> planes;
  for (GizmoHandle h : {GizmoHandle::XY, GizmoHandle::YZ, GizmoHandle::ZX}) {
    auto* a = new QAction(icons::axis(handleName(h)), handleName(h), this);
    connect(a, &QAction::triggered, this, [this, h] { state_->setConstraint(h); });
    planes << a;
  }
  QToolButton* planeBtn = flyout(axisToolBar_, planes);
  planeBtn->setCheckable(true);
  planeBtn->setObjectName("PlaneFlyout");
  axisToolBar_->addWidget(planeBtn);
  connect(state_, &EditorState::constraintChanged, planeBtn, [planeBtn, planes](GizmoHandle h) {
    int i = h == GizmoHandle::XY ? 0 : h == GizmoHandle::YZ ? 1 : h == GizmoHandle::ZX ? 2 : -1;
    planeBtn->setChecked(i >= 0);
    if (i >= 0) planeBtn->setDefaultAction(planes[i]);
    planeBtn->setChecked(i >= 0);
  });
  axisToolBar_->addAction(axisCycleAct_);
  axisToolBar_->addSeparator();
  axisToolBar_->addAction(proxyCommand("snaps.toggle", tr("Snaps Toggle"), icons::snap(3)));

  QMenu* showUi = menuAt("Customize/Show UI");
  QAction* main = mainToolBar_->toggleViewAction();
  main->setText(tr("Show Main Toolbar"));
  registerCommand("ui.mainToolbar", main);
  showUi->addAction(main);
  QAction* axis = axisToolBar_->toggleViewAction();
  axis->setText(tr("Show Axis Constraints Toolbar"));
  registerCommand("ui.axisToolbar", axis);
  showUi->addAction(axis);
}

// ---------------------------------------------------------------------------------------------
// Workspaces

void MainWindow::createWorkspaceSelector() {
  auto* host = new QWidget(menuBar());
  auto* h = new QHBoxLayout(host);
  h->setContentsMargins(0, 0, 6, 0);
  h->setSpacing(4);
  h->addWidget(new QLabel(tr("Workspaces:"), host));
  workspaceCombo_ = new QComboBox(host);
  workspaceCombo_->setMinimumContentsLength(14);
  workspaceCombo_->setObjectName("WorkspaceCombo");
  h->addWidget(workspaceCombo_);
  menuBar()->setCornerWidget(host, Qt::TopRightCorner);
  auto refresh = [this] {
    QSignalBlocker b(workspaceCombo_);
    QString cur = appSettings().value("workspaces/current", tr("Default")).toString();
    workspaceCombo_->clear();
    workspaceCombo_->addItem(tr("Default"), QString("ws:Default"));
    for (const QString& n : workspaceNames())
      if (n != tr("Default")) workspaceCombo_->addItem(n, QString("ws:%1").arg(n));
    workspaceCombo_->insertSeparator(workspaceCombo_->count());
    workspaceCombo_->addItem(tr("Manage Workspaces..."), QString("manage"));
    workspaceCombo_->addItem(tr("Reset to Default State"), QString("reset"));
    int at = workspaceCombo_->findText(cur);
    workspaceCombo_->setCurrentIndex(at < 0 ? 0 : at);
  };
  refresh();
  connect(workspaceCombo_, QOverload<int>::of(&QComboBox::activated), this, [this, refresh](int index) {
    QString data = workspaceCombo_->itemData(index).toString();
    if (data == "manage") {
      ManageWorkspacesDialog dlg(this);
      dlg.exec();
    } else if (data == "reset") {
      QString cur = appSettings().value("workspaces/current", tr("Default")).toString();
      if (cur == tr("Default")) resetUiLayout();
      else if (!loadWorkspace(cur)) resetUiLayout();
    } else if (data.startsWith("ws:")) {
      QString name = data.mid(3);
      if (name == "Default" && !workspaceNames().contains(tr("Default"))) resetUiLayout();
      else loadWorkspace(name);
      QSettings s = appSettings();
      s.setValue("workspaces/current", name == "Default" ? tr("Default") : name);
    }
    refresh();
  });
  connect(this, &MainWindow::workspacesChanged, this, refresh);
}

QStringList MainWindow::workspaceNames() const {
  QSettings s = appSettings();
  s.beginGroup("workspaceData");
  QStringList names = s.childGroups();
  s.endGroup();
  QStringList out;
  for (const QString& n : names) out << QString::fromUtf8(QByteArray::fromHex(n.toLatin1()));
  out.sort(Qt::CaseInsensitive);
  return out;
}

void MainWindow::saveWorkspace(const QString& name) {
  QSettings s = appSettings();
  QString key = "workspaceData/" + QString::fromLatin1(name.toUtf8().toHex());
  s.setValue(key + "/state", saveState());
  s.setValue(key + "/viewLayout", viewports_->viewLayout());
  s.setValue(key + "/menuBar", menuBar()->isVisible());
  s.setValue("workspaces/current", name);
  Q_EMIT workspacesChanged();
}

bool MainWindow::loadWorkspace(const QString& name) {
  QSettings s = appSettings();
  QString key = "workspaceData/" + QString::fromLatin1(name.toUtf8().toHex());
  if (!s.contains(key + "/state")) return false;
  restoreState(s.value(key + "/state").toByteArray());
  viewports_->setViewLayout(s.value(key + "/viewLayout", 9).toInt());
  s.setValue("workspaces/current", name);
  setPrompt(tr("Workspace \"%1\" loaded").arg(name));
  Q_EMIT workspacesChanged();
  return true;
}

void MainWindow::deleteWorkspace(const QString& name) {
  QSettings s = appSettings();
  s.remove("workspaceData/" + QString::fromLatin1(name.toUtf8().toHex()));
  if (s.value("workspaces/current").toString() == name) s.setValue("workspaces/current", tr("Default"));
  Q_EMIT workspacesChanged();
}

void MainWindow::resetUiLayout() {
  restoreState(startupState_);
  menuBar()->setVisible(true);
  statusBar()->setVisible(true);
  for (QToolBar* t : {mainToolBar_, axisToolBar_}) t->setVisible(true);
  commandDock_->setVisible(true);
  explorerDock_->setVisible(true);
  if (expertAct_ && expertAct_->isChecked()) expertAct_->setChecked(false);
  viewports_->setViewLayout(9);
  setPrompt(tr("Interface reset to the startup layout"));
}

void MainWindow::setUiLocked(bool locked) {
  for (QToolBar* t : findChildren<QToolBar*>()) t->setMovable(!locked);
  for (QDockWidget* d : findChildren<QDockWidget*>()) {
    QDockWidget::DockWidgetFeatures f = d->features();
    if (locked) f &= ~(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    else f |= QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    d->setFeatures(f);
  }
  if (lockUiAct_ && lockUiAct_->isChecked() != locked) {
    QSignalBlocker b(lockUiAct_);
    lockUiAct_->setChecked(locked);
  }
  QSettings s = appSettings();
  s.setValue("ui/locked", locked);
}

// ---------------------------------------------------------------------------------------------
// Hotkeys

QHash<QString, QString> MainWindow::hotkeys() const {
  QHash<QString, QString> out;
  for (auto it = commands_.begin(); it != commands_.end(); ++it) {
    QKeySequence k = overriddenKeys_.contains(it.key()) ? overriddenKeys_.value(it.key()) : it.value()->shortcut();
    out.insert(it.key(), k.toString(QKeySequence::PortableText));
  }
  return out;
}

void MainWindow::applyHotkeys(const QHash<QString, QString>& keys, bool persist) {
  // Restore any keys stepped aside for the shortcut override first.
  if (!overriddenKeys_.isEmpty()) {
    for (auto it = overriddenKeys_.begin(); it != overriddenKeys_.end(); ++it)
      if (QAction* a = commands_.value(it.key())) a->setShortcut(it.value());
    overriddenKeys_.clear();
  }
  for (auto it = keys.begin(); it != keys.end(); ++it)
    if (QAction* a = commands_.value(it.key())) {
      QKeySequence k(it.value(), QKeySequence::PortableText);
      if (it.key() == "render.render" && !k.isEmpty()) a->setShortcuts({k, QKeySequence("F9")});
      else a->setShortcut(k);
    }
  if (persist) {
    QSettings s = appSettings();
    s.beginGroup("hotkeys");
    s.remove("");
    for (auto it = commands_.begin(); it != commands_.end(); ++it) {
      QString cur = it.value()->shortcut().toString(QKeySequence::PortableText);
      if (cur != defaultShortcuts_.value(it.key())) s.setValue(it.key(), cur);
    }
    s.endGroup();
  }
  updateShortcutOverride();
}

}  // namespace veraa
