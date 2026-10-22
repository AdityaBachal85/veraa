// Status bar (two rows, like 3ds Max): script mini listener, selection count and prompt,
// isolate / selection lock / absolute-offset toggles, X/Y/Z transform type-in, grid size,
// adaptive degradation, a slot for the animation controls, and the viewport navigation
// controls.
#include <algorithm>
#include <QAction>
#include <QActionGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolButton>

#include "icons.h"
#include "icons_ui.h"
#include "mainwindow.h"
#include "navtools.h"
#include "paramform.h"
#include "scriptui.h"
#include "uistate.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

namespace {
QSettings appSettings() { return QSettings("Veraa", "Veraa"); }

QToolButton* toolButton(QWidget* parent, QAction* a) {
  auto* b = new QToolButton(parent);
  b->setDefaultAction(a);
  b->setAutoRaise(true);
  b->setIconSize(QSize(18, 18));
  return b;
}

QToolButton* flyoutButton(QWidget* parent, const QList<QAction*>& actions) {
  auto* b = new QToolButton(parent);
  auto* menu = new QMenu(b);
  for (QAction* a : actions) menu->addAction(a);
  b->setMenu(menu);
  b->setPopupMode(QToolButton::MenuButtonPopup);
  b->setDefaultAction(actions.front());
  b->setAutoRaise(true);
  b->setIconSize(QSize(18, 18));
  QObject::connect(menu, &QMenu::triggered, b, [b](QAction* a) { b->setDefaultAction(a); });
  return b;
}
}  // namespace

void MainWindow::createStatusBar() {
  QStatusBar* sb = statusBar();
  sb->setSizeGripEnabled(false);
  auto* bar = new QWidget(sb);
  bar->setObjectName("VeraaStatusBar");
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(2, 1, 2, 1);
  h->setSpacing(6);

  // 1. Script mini listener (two lines).
  miniListener_ = new ScriptListener(this, ScriptListener::Mini, bar);
  miniListener_->setFixedWidth(230);
  h->addWidget(miniListener_);

  // 2. Selection info, toggles, type-in, grid (row 0); prompt (row 1).
  auto* status = new QWidget(bar);
  auto* g = new QGridLayout(status);
  g->setContentsMargins(0, 0, 0, 0);
  g->setHorizontalSpacing(4);
  g->setVerticalSpacing(1);
  selInfo_ = new QLabel(tr("None Selected"), status);
  selInfo_->setMinimumWidth(130);
  g->addWidget(selInfo_, 0, 0);
  auto* toggles = new QHBoxLayout;
  toggles->setSpacing(1);
  QAction* isoToggle = command("tools.isolate");
  isoToggle->setIcon(icons::isolate());
  isoToggle->setToolTip(tr("Isolate Selection Toggle (Alt+Q)"));
  toggles->addWidget(toolButton(status, isoToggle));
  lockSelAct_->setToolTip(tr("Selection Lock Toggle (Space)"));
  toggles->addWidget(toolButton(status, lockSelAct_));
  QAction* absAct = cmd("edit.absoluteOffsetToggle", tr("Absolute Mode Transform Type-In"), QKeySequence(),
                        icons::absoluteMode(true));
  absAct->setCheckable(true);
  absAct->setChecked(!UiState::instance().absoluteTypeIn());
  connect(absAct, &QAction::toggled, this, [this, absAct](bool offset) {
    UiState::instance().setAbsoluteTypeIn(!offset);
    absAct->setIcon(icons::absoluteMode(!offset));
    absAct->setText(offset ? tr("Offset Mode Transform Type-In") : tr("Absolute Mode Transform Type-In"));
    absAct->setToolTip(absAct->text());
    updateTypeIn();
  });
  absOffsetBtn_ = toolButton(status, absAct);
  toggles->addWidget(absOffsetBtn_);
  g->addLayout(toggles, 0, 1);
  auto* typeIn = new QHBoxLayout;
  typeIn->setSpacing(3);
  const char* axes[3] = {"X:", "Y:", "Z:"};
  for (int i = 0; i < 3; ++i) {
    auto* l = new QLabel(axes[i], status);
    typeIn->addWidget(l);
    typeIn_[i] = new UnitSpinBox(status);
    typeIn_[i]->setObjectName(QString("TypeIn%1").arg(QChar('X' + i)));
    typeIn_[i]->setRange(-1e9, 1e9);
    typeIn_[i]->setDecimals(3);
    typeIn_[i]->setKeyboardTracking(false);
    typeIn_[i]->setFixedWidth(118);
    typeIn_[i]->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    installSpinnerScrub(typeIn_[i]);
    connect(typeIn_[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, i](double v) {
      if (!updatingTypeIn_) applyTypeIn(i, v);
    });
    typeIn->addWidget(typeIn_[i]);
  }
  g->addLayout(typeIn, 0, 2);
  gridInfo_ = new QLabel(status);
  gridInfo_->setMinimumWidth(100);
  g->addWidget(gridInfo_, 0, 3);
  prompt_ = new QLabel(tr("Click or click-and-drag to select objects"), status);
  prompt_->setMinimumWidth(200);
  g->addWidget(prompt_, 1, 0, 1, 3);
  g->setColumnStretch(4, 1);
  QAction* adaptive = cmd("views.adaptiveDegradation", tr("Adaptive Degradation"), QKeySequence("O"), icons::adaptive());
  adaptive->setCheckable(true);
  adaptive->setToolTip(tr("Adaptive Degradation (O): heavy objects draw as boxes while navigating"));
  connect(adaptive, &QAction::toggled, this, [](bool on) { UiState::instance().setAdaptiveDegradation(on); });
  auto* adaptiveBtn = toolButton(status, adaptive);
  g->addWidget(adaptiveBtn, 1, 3, Qt::AlignLeft);
  h->addWidget(status, 1);

  // 3. Animation controls (time controls, key mode, playback) from the animation module.
  auto* anim = new QWidget(bar);
  anim->setObjectName("AnimationControlsSlot");
  animSlot_ = new QHBoxLayout(anim);
  animSlot_->setContentsMargins(0, 0, 0, 0);
  animSlot_->setSpacing(2);
  h->addWidget(anim);

  // 4. Viewport navigation controls.
  auto* nav = new QWidget(bar);
  nav->setObjectName("NavigationControls");
  auto* ng = new QGridLayout(nav);
  ng->setContentsMargins(0, 0, 0, 0);
  ng->setSpacing(0);
  createNavigationControls(ng);
  h->addWidget(nav);

  sb->addPermanentWidget(bar, 1);

  auto refreshSel = [this] {
    int n = doc_->selection().size();
    QString t = n == 0 ? tr("None Selected") : n == 1 ? tr("1 Object Selected") : tr("%1 Objects Selected").arg(n);
    if (UiState::instance().selectionLocked()) t += tr(" (Locked)");
    selInfo_->setText(t);
  };
  connect(doc_, &SceneDocument::selectionChanged, this, refreshSel);
  connect(&UiState::instance(), &UiState::changed, this, refreshSel);
  connect(state_, &EditorState::toolChanged, this, [this] { updateTypeIn(); });
}

