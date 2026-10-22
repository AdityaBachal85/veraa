// Motion command panel tab: controller assignment, PRS keys, Key Info, constraint parameters
// and trajectories.
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

#include "animcontroller.h"
#include "animicons.h"
#include "animui.h"
#include "commandpanel.h"
#include "extension.h"
#include "mainwindow.h"
#include "paramform.h"

namespace veraa {
namespace {

const char* const kTangentNames[] = {"Auto", "Smooth", "Linear", "Step", "Fast", "Slow", "Custom"};

QDoubleSpinBox* spin(QWidget* parent, double lo, double hi, int decimals = 2, double step = 1.0) {
  auto* s = new QDoubleSpinBox(parent);
  s->setRange(lo, hi);
  s->setDecimals(decimals);
  s->setSingleStep(step);
  s->setKeyboardTracking(false);
  s->setMinimumWidth(40);
  return s;
}

class MotionPanel : public QScrollArea {
 public:
  explicit MotionPanel(AnimController* c) : c_(c) {
    setObjectName("motionPanel");
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget;
    setWidget(content);
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);

    auto* modeRow = new QHBoxLayout;
    paramsBtn_ = new QPushButton(tr("Parameters"), content);
    trajBtn_ = new QPushButton(tr("Trajectories"), content);
    paramsBtn_->setCheckable(true);
    trajBtn_->setCheckable(true);
    paramsBtn_->setMinimumWidth(10);
    trajBtn_->setMinimumWidth(10);
    paramsBtn_->setChecked(true);
    auto* grp = new QButtonGroup(content);
    grp->addButton(paramsBtn_, 0);
    grp->addButton(trajBtn_, 1);
    modeRow->addWidget(paramsBtn_);
    modeRow->addWidget(trajBtn_);
    v->addLayout(modeRow);
    nodeLabel_ = new QLabel(content);
    nodeLabel_->setStyleSheet("font-weight: bold;");
    v->addWidget(nodeLabel_);
    pages_ = new QStackedWidget(content);
    v->addWidget(pages_);
    v->addStretch(1);
    pages_->addWidget(buildParameters());
    pages_->addWidget(buildTrajectories());
    connect(grp, &QButtonGroup::idClicked, this, [this](int id) {
      pages_->setCurrentIndex(id);
      setShowSelectionTrajectories(id == 1 && showTraj_->isChecked());
      c_->window().editorState()->requestRedraw();
    });

    connect(c->document(), &SceneDocument::selectionChanged, this, [this] { rebuild(); });
    connect(c->document(), &SceneDocument::structureChanged, this, [this] { rebuild(); });
    connect(c, &AnimController::animationChanged, this, [this] {
      if (!editing_) rebuild();
    });
    connect(c, &AnimController::frameChanged, this, [this] { refreshKeyInfo(); });
    rebuild();
  }

 private:
  // ---- Parameters page ----
  QWidget* buildParameters() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);

    auto* assign = new Rollout(tr("Assign Controller"), page);
    auto* av = new QVBoxLayout;
    tree_ = new QTreeWidget(assign->body());
    tree_->setObjectName("motionControllerTree");
    tree_->setHeaderHidden(true);
    tree_->setMinimumHeight(170);
    tree_->setRootIsDecorated(true);
    tree_->setMinimumWidth(10);
    auto* btnRow = new QHBoxLayout;
    auto* assignBtn = new QPushButton(tr("Assign Controller..."), assign->body());
    auto* propsBtn = new QPushButton(tr("Properties..."), assign->body());
    assignBtn->setMinimumWidth(10);
    propsBtn->setMinimumWidth(10);
    btnRow->addWidget(assignBtn);
    btnRow->addWidget(propsBtn);
    av->addWidget(tree_);
    av->addLayout(btnRow);
    assign->form()->addRow(av);
    v->addWidget(assign);
    connect(assignBtn, &QPushButton::clicked, this, [this] { assignController(); });
    connect(propsBtn, &QPushButton::clicked, this, [this] {
      if (!currentPath_.empty()) editControllerProperties(c_, currentPath_, this);
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
      currentPath_ = it ? it->data(0, Qt::UserRole).toString().toStdString() : std::string();
      if (currentPath_.rfind("node:", 0) != 0) currentPath_.clear();
      keyIndex_ = 0;
      refreshKeyInfo();
    });

    auto* prs = new Rollout(tr("PRS Parameters"), page);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(2);
    grid->addWidget(new QLabel(tr("Create Key:"), prs->body()), 0, 0, 1, 3);
    grid->addWidget(new QLabel(tr("Delete Key:"), prs->body()), 2, 0, 1, 3);
    const char* names[] = {"Position", "Rotation", "Scale"};
    for (int i = 0; i < 3; ++i) {
      auto* cb = new QPushButton(tr(names[i]), prs->body());
      auto* db = new QPushButton(tr(names[i]), prs->body());
      cb->setObjectName(QStringLiteral("prsCreate%1").arg(names[i]));
      for (QPushButton* b : {cb, db}) b->setMinimumWidth(10);
      grid->addWidget(cb, 1, i);
      grid->addWidget(db, 3, i);
      connect(cb, &QPushButton::clicked, this, [this, i] { prsKey(i, true); });
      connect(db, &QPushButton::clicked, this, [this, i] { prsKey(i, false); });
    }
    prs->form()->addRow(grid);
    v->addWidget(prs);

