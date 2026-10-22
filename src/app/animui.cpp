#include "animui.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <set>

#include "animcontroller.h"
#include "mainwindow.h"
#include "picktool.h"

namespace veraa {

void startPickNode(MainWindow& w, const QString& prompt, std::function<bool(const Node&)> filter,
                   std::function<void(uint32_t)> done) {
  w.editorState()->setViewportTool(std::make_shared<PickNodeTool>(prompt, std::move(filter), std::move(done)));
  if (w.promptLabel()) w.promptLabel()->setText(prompt);
}

uint32_t pickNodeByName(QWidget* parent, const Scene& scene, const QString& title, std::function<bool(const Node&)> filter) {
  QDialog d(parent);
  d.setWindowTitle(title);
  auto* v = new QVBoxLayout(&d);
  auto* find = new QLineEdit(&d);
  find->setPlaceholderText(QObject::tr("Find..."));
  auto* list = new QListWidget(&d);
  for (const auto& up : scene.nodes) {
    if (filter && !filter(*up)) continue;
    auto* item = new QListWidgetItem(QString::fromStdString(up->name), list);
    item->setData(Qt::UserRole, up->id);
  }
  list->setCurrentRow(0);
  QObject::connect(find, &QLineEdit::textChanged, &d, [list](const QString& t) {
    for (int i = 0; i < list->count(); ++i) list->item(i)->setHidden(!list->item(i)->text().contains(t, Qt::CaseInsensitive));
  });
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
  QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  QObject::connect(list, &QListWidget::itemDoubleClicked, &d, &QDialog::accept);
  v->addWidget(find);
  v->addWidget(list);
  v->addWidget(bb);
  if (d.exec() != QDialog::Accepted || !list->currentItem()) return 0;
  return list->currentItem()->data(Qt::UserRole).toUInt();
}

QString controllerName(const Track* t) {
  if (!t) return QObject::tr("Bezier Float");
  switch (t->type) {
    case ControllerType::Bezier: return QObject::tr("Bezier Float");
    case ControllerType::Linear: return QObject::tr("Linear Float");
    case ControllerType::TCB: return QObject::tr("TCB Float");
    case ControllerType::Noise: return QObject::tr("Noise Float");
    case ControllerType::Expression: return QObject::tr("Float Expression");
    case ControllerType::List: return QObject::tr("Float List");
  }
  return QString();
}

QString constraintTitle(ConstraintType t) {
  switch (t) {
    case ConstraintType::Path: return QObject::tr("Path Constraint");
    case ConstraintType::LookAt: return QObject::tr("Look-At Constraint");
    case ConstraintType::Position: return QObject::tr("Position Constraint");
    case ConstraintType::Orientation: return QObject::tr("Orientation Constraint");
    case ConstraintType::Link: return QObject::tr("Link Constraint");
    case ConstraintType::Attachment: return QObject::tr("Attachment Constraint");
    case ConstraintType::Surface: return QObject::tr("Surface Constraint");
  }
  return QString();
}

namespace {

// Nearest triangle and barycentric coordinates of a world point on a node's mesh.
void nearestFace(const Node& target, const Mat4& world, const Vec3& p, int& face, double& u, double& v) {
  const TriMesh& tm = target.triMesh();
  float best = kInfinity;
  face = 0;
  u = v = 1.0 / 3.0;
  for (size_t f = 0; f < tm.triangleCount(); ++f) {
    Vec3 a = world.transformPoint(tm.positions[tm.indices[f * 3]]);
    Vec3 b = world.transformPoint(tm.positions[tm.indices[f * 3 + 1]]);
    Vec3 c = world.transformPoint(tm.positions[tm.indices[f * 3 + 2]]);
    // Project onto the plane and clamp barycentrics.
    Vec3 e0 = b - a, e1 = c - a, d = p - a;
    float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(d, e0), d21 = dot(d, e1);
    float den = d00 * d11 - d01 * d01;
    if (std::fabs(den) < 1e-20f) continue;
    float bv = (d11 * d20 - d01 * d21) / den, bw = (d00 * d21 - d01 * d20) / den;
    bv = clampf(bv, 0.0f, 1.0f);
    bw = clampf(bw, 0.0f, 1.0f - bv);
    Vec3 q = a + e0 * bv + e1 * bw;
    float dist = length(q - p);
    if (dist < best) {
      best = dist;
      face = static_cast<int>(f);
      u = bv;
      v = bw;
    }
  }
}

}  // namespace

void addConstraintTo(AnimController* c, uint32_t nodeId, ConstraintType type, uint32_t targetId, bool keepOffset) {
  Animation* anim = c->animation();
  Scene& s = c->scene();
  if (!anim || !s.findNode(nodeId) || nodeId == targetId) return;
  c->editAnimation(constraintTitle(type), [&] {
    if (type == ConstraintType::Link) {
      anim->addLinkTarget(s, nodeId, targetId, c->frame());
      return;
    }
    Constraint con;
    con.type = type;
    con.keepOffset = keepOffset;
    ConstraintTarget t;
    t.nodeId = targetId;
    if (type == ConstraintType::Attachment)
      if (const Node* tn = s.findNode(targetId)) {
        Mat4 m = anim->worldMatrixAt(s, targetId, c->frame()) * tn->objectOffset.matrix();
        nearestFace(*tn, m, anim->worldMatrixAt(s, nodeId, c->frame()).translation(), t.face, t.baryU, t.baryV);
      }
    con.targets.push_back(t);
    if (type == ConstraintType::Path) con.follow = false;
    anim->addConstraint(s, nodeId, con);
  });
}

void beginConstraintPick(MainWindow& w, ConstraintType type) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  QVector<uint32_t> sel = c->document()->selection();
  if (sel.isEmpty()) {
    QMessageBox::information(&w, constraintTitle(type), QObject::tr("Select the object(s) to constrain first."));
    return;
  }
  std::function<bool(const Node&)> filter;
  QString what = QObject::tr("target object");
  if (type == ConstraintType::Path) {
    filter = [](const Node& n) { return n.kind() == ObjectKind::Shape; };
    what = QObject::tr("path (a shape)");
  } else if (type == ConstraintType::Attachment || type == ConstraintType::Surface) {
    filter = [](const Node& n) { return n.kind() == ObjectKind::Geometry; };
    what = QObject::tr("surface object");
  }
  auto selection = sel;
  auto userFilter = filter;
  startPickNode(w, QObject::tr("%1: pick the %2 in a viewport").arg(constraintTitle(type), what),
                [selection, userFilter](const Node& n) { return !selection.contains(n.id) && (!userFilter || userFilter(n)); },
                [c, selection, type](uint32_t target) {
                  for (uint32_t id : selection) addConstraintTo(c, id, type, target);
                });
}