QString MainWindow::activeNavigationTool() const {
  if (!navGroup_) return QString();
  QAction* a = navGroup_->checkedAction();
  return a ? a->property("commandId").toString() : QString();
}

void MainWindow::createNavigationControls(QGridLayout* grid) {
  QWidget* parent = grid->parentWidget();
  navGroup_ = new QActionGroup(this);
  navGroup_->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
  auto navAct = [&](const QString& id, const QString& text, const QKeySequence& key, const QIcon& icon, NavMode mode) {
    QAction* a = cmd(id, text, key, icon);
    a->setCheckable(true);
    a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key.toString(QKeySequence::NativeText)));
    navGroup_->addAction(a);
    connect(a, &QAction::triggered, this, [this, a, mode](bool on) {
      if (on) {
        state_->setTool(TransformTool::Select);
        state_->setViewportTool(makeNavTool(mode, viewports_));
        setPrompt(navToolPrompt(mode));
      } else if (isNavTool(state_->viewportTool())) {
        state_->setViewportTool(nullptr);
      }
      Q_UNUSED(a);
    });
    return a;
  };
  QAction* zoom = navAct("views.zoom", tr("Zoom"), QKeySequence("Alt+Z"), icons::zoom(), NavMode::Zoom);
  QAction* zoomAll = navAct("views.zoomAll", tr("Zoom All"), QKeySequence(), icons::zoomAll(), NavMode::ZoomAll);
  QAction* fov = navAct("views.fieldOfView", tr("Field-of-View"), QKeySequence(), icons::fieldOfView(), NavMode::Fov);
  QAction* region = navAct("views.zoomRegion", tr("Zoom Region"), QKeySequence("Ctrl+W"), icons::zoomRegion(),
                           NavMode::ZoomRegion);
  QAction* pan = navAct("views.pan", tr("Pan View"), QKeySequence("Ctrl+P"), icons::pan(), NavMode::Pan);
  QAction* orbit = navAct("views.orbit", tr("Orbit"), QKeySequence("Ctrl+R"), icons::orbit(0), NavMode::Orbit);
  QAction* orbitSel = navAct("views.orbitSelected", tr("Orbit Selected"), QKeySequence(), icons::orbit(1),
                             NavMode::OrbitSelected);
  QAction* orbitSub = navAct("views.orbitSubObject", tr("Orbit SubObject"), QKeySequence(), icons::orbit(2),
                             NavMode::OrbitSubObject);
  // Clear the checked tool when another viewport tool takes over (or Esc ends it).
  connect(state_, &EditorState::viewportToolChanged, this, [this] {
    if (isNavTool(state_->viewportTool())) return;
    if (QAction* a = navGroup_->checkedAction()) {
      QSignalBlocker b(a);
      a->setChecked(false);
    }
  });

  QAction* zoomExt = cmd("views.zoomExtents", tr("Zoom Extents"), QKeySequence("Alt+Ctrl+Z"), icons::zoomExtents());
  connect(zoomExt, &QAction::triggered, this, [this] { viewports_->active()->zoomExtents(false); });
  QAction* zoomExtAllSel = cmd("views.zoomExtentsAllSelected", tr("Zoom Extents All Selected"), QKeySequence(),
                               icons::zoomExtentsAll());
  connect(zoomExtAllSel, &QAction::triggered, this, [this] { viewports_->zoomExtentsAll(true); });

  grid->addWidget(toolButton(parent, zoom), 0, 0);
  grid->addWidget(toolButton(parent, zoomAll), 0, 1);
  grid->addWidget(flyoutButton(parent, {zoomExt, zoomExtAct_}), 0, 2);
  grid->addWidget(flyoutButton(parent, {zoomExtAllAct_, zoomExtAllSel}), 0, 3);
  grid->addWidget(flyoutButton(parent, {fov, region}), 1, 0);
  grid->addWidget(toolButton(parent, pan), 1, 1);
  grid->addWidget(flyoutButton(parent, {orbit, orbitSel, orbitSub}), 1, 2);
  grid->addWidget(toolButton(parent, maximizeAct_), 1, 3);
}