    auto* info = new Rollout(tr("Key Info (Basic)"), page);
    trackLabel_ = new QLabel(info->body());
    trackLabel_->setWordWrap(true);
    trackLabel_->setMinimumWidth(40);
    info->form()->addRow(trackLabel_);
    auto* keyRow = new QHBoxLayout;
    auto* prevK = new QToolButton(info->body());
    prevK->setIcon(animicons::prevFrame());
    auto* nextK = new QToolButton(info->body());
    nextK->setIcon(animicons::nextFrame());
    keyNum_ = new QLabel(info->body());
    keyRow->addWidget(prevK);
    keyRow->addWidget(keyNum_, 1);
    keyRow->addWidget(nextK);
    info->form()->addRow(tr("Key:"), keyRow);
    time_ = spin(info->body(), -1e6, 1e6, 0);
    value_ = spin(info->body(), -1e9, 1e9, 3, 0.1);
    inType_ = new QComboBox(info->body());
    outType_ = new QComboBox(info->body());
    inType_->setMinimumWidth(40);
    outType_->setMinimumWidth(40);
    for (const char* n : kTangentNames) {
      inType_->addItem(animicons::tangent(inType_->count()), tr(n));
      outType_->addItem(animicons::tangent(outType_->count()), tr(n));
    }
    info->form()->addRow(tr("Time:"), time_);
    info->form()->addRow(tr("Value:"), value_);
    info->form()->addRow(tr("In:"), inType_);
    info->form()->addRow(tr("Out:"), outType_);
    v->addWidget(info);
    connect(prevK, &QToolButton::clicked, this, [this] { stepKey(-1); });
    connect(nextK, &QToolButton::clicked, this, [this] { stepKey(1); });

    auto* adv = new Rollout(tr("Key Info (Advanced)"), page);
    inSlope_ = spin(adv->body(), -1e6, 1e6, 3, 0.1);
    outSlope_ = spin(adv->body(), -1e6, 1e6, 3, 0.1);
    tension_ = spin(adv->body(), -1, 1, 2, 0.05);
    continuity_ = spin(adv->body(), -1, 1, 2, 0.05);
    bias_ = spin(adv->body(), -1, 1, 2, 0.05);
    easeTo_ = spin(adv->body(), 0, 50, 1, 1);
    easeFrom_ = spin(adv->body(), 0, 50, 1, 1);
    adv->form()->addRow(tr("In Slope:"), inSlope_);
    adv->form()->addRow(tr("Out Slope:"), outSlope_);
    adv->form()->addRow(tr("Tension:"), tension_);
    adv->form()->addRow(tr("Continuity:"), continuity_);
    adv->form()->addRow(tr("Bias:"), bias_);
    adv->form()->addRow(tr("Ease To:"), easeTo_);
    adv->form()->addRow(tr("Ease From:"), easeFrom_);
    adv->setExpanded(false);
    v->addWidget(adv);

    for (QDoubleSpinBox* s : {time_, value_, inSlope_, outSlope_, tension_, continuity_, bias_, easeTo_, easeFrom_})
      connect(s, &QDoubleSpinBox::valueChanged, this, [this] { applyKeyInfo(); });
    for (QComboBox* cb : {inType_, outType_})
      connect(cb, &QComboBox::currentIndexChanged, this, [this] { applyKeyInfo(); });