void assignFloatController(AnimController* c, const std::vector<std::string>& paths, ControllerType type) {
  Animation* anim = c->animation();
  if (!anim || paths.empty()) return;
  c->editAnimation(QObject::tr("Assign Controller"), [&] {
    for (const auto& path : paths) {
      double current = 0.0;
      Animation::readValue(c->scene(), path, current);
      Track& t = anim->tracks[path];
      bool hadKeys = t.hasKeys() || t.type == ControllerType::List;
      if (type == ControllerType::Noise && !hadKeys) {
        // Keep the current value: a list of a constant Bezier track and the noise.
        Track base;
        base.setKey(c->startFrame(), current);
        t = base;
        t.setController(ControllerType::List);
        Track noise;
        noise.setController(ControllerType::Noise);
        t.subs.push_back(noise);
        t.subNames.push_back("Noise Float");
        t.weights.push_back(100.0);
        continue;
      }
      t.setController(type);
      if (type == ControllerType::Expression && t.expression.empty()) t.expression = QString::number(current, 'g', 8).toStdString();
      if ((type == ControllerType::Bezier || type == ControllerType::Linear || type == ControllerType::TCB) && !t.hasKeys()) {
        // Nothing to animate yet; drop the empty track.
        anim->tracks.erase(path);
      }
    }
  });
}

void setRotationController(AnimController* c, uint32_t nodeId, int controller) {
  Animation* anim = c->animation();
  Node* n = c->scene().findNode(nodeId);
  if (!anim || !n) return;
  NodeAnimation& na = anim->nodes[nodeId];
  if (na.rotationController == controller) return;
  c->editAnimation(QObject::tr("Assign Rotation Controller"), [&] {
    Scene& s = c->scene();
    std::string base = "node:" + std::to_string(nodeId) + "/";
    if (controller == 1) {
      std::set<double> times;
      for (const char* ch : {"rot.x", "rot.y", "rot.z"})
        if (const Track* t = anim->findTrack(base + ch)) {
          std::vector<double> ts;
          t->keyTimes(ts);
          times.insert(ts.begin(), ts.end());
        }
      std::vector<std::pair<double, Vec3>> samples;
      for (double t : times)
        samples.emplace_back(t, Vec3(static_cast<float>(anim->valueAt(s, base + "rot.x", t)),
                                     static_cast<float>(anim->valueAt(s, base + "rot.y", t)),
                                     static_cast<float>(anim->valueAt(s, base + "rot.z", t))));
      for (const char* ch : {"rot.x", "rot.y", "rot.z"}) anim->tracks.erase(base + ch);
      na.rotationController = 1;
      na.rotationKeys.clear();
      for (const auto& [t, e] : samples) anim->setRotationKey(nodeId, t, e);
    } else {
      std::vector<RotationKey> keys = na.rotationKeys;
      na.rotationKeys.clear();
      na.rotationController = 0;
      Vec3 prev = n->transform.rotation;
      bool first = true;
      for (const auto& k : keys) {
        Vec3 e = k.q.toEulerDeg();
        if (!first)
          for (int i = 0; i < 3; ++i) e[i] += 360.0f * std::round((prev[i] - e[i]) / 360.0f);
        first = false;
        prev = e;
        for (int i = 0; i < 3; ++i) {
          Track& t = anim->tracks[base + Animation::transformChannels()[static_cast<size_t>(3 + i)]];
          t.setKey(k.time, e[i]);
        }
      }
    }
  });
}