void MainWindow::applyGridLabel() {
  double spacing = appSettings().value("grid/spacing", 10.0).toDouble();
  gridInfo_->setText(tr("Grid = %1").arg(Units::instance().format(spacing, 3)));
}

void MainWindow::updateTypeIn() {
  if (!typeIn_[0]) return;
  updatingTypeIn_ = true;
  auto nodes = doc_->selectedNodes();
  TransformTool tool = state_->tool();
  bool abs = UiState::instance().absoluteTypeIn();
  bool length = tool == TransformTool::Move || tool == TransformTool::Select;
  for (int i = 0; i < 3; ++i) {
    UnitSpinBox* sb = typeIn_[i];
    QSignalBlocker b(sb);
    sb->setLengthMode(length);
    sb->setSuffix(tool == TransformTool::Rotate ? QString::fromUtf8("°") : tool == TransformTool::Scale ? "%" : "");
    sb->setSingleStep(length ? 1.0 : tool == TransformTool::Rotate ? 1.0 : 1.0);
    sb->setEnabled(!nodes.isEmpty());
    double v = 0.0;
    if (!nodes.isEmpty()) {
      Node* n = nodes[0];
      if (!abs) v = tool == TransformTool::Scale ? 100.0 : 0.0;
      else if (length) v = n->worldPosition()[i];
      else if (tool == TransformTool::Rotate) v = n->transform.rotation[i];
      else v = n->transform.scale[i] * 100.0;
    }
    sb->setValue(v);
  }
  if (typeInLabel_) typeInLabel_->setText(abs ? tr("Absolute") : tr("Offset"));
  updatingTypeIn_ = false;
}

void MainWindow::applyTypeIn(int axis, double value) {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  TransformTool tool = state_->tool();
  bool abs = UiState::instance().absoluteTypeIn();
  doc_->beginEdit(tr("Transform Type-In"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    if (tool == TransformTool::Move || tool == TransformTool::Select) {
      Vec3 p = n->worldPosition();
      if (abs) p[axis] = static_cast<float>(value);
      else p[axis] += static_cast<float>(value);
      n->setWorldPosition(p);
    } else if (tool == TransformTool::Rotate) {
      if (abs) {
        n->transform.rotation[axis] = static_cast<float>(value);
      } else {
        Vec3 worldAxis(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
        Vec3 localAxis = normalize(n->parentWorldMatrix().inverse().transformDir(worldAxis));
        Mat4 r = Mat4::rotateAxis(localAxis, radians(static_cast<float>(value)));
        n->transform.rotation = eulerDegFromMatrix(r * Mat4::fromEulerDeg(n->transform.rotation));
      }
    } else {
      float f = static_cast<float>(value) / 100.0f;
      if (abs) n->transform.scale[axis] = f;
      else n->transform.scale[axis] *= f;
    }
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform);
  doc_->endEdit();
  // Offset mode fields return to zero (100% for scale) after applying.
  if (!abs) updateTypeIn();
}

}  // namespace veraa