    constraintsHost_ = new QWidget(page);
    constraintsLayout_ = new QVBoxLayout(constraintsHost_);
    constraintsLayout_->setContentsMargins(0, 0, 0, 0);
    v->addWidget(constraintsHost_);
    return page;
  }

  // ---- Trajectories page ----
  QWidget* buildTrajectories() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    auto* r = new Rollout(tr("Trajectories"), page);
    showTraj_ = new QCheckBox(tr("Show Trajectory of Selection"), r->body());
    showTraj_->setChecked(true);
    nodeTraj_ = new QCheckBox(tr("Always Display Trajectory"), r->body());
    r->form()->addRow(showTraj_);
    r->form()->addRow(nodeTraj_);
    auto* deleteKey = new QPushButton(tr("Delete Key"), r->body());
    auto* addKey = new QPushButton(tr("Add Key"), r->body());
    auto* row = new QHBoxLayout;
    row->addWidget(deleteKey);
    row->addWidget(addKey);
    r->form()->addRow(row);
    sampleStart_ = new QSpinBox(r->body());
    sampleEnd_ = new QSpinBox(r->body());
    samples_ = new QSpinBox(r->body());
    for (QSpinBox* s : {sampleStart_, sampleEnd_}) s->setRange(-100000, 100000);
    samples_->setRange(2, 10000);
    samples_->setValue(10);
    sampleStart_->setValue(c_->startFrame());
    sampleEnd_->setValue(c_->endFrame());
    r->form()->addRow(tr("Start Time:"), sampleStart_);
    r->form()->addRow(tr("End Time:"), sampleEnd_);
    r->form()->addRow(tr("Samples:"), samples_);
    auto* toSpline = new QPushButton(tr("Convert To"), r->body());
    auto* fromSpline = new QPushButton(tr("Convert From"), r->body());
    auto* convRow = new QHBoxLayout;
    convRow->addWidget(toSpline);
    convRow->addWidget(fromSpline);
    r->form()->addRow(new QLabel(tr("Spline Conversion:"), r->body()));
    r->form()->addRow(convRow);
    auto* collapse = new QPushButton(tr("Collapse Transform"), r->body());
    collapse->setToolTip(tr("Bakes constraints and IK into position, rotation and scale keys"));
    r->form()->addRow(collapse);
    for (QPushButton* b : {toSpline, fromSpline, collapse, deleteKey, addKey}) b->setMinimumWidth(10);
    v->addWidget(r);

    connect(showTraj_, &QCheckBox::toggled, this, [this](bool on) {
      setShowSelectionTrajectories(on && pages_->currentIndex() == 1);
      c_->window().editorState()->requestRedraw();
    });
    connect(nodeTraj_, &QCheckBox::toggled, this, [this](bool on) {
      Node* n = c_->document()->firstSelected();
      Animation* anim = c_->animation();
      if (!n || !anim || anim->nodes[n->id].trajectory == on) return;
      uint32_t id = n->id;
      c_->editAnimation(tr("Trajectory Display"), [&] { anim->nodes[id].trajectory = on; });
    });
    connect(deleteKey, &QPushButton::clicked, this, [this] {
      for (Node* n : c_->document()->selectedNodes()) {
        uint32_t id = n->id;
        c_->editAnimation(tr("Delete Key"), [&] { c_->animation()->deleteNodeKeysAt(id, c_->frame()); });
      }
    });
    connect(addKey, &QPushButton::clicked, this, [this] { prsKey(0, true); });
    connect(toSpline, &QPushButton::clicked, this, [this] { convertToSpline(); });
    connect(fromSpline, &QPushButton::clicked, this, [this] {
      Node* n = c_->document()->firstSelected();
      if (!n) return;
      uint32_t id = n->id;
      startPickNode(c_->window(), tr("Convert From: pick a spline"), [](const Node& s) { return s.kind() == ObjectKind::Shape; },
                    [this, id](uint32_t shape) { convertFromSpline(id, shape); });
    });
    connect(collapse, &QPushButton::clicked, this, [this] { collapseTransforms(); });
    return page;
  }

  // ---- Rebuild ----
  void rebuild() {
    Node* n = c_->document()->firstSelected();
    nodeId_ = n ? n->id : 0;
    nodeLabel_->setText(n ? QString::fromStdString(n->name) : tr("Nothing selected"));
    std::string keep = currentPath_;
    {
      QSignalBlocker b(tree_);
      tree_->clear();
    }
    // Constraint rollouts.
    while (QLayoutItem* it = constraintsLayout_->takeAt(0)) {
      if (it->widget()) it->widget()->deleteLater();
      delete it;
    }
    Animation* anim = c_->animation();
    if (!n || !anim) {
      currentPath_.clear();
      refreshKeyInfo();
      return;
    }
    {
      QSignalBlocker b(nodeTraj_);
      auto na = anim->nodes.find(n->id);
      nodeTraj_->setChecked(na != anim->nodes.end() && na->second.trajectory);
    }
    QSignalBlocker blocker(tree_);
    std::string base = "node:" + std::to_string(n->id) + "/";
    auto* transform = new QTreeWidgetItem(tree_, {tr("Transform : Position/Rotation/Scale")});
    transform->setData(0, Qt::UserRole, "transform");
    auto na = anim->nodes.find(n->id);
    const NodeAnimation* nodeAnim = na != anim->nodes.end() ? &na->second : nullptr;
    bool tcbRot = nodeAnim && nodeAnim->rotationController == 1;
    const char* groups[] = {"Position : Position XYZ", "Rotation : Euler XYZ", "Scale : Bezier Scale"};
    const char* groupKeys[] = {"group:pos", "group:rot", "group:scale"};
    const char* chNames[] = {"X Position", "Y Position", "Z Position", "X Rotation", "Y Rotation", "Z Rotation",
                             "X Scale",    "Y Scale",    "Z Scale"};
    QTreeWidgetItem* select = nullptr;
    for (int g = 0; g < 3; ++g) {
      QString title = tr(groups[g]);
      if (g == 1 && tcbRot) title = tr("Rotation : TCB Rotation");
      auto* gi = new QTreeWidgetItem(transform, {title});
      gi->setData(0, Qt::UserRole, groupKeys[g]);
      if (g == 1 && tcbRot) continue;
      for (int k = 0; k < 3; ++k) {
        std::string path = base + Animation::transformChannels()[static_cast<size_t>(g * 3 + k)];
        const Track* t = anim->findTrack(path);
        QString label = QStringLiteral("%1 : %2").arg(tr(chNames[g * 3 + k]), controllerName(t));
        if (anim->isAnimated(path)) label += QStringLiteral("  ●");
        auto* it = new QTreeWidgetItem(gi, {label});
        it->setData(0, Qt::UserRole, QString::fromStdString(path));
        if (path == keep) select = it;
      }
    }
    if (nodeAnim)
      for (size_t i = 0; i < nodeAnim->constraints.size(); ++i) {
        auto* it = new QTreeWidgetItem(transform, {constraintTitle(nodeAnim->constraints[i].type)});
        it->setData(0, Qt::UserRole, QStringLiteral("constraint:%1").arg(i));
        it->setForeground(0, QColor(0xf0, 0xc0, 0x60));
        constraintsLayout_->addWidget(buildConstraintRollout(n->id, static_cast<int>(i)));
      }
    transform->setExpanded(true);
    for (int i = 0; i < transform->childCount(); ++i) transform->child(i)->setExpanded(true);
    // Object and modifier parameters.
    auto addParams = [&](QTreeWidgetItem* parent, const std::string& prefix, const ParamBlock& p) {
      for (const auto& d : p.defs()) {
        if (d.type == ParamType::String || d.type == ParamType::File) continue;
        std::vector<std::string> comps;
        if (d.type == ParamType::Color) comps = {d.key + ".r", d.key + ".g", d.key + ".b"};
        else comps = {d.key};
        for (const auto& key : comps) {
          std::string path = prefix + key;
          const Track* t = anim->findTrack(path);
          QString label = QStringLiteral("%1 : %2").arg(QString::fromStdString(d.label) +
                                                            (key.size() > d.key.size() ? QString::fromStdString(key.substr(d.key.size())) : QString()),
                                                        controllerName(t));
          if (anim->isAnimated(path)) label += QStringLiteral("  ●");
          auto* it = new QTreeWidgetItem(parent, {label});
          it->setData(0, Qt::UserRole, QString::fromStdString(path));
          if (path == keep) select = it;
        }
      }
    };
    if (n->object) {
      auto* obj = new QTreeWidgetItem(tree_, {tr("Object (%1)").arg(QString::fromStdString(objectDisplayName(n->object->type())))});
      addParams(obj, base + "object/", n->object->params);
    }
    for (size_t i = 0; i < n->modifiers.size(); ++i) {
      if (!n->modifiers[i]) continue;
      auto* mi = new QTreeWidgetItem(tree_, {tr("Modifier: %1").arg(QString::fromStdString(modifierDisplayName(n->modifiers[i]->type())))});
      addParams(mi, base + "mod:" + std::to_string(i) + "/", n->modifiers[i]->params);
    }
    if (nodeAnim && !nodeAnim->custom.empty()) {
      auto* ci = new QTreeWidgetItem(tree_, {tr("Custom Attributes")});
      for (const auto& ca : nodeAnim->custom) {
        std::string path = base + "custom/" + ca.name;
        auto* it = new QTreeWidgetItem(ci, {QStringLiteral("%1 : %2").arg(QString::fromStdString(ca.name), controllerName(anim->findTrack(path)))});
        it->setData(0, Qt::UserRole, QString::fromStdString(path));
        if (path == keep) select = it;
      }
      ci->setExpanded(true);
    }
    if (!select) select = transform->child(0) ? transform->child(0)->child(0) : nullptr;
    if (select) tree_->setCurrentItem(select);
    currentPath_ = select ? select->data(0, Qt::UserRole).toString().toStdString() : std::string();
    if (currentPath_.rfind("node:", 0) != 0) currentPath_.clear();
    blocker.unblock();
    refreshKeyInfo();
  }

  // ---- Controllers ----
  void assignController() {
    QTreeWidgetItem* it = tree_->currentItem();
    if (!it || !nodeId_) return;
    QString key = it->data(0, Qt::UserRole).toString();
    QStringList items;
    std::vector<std::string> paths;
    std::string base = "node:" + std::to_string(nodeId_) + "/";
    auto groupPaths = [&](int g) {
      for (int k = 0; k < 3; ++k) paths.push_back(base + Animation::transformChannels()[static_cast<size_t>(g * 3 + k)]);
    };
    enum Kind { Float, Pos, Rot, Scale, Transform } kind = Float;
    if (key == "group:pos") {
      kind = Pos;
      groupPaths(0);
    } else if (key == "group:rot") {
      kind = Rot;
      groupPaths(1);
    } else if (key == "group:scale") {
      kind = Scale;
      groupPaths(2);
    } else if (key == "transform") {
      kind = Transform;
    } else if (key.startsWith("node:")) {
      paths.push_back(key.toStdString());
    } else {
      return;
    }
    const QStringList floats = {tr("Bezier Float"), tr("Linear Float"), tr("TCB Float"), tr("Noise Float"),
                                tr("Float Expression"), tr("Float List")};
    switch (kind) {
      case Float: items = floats; break;
      case Pos:
        items = QStringList{tr("Position XYZ (Bezier)"), tr("Linear Position"), tr("TCB Position"), tr("Noise Position"),
                            tr("Position Expression"), tr("Position List"), tr("Path Constraint"), tr("Position Constraint"),
                            tr("Attachment Constraint"), tr("Surface Constraint")};
        break;
      case Rot:
        items = QStringList{tr("Euler XYZ"), tr("TCB Rotation"), tr("Noise Rotation"), tr("Orientation Constraint"),
                            tr("Look-At Constraint")};
        break;
      case Scale: items = QStringList{tr("Bezier Scale"), tr("Linear Scale"), tr("TCB Scale"), tr("Noise Scale"), tr("Scale List")}; break;
      case Transform: items = QStringList{tr("Position/Rotation/Scale (remove constraints)"), tr("Link Constraint")}; break;
    }
    QDialog d(this);
    d.setWindowTitle(tr("Assign Controller"));
    auto* v = new QVBoxLayout(&d);
    auto* list = new QListWidget(&d);
    list->addItems(items);
    list->setCurrentRow(0);
    v->addWidget(list);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &d, &QDialog::accept);
    if (d.exec() != QDialog::Accepted) return;
    int choice = list->currentRow();
    uint32_t id = nodeId_;
    const ControllerType floatTypes[] = {ControllerType::Bezier, ControllerType::Linear, ControllerType::TCB,
                                         ControllerType::Noise,  ControllerType::Expression, ControllerType::List};
    MainWindow& w = c_->window();
    auto pickConstraint = [&](ConstraintType t) {
      c_->document()->setSelection({id});
      beginConstraintPick(w, t);
    };
    switch (kind) {
      case Float: assignFloatController(c_, paths, floatTypes[choice]); break;
      case Pos:
        if (choice < 6) assignFloatController(c_, paths, floatTypes[choice]);
        else pickConstraint(choice == 6 ? ConstraintType::Path : choice == 7 ? ConstraintType::Position
                                                                : choice == 8 ? ConstraintType::Attachment
                                                                              : ConstraintType::Surface);
        break;
      case Rot:
        if (choice == 0) setRotationController(c_, id, 0);
        else if (choice == 1) setRotationController(c_, id, 1);
        else if (choice == 2) {
          setRotationController(c_, id, 0);
          assignFloatController(c_, paths, ControllerType::Noise);
        } else pickConstraint(choice == 3 ? ConstraintType::Orientation : ConstraintType::LookAt);
        break;
      case Scale: {
        const ControllerType st[] = {ControllerType::Bezier, ControllerType::Linear, ControllerType::TCB, ControllerType::Noise,
                                     ControllerType::List};
        assignFloatController(c_, paths, st[choice]);
        break;
      }
      case Transform:
        if (choice == 0) {
          c_->editAnimation(tr("Remove Constraints"), [&] { c_->animation()->nodes[id].constraints.clear(); });
        } else {
          c_->document()->setSelection({id});
          beginConstraintPick(w, ConstraintType::Link);
        }
        break;
    }
  }

  void prsKey(int group, bool create) {
    Animation* anim = c_->animation();
    if (!anim) return;
    QVector<Node*> nodes = c_->document()->selectedNodes();
    if (nodes.isEmpty()) return;
    c_->editAnimation(create ? tr("Create Key") : tr("Delete Key"), [&] {
      Scene& s = c_->scene();
      double f = c_->frame();
      for (Node* n : nodes) {
        auto na = anim->nodes.find(n->id);
        if (group == 1 && na != anim->nodes.end() && na->second.rotationController == 1) {
          if (create) anim->setRotationKey(n->id, f, n->transform.rotation);
          else {
            auto& rk = na->second.rotationKeys;
            rk.erase(std::remove_if(rk.begin(), rk.end(), [f](const RotationKey& k) { return std::fabs(k.time - f) < 1e-3; }),
                     rk.end());
          }
          continue;
        }
        for (int k = 0; k < 3; ++k) {
          std::string path = Animation::nodePath(n->id, Animation::transformChannels()[static_cast<size_t>(group * 3 + k)]);
          if (create) {
            double v;
            if (Animation::readValue(s, path, v)) anim->setKey(s, path, f, v, anim->isAnimated(path) ? c_->startFrame() : f);
          } else {
            anim->deleteKey(path, f);
          }
        }
      }
    });
  }

  // ---- Key info ----
  Track* currentTrack() const {
    Animation* anim = c_->animation();
    if (!anim || currentPath_.empty()) return nullptr;
    if (anim->activeLayer > 0 && anim->activeLayer <= static_cast<int>(anim->layers.size())) {
      auto& L = anim->layers[static_cast<size_t>(anim->activeLayer - 1)].tracks;
      auto it = L.find(currentPath_);
      if (it != L.end()) return &it->second;
    }
    return anim->findTrack(currentPath_);
  }

  void refreshKeyInfo() {
    Track* t = currentTrack();
    std::vector<QWidget*> widgets = {time_, value_, inType_, outType_, inSlope_, outSlope_, tension_, continuity_, bias_, easeTo_, easeFrom_};
    trackLabel_->setText(currentPath_.empty() ? tr("Select a track above.")
                                              : QString::fromStdString(Animation::displayName(c_->scene(), currentPath_)));
    bool has = t && !t->keys.empty();
    for (QWidget* w : widgets) w->setEnabled(has);
    if (!has) {
      keyNum_->setText(tr("No keys"));
      return;
    }
    // Prefer the key at the current frame.
    int at = t->keyIndexAt(c_->frame());
    if (at >= 0) keyIndex_ = at;
    keyIndex_ = std::clamp(keyIndex_, 0, static_cast<int>(t->keys.size()) - 1);
    const Key& k = t->keys[static_cast<size_t>(keyIndex_)];
    loading_ = true;
    keyNum_->setText(tr("%1 of %2").arg(keyIndex_ + 1).arg(t->keys.size()));
    time_->setValue(k.time);
    value_->setValue(k.value);
    inType_->setCurrentIndex(static_cast<int>(k.inType));
    outType_->setCurrentIndex(static_cast<int>(k.outType));
    double in, out;
    t->slopes(static_cast<size_t>(keyIndex_), in, out);
    inSlope_->setValue(in);
    outSlope_->setValue(out);
    tension_->setValue(k.tension);
    continuity_->setValue(k.continuity);
    bias_->setValue(k.bias);
    easeTo_->setValue(k.easeTo);
    easeFrom_->setValue(k.easeFrom);
    loading_ = false;
  }

  void stepKey(int d) {
    Track* t = currentTrack();
    if (!t || t->keys.empty()) return;
    keyIndex_ = (keyIndex_ + d + static_cast<int>(t->keys.size())) % static_cast<int>(t->keys.size());
    c_->setFrame(t->keys[static_cast<size_t>(keyIndex_)].time);
    refreshKeyInfo();
  }

  void applyKeyInfo() {
    if (loading_) return;
    Track* t = currentTrack();
    if (!t || keyIndex_ < 0 || keyIndex_ >= static_cast<int>(t->keys.size())) return;
    std::string path = currentPath_;
    int idx = keyIndex_;
    Key nk = t->keys[static_cast<size_t>(idx)];
    nk.time = time_->value();
    nk.value = value_->value();
    nk.inType = static_cast<TangentType>(inType_->currentIndex());
    nk.outType = static_cast<TangentType>(outType_->currentIndex());
    double in, out;
    t->slopes(static_cast<size_t>(idx), in, out);
    // Editing a slope makes that side Custom.
    if (std::fabs(inSlope_->value() - in) > 1e-6) nk.inType = TangentType::Custom;
    if (std::fabs(outSlope_->value() - out) > 1e-6) nk.outType = TangentType::Custom;
    nk.inSlope = nk.inType == TangentType::Custom ? inSlope_->value() : in;
    nk.outSlope = nk.outType == TangentType::Custom ? outSlope_->value() : out;
    nk.tension = tension_->value();
    nk.continuity = continuity_->value();
    nk.bias = bias_->value();
    nk.easeTo = easeTo_->value();
    nk.easeFrom = easeFrom_->value();
    editing_ = true;
    c_->editAnimation(tr("Edit Key"), [&] {
      Track* tr2 = currentTrack();
      if (!tr2 || idx >= static_cast<int>(tr2->keys.size())) return;
      tr2->keys[static_cast<size_t>(idx)] = nk;
      tr2->sortKeys();
      for (size_t i = 0; i < tr2->keys.size(); ++i)
        if (std::fabs(tr2->keys[i].time - nk.time) < 1e-9 && tr2->keys[i].value == nk.value) keyIndex_ = static_cast<int>(i);
    });
    editing_ = false;
    Q_UNUSED(path);
    refreshKeyInfo();
  }

  // ---- Constraint rollouts ----
  QWidget* buildConstraintRollout(uint32_t nodeId, int index) {
    Animation* anim = c_->animation();
    Constraint& con = anim->nodes[nodeId].constraints[static_cast<size_t>(index)];
    auto* r = new Rollout(constraintTitle(con.type).replace(" Constraint", tr(" Parameters")), constraintsHost_);
    auto* targets = new QListWidget(r->body());
    targets->setMaximumHeight(80);
    for (size_t k = 0; k < con.targets.size(); ++k) {
      const Node* tn = c_->scene().findNode(con.targets[k].nodeId);
      QString name = tn ? QString::fromStdString(tn->name) : tr("World");
      if (con.type == ConstraintType::Link) targets->addItem(QStringLiteral("%1 @ %2").arg(name).arg(con.targets[k].frame));
      else targets->addItem(QStringLiteral("%1   %2").arg(name).arg(con.targets[k].weight));
    }
    r->form()->addRow(targets);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(con.type == ConstraintType::Link ? tr("Add Link") : tr("Add Target"), r->body());
    auto* del = new QPushButton(con.type == ConstraintType::Link ? tr("Delete Link") : tr("Delete Target"), r->body());
    add->setMinimumWidth(10);
    del->setMinimumWidth(10);
    targets->setMinimumWidth(10);
    row->addWidget(add);
    row->addWidget(del);
    r->form()->addRow(row);
    // Changes go through a normal edit so Auto Key can key animatable values.
    auto change = [this, nodeId, index](const QString& label, const std::function<void(Constraint&)>& fn) {
      Animation* a = c_->animation();
      auto it = a->nodes.find(nodeId);
      if (it == a->nodes.end() || index >= static_cast<int>(it->second.constraints.size())) return;
      editing_ = true;
      c_->document()->beginEdit(label);
      fn(it->second.constraints[static_cast<size_t>(index)]);
      a->touch();
      c_->document()->endEdit();
      c_->refresh();
      editing_ = false;
    };
    connect(add, &QPushButton::clicked, this, [this, nodeId, index, type = con.type, change] {
      std::function<bool(const Node&)> filter;
      if (type == ConstraintType::Path) filter = [](const Node& n) { return n.kind() == ObjectKind::Shape; };
      startPickNode(c_->window(), tr("Pick a target"), filter, [this, nodeId, index, type, change](uint32_t t) {
        if (t == nodeId) return;
        if (type == ConstraintType::Link) {
          c_->editAnimation(tr("Add Link"), [&] { c_->animation()->addLinkTarget(c_->scene(), nodeId, t, c_->frame()); });
          return;
        }
        change(tr("Add Target"), [t](Constraint& cc) {
          ConstraintTarget ct;
          ct.nodeId = t;
          cc.targets.push_back(ct);
        });
        rebuild();
      });
    });
    connect(del, &QPushButton::clicked, this, [this, targets, change] {
      int row = targets->currentRow();
      if (row < 0) return;
      change(tr("Delete Target"), [row](Constraint& cc) {
        if (row < static_cast<int>(cc.targets.size())) cc.targets.erase(cc.targets.begin() + row);
      });
      rebuild();
    });
    if (con.type != ConstraintType::Link) {
      auto* weight = spin(r->body(), 0, 100, 1, 1);
      weight->setValue(con.targets.empty() ? 50.0 : con.targets[0].weight);
      r->form()->addRow(tr("Weight:"), weight);
      connect(targets, &QListWidget::currentRowChanged, this, [this, nodeId, index, weight](int row) {
        auto& cc = c_->animation()->nodes[nodeId].constraints[static_cast<size_t>(index)];
        if (row >= 0 && row < static_cast<int>(cc.targets.size())) {
          QSignalBlocker b(weight);
          weight->setValue(cc.targets[static_cast<size_t>(row)].weight);
        }
      });
      connect(weight, &QDoubleSpinBox::valueChanged, this, [targets, change](double w) {
        int row = std::max(0, targets->currentRow());
        change(QObject::tr("Constraint Weight"), [row, w](Constraint& cc) {
          if (row < static_cast<int>(cc.targets.size())) cc.targets[static_cast<size_t>(row)].weight = w;
        });
      });
    } else {
      auto* world = new QPushButton(tr("Link to World"), r->body());
      r->form()->addRow(world);
      connect(world, &QPushButton::clicked, this, [this, nodeId] {
        c_->editAnimation(tr("Link to World"), [&] { c_->animation()->addLinkTarget(c_->scene(), nodeId, 0, c_->frame()); });
      });
    }
    auto check = [&](const QString& label, bool value, std::function<void(Constraint&, bool)> set) {
      auto* cb = new QCheckBox(label, r->body());
      cb->setChecked(value);
      r->form()->addRow(cb);
      connect(cb, &QCheckBox::toggled, this, [change, set, label](bool on) {
        change(label, [&](Constraint& cc) { set(cc, on); });
      });
    };
    auto number = [&](const QString& label, double value, double lo, double hi, std::function<void(Constraint&, double)> set,
                      const char* name = nullptr) {
      auto* s = spin(r->body(), lo, hi, 2, 1);
      if (name) s->setObjectName(name);
      s->setValue(value);
      r->form()->addRow(label, s);
      connect(s, &QDoubleSpinBox::valueChanged, this, [change, set, label](double v) {
        change(label, [&](Constraint& cc) { set(cc, v); });
      });
    };
    auto axis = [&](int value, std::function<void(Constraint&, int)> set) {
      auto* cb = new QComboBox(r->body());
      cb->addItems({"X", "Y", "Z"});
      cb->setCurrentIndex(value);
      r->form()->addRow(tr("Axis:"), cb);
      connect(cb, &QComboBox::currentIndexChanged, this, [change, set](int v) {
        change(QObject::tr("Axis"), [&](Constraint& cc) { set(cc, v); });
      });
    };
    switch (con.type) {
      case ConstraintType::Path: {
        std::string pct = Animation::nodePath(nodeId, "c:" + std::to_string(index) + "/percent");
        double cur = anim->isAnimated(pct) ? anim->valueAt(c_->scene(), pct, c_->frame()) : con.percent;
        number(tr("% Along Path:"), cur, -1e6, 1e6, [](Constraint& cc, double v) { cc.percent = v; }, "pathPercent");
        check(tr("Follow"), con.follow, [](Constraint& cc, bool v) { cc.follow = v; });
        check(tr("Bank"), con.bank, [](Constraint& cc, bool v) { cc.bank = v; });
        number(tr("Bank Amount:"), con.bankAmount, -100, 100, [](Constraint& cc, double v) { cc.bankAmount = v; });
        number(tr("Smoothness:"), con.smoothness, 0, 10, [](Constraint& cc, double v) { cc.smoothness = v; });
        check(tr("Constant Velocity"), con.constantVelocity, [](Constraint& cc, bool v) { cc.constantVelocity = v; });
        check(tr("Loop"), con.loop, [](Constraint& cc, bool v) { cc.loop = v; });
        check(tr("Flip"), con.flip, [](Constraint& cc, bool v) { cc.flip = v; });
        axis(con.axis, [](Constraint& cc, int v) { cc.axis = v; });
        break;
      }
      case ConstraintType::LookAt: {
        axis(con.axis, [](Constraint& cc, int v) { cc.axis = v; });
        check(tr("Flip"), con.flip, [](Constraint& cc, bool v) { cc.flip = v; });
        check(tr("Keep Initial Offset"), con.keepOffset, [](Constraint& cc, bool v) { cc.keepOffset = v; });
        auto* up = new QPushButton(con.upNode ? tr("Upnode: %1").arg(QString::fromStdString(c_->scene().findNode(con.upNode)
                                                                                               ? c_->scene().findNode(con.upNode)->name
                                                                                               : std::string("?")))
                                              : tr("Upnode: World (pick...)"),
                                   r->body());
        r->form()->addRow(up);
        connect(up, &QPushButton::clicked, this, [this, change] {
          startPickNode(c_->window(), tr("Pick the up node"), {}, [this, change](uint32_t id) {
            change(tr("Upnode"), [id](Constraint& cc) { cc.upNode = id; });
            rebuild();
          });
        });
        break;
      }
      case ConstraintType::Position:
      case ConstraintType::Orientation:
        check(tr("Keep Initial Offset"), con.keepOffset, [](Constraint& cc, bool v) { cc.keepOffset = v; });
        break;
      case ConstraintType::Attachment:
        if (!con.targets.empty()) {
          ConstraintTarget& t = con.targets.front();
          number(tr("Face:"), t.face, 0, 1e7, [](Constraint& cc, double v) {
            if (!cc.targets.empty()) cc.targets.front().face = static_cast<int>(v);
          });
          number(tr("A:"), t.baryU, 0, 1, [](Constraint& cc, double v) {
            if (!cc.targets.empty()) cc.targets.front().baryU = v;
          });
          number(tr("B:"), t.baryV, 0, 1, [](Constraint& cc, double v) {
            if (!cc.targets.empty()) cc.targets.front().baryV = v;
          });
        }
        check(tr("Align to Surface"), con.align, [](Constraint& cc, bool v) { cc.align = v; });
        break;
      case ConstraintType::Surface:
        number(tr("U Position:"), con.u, -1e4, 1e4, [](Constraint& cc, double v) { cc.u = v; });
        number(tr("V Position:"), con.v, -1e4, 1e4, [](Constraint& cc, double v) { cc.v = v; });
        check(tr("Align to Surface"), con.align, [](Constraint& cc, bool v) { cc.align = v; });
        break;
      case ConstraintType::Link: break;
    }
    auto* remove = new QPushButton(tr("Delete Constraint"), r->body());
    r->form()->addRow(remove);
    connect(remove, &QPushButton::clicked, this, [this, nodeId, index] {
      c_->editAnimation(tr("Delete Constraint"), [&] {
        auto& cs = c_->animation()->nodes[nodeId].constraints;
        if (index < static_cast<int>(cs.size())) cs.erase(cs.begin() + index);
      });
    });
    return r;
  }

  // ---- Trajectory tools ----
  void convertToSpline() {
    Node* n = c_->document()->firstSelected();
    Animation* anim = c_->animation();
    if (!n || !anim) return;
    int s0 = sampleStart_->value(), s1 = sampleEnd_->value(), count = samples_->value();
    if (s1 <= s0) return;
    SceneDocument* doc = c_->document();
    doc->beginEdit(tr("Convert To Spline"));
    Node* line = doc->createNode("EditableSpline");
    if (line) {
      line->name = c_->scene().uniqueNodeName(n->name + "_Trajectory");
      auto* sp = static_cast<EditableSplineObject*>(line->object.get());
      Spline spline;
      for (int i = 0; i < count; ++i) {
        double f = s0 + (s1 - s0) * static_cast<double>(i) / (count - 1);
        Knot k = cornerKnot(anim->worldMatrixAt(c_->scene(), n->id, f).translation());
        k.type = KnotType::Smooth;
        spline.knots.push_back(k);
      }
      spline.updateAutoHandles();
      sp->shape.splines = {spline};
      line->invalidate();
      doc->notifyStructureChanged();
    }
    doc->endEdit();
  }

  void convertFromSpline(uint32_t nodeId, uint32_t shapeId) {
    Animation* anim = c_->animation();
    std::vector<Vec3> poly = shapeWorldPolylineAt(c_->scene(), shapeId, c_->frame());
    if (!anim || poly.size() < 2) return;
    int s0 = sampleStart_->value(), s1 = sampleEnd_->value(), count = samples_->value();
    if (s1 <= s0) return;
    c_->editAnimation(tr("Convert From Spline"), [&] {
      Scene& s = c_->scene();
      Node* n = s.findNode(nodeId);
      if (!n) return;
      Mat4 parentInv = n->parentId ? anim->worldMatrixAt(s, n->parentId, c_->frame()).inverse() : Mat4::identity();
      for (int k = 0; k < 3; ++k) anim->tracks.erase(Animation::nodePath(nodeId, Animation::transformChannels()[static_cast<size_t>(k)]));
      for (int i = 0; i < count; ++i) {
        double t = static_cast<double>(i) / (count - 1);
        double f = s0 + (s1 - s0) * t;
        Vec3 p, tan;
        polylinePointAt(poly, t * 100.0, true, p, tan);
        Vec3 local = parentInv.transformPoint(p);
        for (int k = 0; k < 3; ++k)
          anim->tracks[Animation::nodePath(nodeId, Animation::transformChannels()[static_cast<size_t>(k)])].setKey(f, local[k]);
      }
    });
  }

  void collapseTransforms() {
    Animation* anim = c_->animation();
    if (!anim) return;
    QVector<Node*> nodes = c_->document()->selectedNodes();
    int s0 = sampleStart_->value(), s1 = sampleEnd_->value();
    if (nodes.isEmpty() || s1 <= s0) return;
    c_->editAnimation(tr("Collapse Transform"), [&] {
      Scene& s = c_->scene();
      for (Node* n : nodes) {
        std::vector<Transform> samples;
        for (int f = s0; f <= s1; ++f) {
          Mat4 w = anim->worldMatrixAt(s, n->id, f);
          Mat4 pw = n->parentId ? anim->worldMatrixAt(s, n->parentId, f) : Mat4::identity();
          Transform t = decomposeTransform(pw.inverse() * w);
          if (!samples.empty())
            for (int k = 0; k < 3; ++k) t.rotation[k] += 360.0f * std::round((samples.back().rotation[k] - t.rotation[k]) / 360.0f);
          samples.push_back(t);
        }
        anim->nodes[n->id].constraints.clear();
        anim->nodes[n->id].rotationController = 0;
        anim->nodes[n->id].rotationKeys.clear();
        for (int c = 0; c < 9; ++c) {
          Track tr;
          for (size_t i = 0; i < samples.size(); ++i) {
            const Transform& t = samples[i];
            float v = c < 3 ? t.position[c] : c < 6 ? t.rotation[c - 3] : t.scale[c - 6];
            tr.setKey(s0 + static_cast<double>(i), v);
          }
          anim->tracks[Animation::nodePath(n->id, Animation::transformChannels()[static_cast<size_t>(c)])] = tr;
        }
      }
      // IK chains driving these bones no longer apply.
      for (const auto& up : s.nodes)
        if (up->object && up->object->type() == "IKChain")
          for (Node* n : nodes)
            if (up->object->params.getInt("endBone") == static_cast<int>(n->id)) up->object->params.setNumber("enabled", 0);
    });
  }

  AnimController* c_;
  QPushButton *paramsBtn_, *trajBtn_;
  QLabel* nodeLabel_;
  QStackedWidget* pages_;
  QTreeWidget* tree_ = nullptr;
  QLabel *trackLabel_ = nullptr, *keyNum_ = nullptr;
  QDoubleSpinBox *time_ = nullptr, *value_ = nullptr, *inSlope_ = nullptr, *outSlope_ = nullptr, *tension_ = nullptr,
                 *continuity_ = nullptr, *bias_ = nullptr, *easeTo_ = nullptr, *easeFrom_ = nullptr;
  QComboBox *inType_ = nullptr, *outType_ = nullptr;
  QWidget* constraintsHost_ = nullptr;
  QVBoxLayout* constraintsLayout_ = nullptr;
  QCheckBox *showTraj_ = nullptr, *nodeTraj_ = nullptr;
  QSpinBox *sampleStart_ = nullptr, *sampleEnd_ = nullptr, *samples_ = nullptr;
  uint32_t nodeId_ = 0;
  std::string currentPath_;
  int keyIndex_ = 0;
  bool loading_ = false;
  bool editing_ = false;
};

void installMotionPanel(MainWindow& w) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  auto* panel = new MotionPanel(c);
  w.addCommandPanelTab(panel, animicons::motion(), QObject::tr("Motion"));
  auto* show = new QAction(QObject::tr("Motion Panel"), &w);
  w.registerCommand("anim.motionPanel", show);
  QObject::connect(show, &QAction::triggered, &w, [&w, panel] { w.commandPanel()->setCurrentWidget(panel); });
}

}  // namespace

VERAA_APP_EXTENSION(motionPanel, 302, installMotionPanel);

}  // namespace veraa