uint32_t createIkChain(AnimController* c, uint32_t startBone, uint32_t endBone, int solver) {
  Scene& s = c->scene();
  Node* end = s.findNode(endBone);
  if (!end || !s.findNode(startBone) || startBone == endBone || !s.isAncestor(startBone, endBone)) return 0;
  uint32_t id = 0;
  SceneDocument* doc = c->document();
  doc->beginEdit(QObject::tr("IK Solver"));
  Node* ik = doc->createNode("IKChain");
  if (ik) {
    ik->name = s.uniqueNodeName("IK Chain");
    ik->object->params.setNumber("startBone", startBone);
    ik->object->params.setNumber("endBone", endBone);
    ik->object->params.setNumber("solver", solver);
    ik->transform.position = end->worldPosition();
    id = ik->id;
    doc->notifyStructureChanged();
    doc->setSelection({id});
  }
  doc->endEdit();
  c->refresh();
  return id;
}

void editControllerProperties(AnimController* c, const std::string& path, QWidget* parent) {
  Animation* anim = c->animation();
  if (!anim) return;
  Track* t = anim->findTrack(path);
  if (!t) {
    QMessageBox::information(parent, QObject::tr("Properties"), QObject::tr("This track has no controller properties."));
    return;
  }
  // Noise inside a list: edit the first noise entry.
  Track* target = t;
  if (t->type == ControllerType::List)
    for (auto& sub : t->subs)
      if (sub.type == ControllerType::Noise) target = &sub;
  QDialog d(parent);
  d.setWindowTitle(QString::fromStdString(Animation::displayName(c->scene(), path)) + " - " + controllerName(target));
  auto* v = new QVBoxLayout(&d);
  auto* form = new QFormLayout;
  v->addLayout(form);
  Track edit = *target;
  // Widgets by controller type.
  QSpinBox* seed = nullptr;
  QDoubleSpinBox *freq = nullptr, *strength = nullptr, *rough = nullptr;
  QCheckBox* fractal = nullptr;
  QPlainTextEdit* expr = nullptr;
  QTableWidget* vars = nullptr;
  QLabel* exprStatus = nullptr;
  QTableWidget* list = nullptr;
  if (target->type == ControllerType::Noise) {
    seed = new QSpinBox(&d);
    seed->setRange(0, 1000000);
    seed->setValue(edit.noise.seed);
    freq = new QDoubleSpinBox(&d);
    freq->setRange(0.0001, 100);
    freq->setDecimals(3);
    freq->setValue(edit.noise.frequency);
    strength = new QDoubleSpinBox(&d);
    strength->setRange(-1e6, 1e6);
    strength->setValue(edit.noise.strength);
    fractal = new QCheckBox(QObject::tr("Fractal Noise"), &d);
    fractal->setChecked(edit.noise.fractal);
    rough = new QDoubleSpinBox(&d);
    rough->setRange(0, 1);
    rough->setSingleStep(0.05);
    rough->setValue(edit.noise.roughness);
    form->addRow(QObject::tr("Seed:"), seed);
    form->addRow(QObject::tr("Frequency:"), freq);
    form->addRow(QObject::tr("Strength:"), strength);
    form->addRow(fractal);
    form->addRow(QObject::tr("Roughness:"), rough);
  } else if (target->type == ControllerType::Expression) {
    expr = new QPlainTextEdit(QString::fromStdString(edit.expression), &d);
    expr->setMinimumHeight(80);
    exprStatus = new QLabel(&d);
    form->addRow(QObject::tr("Expression:"), expr);
    form->addRow(exprStatus);
    form->addRow(new QLabel(QObject::tr("Predefined: F (frame), T (ticks), S (seconds), NT (normalized time)."), &d));
    vars = new QTableWidget(0, 3, &d);
    vars->setHorizontalHeaderLabels({QObject::tr("Variable"), QObject::tr("Track path or constant"), QObject::tr("Offset")});
    vars->horizontalHeader()->setStretchLastSection(true);
    for (const auto& ev : edit.exprVars) {
      int r = vars->rowCount();
      vars->insertRow(r);
      vars->setItem(r, 0, new QTableWidgetItem(QString::fromStdString(ev.name)));
      vars->setItem(r, 1, new QTableWidgetItem(ev.path.empty() ? QString::number(ev.constant) : QString::fromStdString(ev.path)));
      vars->setItem(r, 2, new QTableWidgetItem(QString::number(ev.frameOffset)));
    }
    auto* addVar = new QPushButton(QObject::tr("Add Variable"), &d);
    QObject::connect(addVar, &QPushButton::clicked, &d, [vars] {
      int r = vars->rowCount();
      vars->insertRow(r);
      vars->setItem(r, 0, new QTableWidgetItem(QStringLiteral("v%1").arg(r + 1)));
      vars->setItem(r, 1, new QTableWidgetItem("0"));
      vars->setItem(r, 2, new QTableWidgetItem("0"));
    });
    form->addRow(vars);
    form->addRow(addVar);
    auto validate = [expr, exprStatus] {
      Expression e;
      std::string err;
      if (e.parse(expr->toPlainText().toStdString(), &err)) exprStatus->setText(QObject::tr("Expression is valid."));
      else exprStatus->setText(QObject::tr("Error: %1").arg(QString::fromStdString(err)));
    };
    QObject::connect(expr, &QPlainTextEdit::textChanged, &d, validate);
    validate();
  } else if (target->type == ControllerType::List) {
    list = new QTableWidget(static_cast<int>(edit.subs.size()), 3, &d);
    list->setHorizontalHeaderLabels({QObject::tr("Entry"), QObject::tr("Weight %"), QObject::tr("Active")});
    for (size_t i = 0; i < edit.subs.size(); ++i) {
      int r = static_cast<int>(i);
      QString name = i < edit.subNames.size() ? QString::fromStdString(edit.subNames[i]) : controllerName(&edit.subs[i]);
      auto* it = new QTableWidgetItem(name);
      it->setFlags(it->flags() & ~Qt::ItemIsEditable);
      list->setItem(r, 0, it);
      list->setItem(r, 1, new QTableWidgetItem(QString::number(i < edit.weights.size() ? edit.weights[i] : 100.0)));
      auto* act = new QTableWidgetItem;
      act->setCheckState(static_cast<int>(i) == edit.active ? Qt::Checked : Qt::Unchecked);
      list->setItem(r, 2, act);
    }
    form->addRow(list);
  } else {
    form->addRow(new QLabel(QObject::tr("Keyframed controller: edit keys in the Curve Editor or Key Info."), &d));
  }
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
  v->addWidget(bb);
  QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  if (d.exec() != QDialog::Accepted) return;
  if (seed) {
    edit.noise.seed = seed->value();
    edit.noise.frequency = freq->value();
    edit.noise.strength = strength->value();
    edit.noise.fractal = fractal->isChecked();
    edit.noise.roughness = rough->value();
  }
  if (expr) {
    edit.expression = expr->toPlainText().toStdString();
    edit.exprVars.clear();
    for (int r = 0; r < vars->rowCount(); ++r) {
      Track::ExprVar ev;
      ev.name = vars->item(r, 0) ? vars->item(r, 0)->text().trimmed().toStdString() : std::string();
      QString val = vars->item(r, 1) ? vars->item(r, 1)->text().trimmed() : QString();
      bool num = false;
      double cval = val.toDouble(&num);
      if (num) ev.constant = cval;
      else ev.path = val.toStdString();
      ev.frameOffset = vars->item(r, 2) ? vars->item(r, 2)->text().toDouble() : 0.0;
      if (!ev.name.empty()) edit.exprVars.push_back(ev);
    }
  }
  if (list) {
    for (int r = 0; r < list->rowCount() && r < static_cast<int>(edit.weights.size()); ++r) {
      edit.weights[static_cast<size_t>(r)] = list->item(r, 1)->text().toDouble();
      if (list->item(r, 2)->checkState() == Qt::Checked) edit.active = r;
    }
  }
  c->editAnimation(QObject::tr("Controller Properties"), [&] {
    Track* live = anim->findTrack(path);
    if (!live) return;
    Track* dst = live;
    if (live->type == ControllerType::List && target != t)
      for (auto& sub : live->subs)
        if (sub.type == ControllerType::Noise) dst = &sub;
    *dst = edit;
  });
}

}  // namespace veraa
