// Animation menu: IK solvers, constraints, controllers, Parameter Editor and Collector, Wire
// Parameters, Animation Layers, Reaction Manager, Bone Tools, skin pose, Walkthrough Assistant,
// rigid body simulation, Load/Save Animation and Delete Selected Animation.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>
#include <fstream>
#include <set>

#include "animcontroller.h"
#include "animui.h"
#include "dynamics.h"
#include "extension.h"
#include "mainwindow.h"
#include "rigging.h"

namespace veraa {

// ---- Shared dialogs (also used by self tests) ----

// Writes a value as a normal edit (so Auto Key reacts) and refreshes views.
void writeValueEdit(AnimController* c, const std::string& path, double v, const QString& label) {
  SceneDocument* doc = c->document();
  doc->beginEdit(label);
  if (Animation::writeValue(c->scene(), path, v)) {
    ValuePath p = ValuePath::parse(path);
    if (p.kind == ValuePath::Kind::MaterialParam) doc->notifyMaterialsChanged();
    else if (p.kind == ValuePath::Kind::RenderParam) doc->notifyRenderSettingsChanged();
    else if (p.id) doc->notifyNodesChanged({p.id}, ChangeTransform | ChangeGeometry);
  }
  if (Animation* a = c->animation()) a->touch();
  doc->endEdit();
  c->refresh();
}

// Saves the animation of nodes to a JSON clip. Returns false on errors.
bool saveAnimationClip(AnimController* c, const std::vector<uint32_t>& ids, const std::string& path, std::string* error) {
  Animation* anim = c->animation();
  if (!anim) return false;
  json clip{{"format", "veraa-animation"}, {"version", 1}, {"fps", c->fps()}, {"start", c->startFrame()}, {"end", c->endFrame()}};
  json nodes = json::object();
  for (uint32_t id : ids) {
    const Node* n = c->scene().findNode(id);
    if (!n) continue;
    json jn{{"tracks", json::object()}};
    std::string prefix = "node:" + std::to_string(id) + "/";
    for (const auto& p : anim->nodeTrackPaths(id))
      if (const Track* t = anim->findTrack(p)) jn["tracks"][p.substr(prefix.size())] = t->toJson();
    auto na = anim->nodes.find(id);
    if (na != anim->nodes.end() && na->second.rotationController == 1) {
      json rk = json::array();
      for (const auto& k : na->second.rotationKeys)
        rk.push_back(json::array({k.time, k.q.x, k.q.y, k.q.z, k.q.w, k.tension, k.continuity, k.bias, k.easeTo, k.easeFrom}));
      jn["rotationKeys"] = rk;
    }
    nodes[n->name] = jn;
  }
  clip["nodes"] = nodes;
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  f << clip.dump(1);
  return static_cast<bool>(f);
}

// Loads a clip onto nodes with the same names (or onto the only selected node). Returns the
// number of nodes that received animation, or -1 on errors.
int loadAnimationClip(AnimController* c, const std::string& path, double offset, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot read " + path;
    return -1;
  }
  json clip;
  try {
    f >> clip;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return -1;
  }
  if (clip.value("format", std::string()) != "veraa-animation" || !clip.contains("nodes")) {
    if (error) *error = "Not an animation clip";
    return -1;
  }
  Animation* anim = c->animation();
  int count = 0;
  const json& nodes = clip["nodes"];
  QVector<Node*> sel = c->document()->selectedNodes();
  c->editAnimation(QObject::tr("Load Animation"), [&] {
    for (auto it = nodes.begin(); it != nodes.end(); ++it) {
      Node* target = c->scene().findNodeByName(it.key());
      if (!target && nodes.size() == 1 && sel.size() == 1) target = sel[0];
      if (!target) continue;
      std::string prefix = "node:" + std::to_string(target->id) + "/";
      const json& jn = it.value();
      if (jn.contains("tracks"))
        for (auto t = jn["tracks"].begin(); t != jn["tracks"].end(); ++t) {
          Track tr = Track::fromJson(t.value());
          for (auto& k : tr.keys) k.time += offset;
          anim->tracks[prefix + t.key()] = tr;
        }
      if (jn.contains("rotationKeys")) {
        NodeAnimation& na = anim->nodes[target->id];
        na.rotationController = 1;
        na.rotationKeys.clear();
        for (const auto& e : jn["rotationKeys"]) {
          if (!e.is_array() || e.size() < 5) continue;
          RotationKey k;
          k.time = e[0].get<double>() + offset;
          k.q = Quat{e[1].get<double>(), e[2].get<double>(), e[3].get<double>(), e[4].get<double>()};
          if (e.size() >= 10) {
            k.tension = e[5].get<double>();
            k.continuity = e[6].get<double>();
            k.bias = e[7].get<double>();
            k.easeTo = e[8].get<double>();
            k.easeFrom = e[9].get<double>();
          }
          na.rotationKeys.push_back(k);
        }
      }
      ++count;
    }
  });
  return count;
}

namespace {

std::string pickValuePath(QWidget* parent, AnimController* c, const QString& title) {
  QDialog d(parent);
  d.setWindowTitle(title);
  auto* v = new QVBoxLayout(&d);
  auto* nodeBox = new QComboBox(&d);
  for (const auto& up : c->scene().nodes) nodeBox->addItem(QString::fromStdString(up->name), up->id);
  if (Node* n = c->document()->firstSelected()) nodeBox->setCurrentIndex(nodeBox->findData(n->id));
  auto* list = new QListWidget(&d);
  auto fill = [c, list, nodeBox] {
    list->clear();
    uint32_t id = nodeBox->currentData().toUInt();
    if (!c->animation()) return;
    for (const auto& p : c->animation()->animatablePaths(c->scene(), id)) {
      auto* it = new QListWidgetItem(QString::fromStdString(Animation::displayName(c->scene(), p)), list);
      it->setData(Qt::UserRole, QString::fromStdString(p));
    }
    list->setCurrentRow(0);
  };
  fill();
  QObject::connect(nodeBox, &QComboBox::currentIndexChanged, &d, fill);
  QObject::connect(list, &QListWidget::itemDoubleClicked, &d, &QDialog::accept);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
  QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  v->addWidget(new QLabel(QObject::tr("Object:"), &d));
  v->addWidget(nodeBox);
  v->addWidget(list);
  v->addWidget(bb);
  if (d.exec() != QDialog::Accepted || !list->currentItem()) return {};
  return list->currentItem()->data(Qt::UserRole).toString().toStdString();
}

QString pathLabel(AnimController* c, const std::string& p) { return QString::fromStdString(Animation::displayName(c->scene(), p)); }

// ---------------------------------------------------------------------------------------------
// Parameter Editor (custom attributes)

class ParameterEditor : public QDialog {
 public:
  explicit ParameterEditor(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Parameter Editor"));
    auto* v = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(tr("Param1"), this);
    type_ = new QComboBox(this);
    type_->addItems({tr("Float"), tr("Integer"), tr("Boolean")});
    min_ = new QDoubleSpinBox(this);
    max_ = new QDoubleSpinBox(this);
    def_ = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* s : {min_, max_, def_}) {
      s->setRange(-1e9, 1e9);
      s->setDecimals(3);
    }
    max_->setValue(100);
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Parameter Type:"), type_);
    form->addRow(tr("Range From:"), min_);
    form->addRow(tr("To:"), max_);
    form->addRow(tr("Default:"), def_);
    v->addLayout(form);
    auto* add = new QPushButton(tr("Add"), this);
    v->addWidget(add);
    list_ = new QListWidget(this);
    v->addWidget(new QLabel(tr("Attributes of the selected object:"), this));
    v->addWidget(list_);
    auto* del = new QPushButton(tr("Delete Attribute"), this);
    v->addWidget(del);
    connect(add, &QPushButton::clicked, this, [this] { addAttribute(); });
    connect(del, &QPushButton::clicked, this, [this] { deleteAttribute(); });
    connect(c->document(), &SceneDocument::selectionChanged, this, [this] { refresh(); });
    refresh();
  }

 private:
  void refresh() {
    list_->clear();
    Node* n = c_->document()->firstSelected();
    if (!n || !c_->animation()) return;
    auto it = c_->animation()->nodes.find(n->id);
    if (it == c_->animation()->nodes.end()) return;
    for (const auto& ca : it->second.custom)
      list_->addItem(QStringLiteral("%1  [%2 .. %3] = %4").arg(QString::fromStdString(ca.name)).arg(ca.minValue).arg(ca.maxValue).arg(ca.value));
  }
  void addAttribute() {
    QVector<Node*> nodes = c_->document()->selectedNodes();
    QString name = name_->text().trimmed();
    if (nodes.isEmpty() || name.isEmpty()) {
      QMessageBox::information(this, windowTitle(), tr("Select an object and enter a name."));
      return;
    }
    name.replace('/', '_');
    c_->editAnimation(tr("Add Attribute"), [&] {
      for (Node* n : nodes) {
        auto& custom = c_->animation()->nodes[n->id].custom;
        bool exists = false;
        for (const auto& ca : custom) exists |= ca.name == name.toStdString();
        if (exists) continue;
        CustomAttribute ca;
        ca.name = name.toStdString();
        ca.type = type_->currentIndex();
        ca.minValue = type_->currentIndex() == 2 ? 0 : min_->value();
        ca.maxValue = type_->currentIndex() == 2 ? 1 : max_->value();
        ca.defaultValue = ca.value = std::clamp(def_->value(), ca.minValue, ca.maxValue);
        custom.push_back(ca);
      }
    });
    refresh();
  }
  void deleteAttribute() {
    Node* n = c_->document()->firstSelected();
    int row = list_->currentRow();
    if (!n || row < 0) return;
    uint32_t id = n->id;
    c_->editAnimation(tr("Delete Attribute"), [&] {
      auto& custom = c_->animation()->nodes[id].custom;
      if (row < static_cast<int>(custom.size())) {
        c_->animation()->tracks.erase(Animation::nodePath(id, "custom/" + custom[static_cast<size_t>(row)].name));
        custom.erase(custom.begin() + row);
      }
    });
    refresh();
  }
  AnimController* c_;
  QLineEdit* name_;
  QComboBox* type_;
  QDoubleSpinBox *min_, *max_, *def_;
  QListWidget* list_;
};

// ---------------------------------------------------------------------------------------------
// Parameter Collector

class ParameterCollector : public QDialog {
 public:
  explicit ParameterCollector(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Parameter Collector"));
    resize(460, 360);
    auto* v = new QVBoxLayout(this);
    table_ = new QTableWidget(0, 2, this);
    table_->setHorizontalHeaderLabels({tr("Parameter"), tr("Value")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(table_);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Parameter..."), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* key = new QPushButton(tr("Key All"), this);
    row->addWidget(add);
    row->addWidget(remove);
    row->addWidget(key);
    v->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
      std::string p = pickValuePath(this, c_, tr("Collect Parameter"));
      if (p.empty()) return;
      SceneDocument* doc = c_->document();
      doc->beginEdit(tr("Collect Parameter"));
      json& list = doc->scene().extensions["parameterCollector"];
      if (!list.is_array()) list = json::array();
      list.push_back(p);
      doc->endEdit();
      rebuild();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
      int r = table_->currentRow();
      json& list = c_->scene().extensions["parameterCollector"];
      if (r < 0 || !list.is_array() || r >= static_cast<int>(list.size())) return;
      c_->document()->beginEdit(tr("Remove Collected Parameter"));
      list.erase(static_cast<size_t>(r));
      c_->document()->endEdit();
      rebuild();
    });
    connect(key, &QPushButton::clicked, this, [this] {
      const json& list = c_->scene().extensions["parameterCollector"];
      if (!list.is_array()) return;
      c_->editAnimation(tr("Key Collected Parameters"), [&] {
        for (const auto& p : list) {
          double val;
          std::string path = p.get<std::string>();
          if (Animation::readValue(c_->scene(), path, val))
            c_->animation()->setKey(c_->scene(), path, c_->frame(), val, c_->animation()->isAnimated(path) ? c_->startFrame() : c_->frame());
        }
      });
    });
    connect(c, &AnimController::frameChanged, this, [this] { refreshValues(); });
    connect(c->document(), &SceneDocument::nodesChanged, this, [this] { refreshValues(); });
    connect(c->document(), &SceneDocument::structureChanged, this, [this] { rebuild(); });
    rebuild();
  }

 private:
  void rebuild() {
    table_->setRowCount(0);
    spins_.clear();
    const json& list = c_->scene().extensions.contains("parameterCollector") ? c_->scene().extensions["parameterCollector"] : json();
    if (!list.is_array()) return;
    for (const auto& p : list) {
      std::string path = p.get<std::string>();
      int r = table_->rowCount();
      table_->insertRow(r);
      auto* name = new QTableWidgetItem(pathLabel(c_, path));
      name->setFlags(name->flags() & ~Qt::ItemIsEditable);
      table_->setItem(r, 0, name);
      auto* s = new QDoubleSpinBox(table_);
      s->setRange(-1e9, 1e9);
      s->setDecimals(3);
      s->setKeyboardTracking(false);
      table_->setCellWidget(r, 1, s);
      spins_.push_back({path, s});
      connect(s, &QDoubleSpinBox::valueChanged, this, [this, path](double v) {
        if (!loading_) writeValueEdit(c_, path, v, tr("Collected Parameter"));
      });
    }
    refreshValues();
  }
  void refreshValues() {
    loading_ = true;
    for (auto& [path, s] : spins_) {
      double v;
      if (Animation::readValue(c_->scene(), path, v)) s->setValue(v);
    }
    loading_ = false;
  }
  AnimController* c_;
  QTableWidget* table_;
  std::vector<std::pair<std::string, QDoubleSpinBox*>> spins_;
  bool loading_ = false;
};

// ---------------------------------------------------------------------------------------------
// Wire Parameters

class WireDialog : public QDialog {
 public:
  explicit WireDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Parameter Wiring"));
    resize(640, 420);
    auto* v = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    auto side = [&](QLineEdit*& path, QPushButton*& pick, QLineEdit*& expr, const QString& title) {
      auto* g = new QGroupBox(title, this);
      auto* f = new QFormLayout(g);
      path = new QLineEdit(g);
      path->setReadOnly(true);
      pick = new QPushButton(tr("Pick Parameter..."), g);
      expr = new QLineEdit("x", g);
      f->addRow(path);
      f->addRow(pick);
      f->addRow(tr("Expression (x = other side):"), expr);
      top->addWidget(g);
    };
    side(fromPath_, fromPick_, exprFrom_, tr("Master (from)"));
    side(toPath_, toPick_, exprTo_, tr("Slave (to)"));
    v->addLayout(top);
    direction_ = new QComboBox(this);
    direction_->addItems({tr("One-way: master drives slave"), tr("Two-way connection")});
    v->addWidget(direction_);
    auto* connectBtn = new QPushButton(tr("Connect"), this);
    v->addWidget(connectBtn);
    list_ = new QListWidget(this);
    v->addWidget(new QLabel(tr("Wires:"), this));
    v->addWidget(list_);
    auto* disconnect = new QPushButton(tr("Disconnect"), this);
    v->addWidget(disconnect);
    connect(fromPick_, &QPushButton::clicked, this, [this] {
      from_ = pickValuePath(this, c_, tr("Master Parameter"));
      fromPath_->setText(pathLabel(c_, from_));
    });
    connect(toPick_, &QPushButton::clicked, this, [this] {
      to_ = pickValuePath(this, c_, tr("Slave Parameter"));
      toPath_->setText(pathLabel(c_, to_));
    });
    connect(connectBtn, &QPushButton::clicked, this, [this] {
      if (from_.empty() || to_.empty() || from_ == to_) return;
      Expression e1, e2;
      std::string err;
      if (!e1.parse(exprTo_->text().toStdString(), &err) || !e2.parse(exprFrom_->text().toStdString(), &err)) {
        QMessageBox::warning(this, windowTitle(), tr("Expression error: %1").arg(QString::fromStdString(err)));
        return;
      }
      Wire w;
      w.from = from_;
      w.to = to_;
      w.exprTo = exprTo_->text().toStdString();
      w.exprFrom = exprFrom_->text().toStdString();
      w.twoWay = direction_->currentIndex() == 1;
      c_->editAnimation(tr("Wire Parameters"), [&] { c_->animation()->wires.push_back(w); });
      refresh();
    });
    connect(disconnect, &QPushButton::clicked, this, [this] {
      int r = list_->currentRow();
      if (r < 0) return;
      c_->editAnimation(tr("Disconnect Wire"), [&] {
        auto& w = c_->animation()->wires;
        if (r < static_cast<int>(w.size())) w.erase(w.begin() + r);
      });
      refresh();
    });
    refresh();
  }

 private:
  void refresh() {
    list_->clear();
    for (const auto& w : c_->animation()->wires)
      list_->addItem(QStringLiteral("%1 %2 %3   [%4]").arg(pathLabel(c_, w.from), w.twoWay ? "<->" : "->", pathLabel(c_, w.to),
                                                          QString::fromStdString(w.exprTo)));
  }
  AnimController* c_;
  QLineEdit *fromPath_, *toPath_, *exprFrom_, *exprTo_;
  QPushButton *fromPick_, *toPick_;
  QComboBox* direction_;
  QListWidget* list_;
  std::string from_, to_;
};

// ---------------------------------------------------------------------------------------------
// Animation Layers

class LayersDialog : public QDialog {
 public:
  explicit LayersDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Animation Layers"));
    resize(420, 340);
    auto* v = new QVBoxLayout(this);
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("Layer"), tr("Weight"), tr("Active")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    v->addWidget(table_);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Layer"), this);
    auto* del = new QPushButton(tr("Delete Layer"), this);
    auto* collapse = new QPushButton(tr("Collapse Layer"), this);
    row->addWidget(add);
    row->addWidget(del);
    row->addWidget(collapse);
    v->addLayout(row);
    connect(add, &QPushButton::clicked, this, [this] {
      c_->editAnimation(tr("Add Animation Layer"), [&] {
        AnimLayer L;
        L.name = "Layer " + std::to_string(c_->animation()->layers.size() + 1);
        c_->animation()->layers.push_back(L);
        c_->animation()->activeLayer = static_cast<int>(c_->animation()->layers.size());
      });
      refresh();
    });
    connect(del, &QPushButton::clicked, this, [this] {
      int r = table_->currentRow();
      if (r <= 0) return;
      c_->editAnimation(tr("Delete Animation Layer"), [&] {
        Animation* a = c_->animation();
        a->layers.erase(a->layers.begin() + (r - 1));
        if (a->activeLayer >= r) a->activeLayer = std::max(0, a->activeLayer - 1);
      });
      refresh();
    });
    connect(collapse, &QPushButton::clicked, this, [this] {
      int r = table_->currentRow();
      if (r <= 0) return;
      c_->editAnimation(tr("Collapse Animation Layer"), [&] {
        Animation* a = c_->animation();
        AnimLayer L = a->layers[static_cast<size_t>(r - 1)];
        double w = L.enabled ? L.weight / 100.0 : 0.0;
        for (const auto& [path, track] : L.tracks) {
          std::set<double> times;
          std::vector<double> ts;
          track.keyTimes(ts);
          times.insert(ts.begin(), ts.end());
          if (const Track* base = a->findTrack(path)) {
            ts.clear();
            base->keyTimes(ts);
            times.insert(ts.begin(), ts.end());
          }
          std::vector<std::pair<double, double>> values;
          for (double t : times) {
            const Track* base = a->findTrack(path);
            double bv = base && (base->hasKeys() || base->procedural()) ? base->evaluate(t) : 0.0;
            if (!base) Animation::readValue(c_->scene(), path, bv);
            values.emplace_back(t, bv + w * track.evaluate(t));
          }
          Track& base = a->tracks[path];
          for (const auto& [t, val] : values) base.setKey(t, val);
        }
        a->layers.erase(a->layers.begin() + (r - 1));
        if (a->activeLayer >= r) a->activeLayer = std::max(0, a->activeLayer - 1);
      });
      refresh();
    });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
      if (loading_) return;
      int r = it->row();
      Animation* a = c_->animation();
      if (it->column() == 2 && it->checkState() == Qt::Checked) {
        c_->editAnimation(tr("Set Active Layer"), [&] { a->activeLayer = r; });
        refresh();
        return;
      }
      if (r == 0) return;
      AnimLayer& L = a->layers[static_cast<size_t>(r - 1)];
      if (it->column() == 0) {
        bool enabled = it->checkState() == Qt::Checked;
        std::string name = it->text().toStdString();
        c_->editAnimation(tr("Edit Animation Layer"), [&] {
          L.enabled = enabled;
          L.name = name;
        });
      } else if (it->column() == 1) {
        double w = std::clamp(it->text().toDouble(), 0.0, 100.0);
        c_->editAnimation(tr("Layer Weight"), [&] { L.weight = w; });
      }
    });
    refresh();
  }

 private:
  void refresh() {
    loading_ = true;
    Animation* a = c_->animation();
    table_->setRowCount(static_cast<int>(a->layers.size()) + 1);
    auto setRow = [&](int r, const QString& name, double weight, bool enabled, bool editable) {
      auto* n = new QTableWidgetItem(name);
      if (editable) n->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
      else n->setFlags(n->flags() & ~Qt::ItemIsEditable);
      table_->setItem(r, 0, n);
      auto* w = new QTableWidgetItem(QString::number(weight));
      if (!editable) w->setFlags(w->flags() & ~Qt::ItemIsEditable);
      table_->setItem(r, 1, w);
      auto* act = new QTableWidgetItem;
      act->setCheckState(a->activeLayer == r ? Qt::Checked : Qt::Unchecked);
      table_->setItem(r, 2, act);
    };
    setRow(0, tr("Base Layer"), 100, true, false);
    for (size_t i = 0; i < a->layers.size(); ++i)
      setRow(static_cast<int>(i) + 1, QString::fromStdString(a->layers[i].name), a->layers[i].weight, a->layers[i].enabled, true);
    loading_ = false;
  }
  AnimController* c_;
  QTableWidget* table_;
  bool loading_ = false;
};

// ---------------------------------------------------------------------------------------------
// Reaction Manager

class ReactionDialog : public QDialog {
 public:
  explicit ReactionDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Reaction Manager"));
    resize(560, 440);
    auto* v = new QVBoxLayout(this);
    list_ = new QListWidget(this);
    v->addWidget(new QLabel(tr("Reactions (driver -> driven):"), this));
    v->addWidget(list_);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Reaction..."), this);
    auto* del = new QPushButton(tr("Delete Reaction"), this);
    smooth_ = new QCheckBox(tr("Smooth"), this);
    row->addWidget(add);
    row->addWidget(del);
    row->addWidget(smooth_);
    v->addLayout(row);
    points_ = new QTableWidget(0, 2, this);
    points_->setHorizontalHeaderLabels({tr("Driver value"), tr("Driven value")});
    v->addWidget(new QLabel(tr("States:"), this));
    v->addWidget(points_);
    auto* prow = new QHBoxLayout;
    auto* create = new QPushButton(tr("Create State (current values)"), this);
    auto* delState = new QPushButton(tr("Delete State"), this);
    prow->addWidget(create);
    prow->addWidget(delState);
    v->addLayout(prow);
    connect(add, &QPushButton::clicked, this, [this] {
      std::string driver = pickValuePath(this, c_, tr("Driver Parameter"));
      if (driver.empty()) return;
      std::string driven = pickValuePath(this, c_, tr("Driven Parameter"));
      if (driven.empty() || driven == driver) return;
      Reaction r;
      r.driver = driver;
      r.driven = driven;
      double dv = 0, nv = 0;
      Animation::readValue(c_->scene(), driver, dv);
      Animation::readValue(c_->scene(), driven, nv);
      r.points.push_back({dv, nv});
      c_->editAnimation(tr("Add Reaction"), [&] { c_->animation()->reactions.push_back(r); });
      refresh();
      list_->setCurrentRow(list_->count() - 1);
    });
    connect(del, &QPushButton::clicked, this, [this] {
      int r = list_->currentRow();
      if (r < 0) return;
      c_->editAnimation(tr("Delete Reaction"), [&] {
        auto& rs = c_->animation()->reactions;
        rs.erase(rs.begin() + r);
      });
      refresh();
    });
    connect(create, &QPushButton::clicked, this, [this] {
      int r = list_->currentRow();
      if (r < 0) return;
      Reaction& rc = c_->animation()->reactions[static_cast<size_t>(r)];
      double dv = c_->animation()->valueAt(c_->scene(), rc.driver, c_->frame());
      // The driven value as currently set in the scene (not the reaction's output).
      double nv = 0;
      Animation::readValue(c_->scene(), rc.driven, nv);
      c_->editAnimation(tr("Create Reaction State"), [&] {
        auto& pts = c_->animation()->reactions[static_cast<size_t>(r)].points;
        pts.erase(std::remove_if(pts.begin(), pts.end(), [dv](const auto& p) { return std::fabs(p.first - dv) < 1e-9; }), pts.end());
        pts.push_back({dv, nv});
        std::sort(pts.begin(), pts.end());
      });
      showPoints();
    });
    connect(delState, &QPushButton::clicked, this, [this] {
      int r = list_->currentRow(), p = points_->currentRow();
      if (r < 0 || p < 0) return;
      c_->editAnimation(tr("Delete Reaction State"), [&] {
        auto& pts = c_->animation()->reactions[static_cast<size_t>(r)].points;
        if (p < static_cast<int>(pts.size())) pts.erase(pts.begin() + p);
      });
      showPoints();
    });
    connect(smooth_, &QCheckBox::toggled, this, [this](bool on) {
      int r = list_->currentRow();
      if (r < 0 || loading_) return;
      c_->editAnimation(tr("Reaction Smoothing"), [&] { c_->animation()->reactions[static_cast<size_t>(r)].smooth = on; });
    });
    connect(points_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* it) {
      int r = list_->currentRow();
      if (r < 0 || loading_) return;
      double val = it->text().toDouble();
      c_->editAnimation(tr("Edit Reaction State"), [&] {
        auto& pts = c_->animation()->reactions[static_cast<size_t>(r)].points;
        if (it->row() < static_cast<int>(pts.size())) {
          if (it->column() == 0) pts[static_cast<size_t>(it->row())].first = val;
          else pts[static_cast<size_t>(it->row())].second = val;
          std::sort(pts.begin(), pts.end());
        }
      });
      showPoints();
    });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { showPoints(); });
    refresh();
  }

 private:
  void refresh() {
    list_->clear();
    for (const auto& r : c_->animation()->reactions) list_->addItem(pathLabel(c_, r.driver) + "  ->  " + pathLabel(c_, r.driven));
    showPoints();
  }
  void showPoints() {
    loading_ = true;
    points_->setRowCount(0);
    int r = list_->currentRow();
    if (r >= 0 && r < static_cast<int>(c_->animation()->reactions.size())) {
      const Reaction& rc = c_->animation()->reactions[static_cast<size_t>(r)];
      smooth_->setChecked(rc.smooth);
      for (const auto& p : rc.points) {
        int row = points_->rowCount();
        points_->insertRow(row);
        points_->setItem(row, 0, new QTableWidgetItem(QString::number(p.first)));
        points_->setItem(row, 1, new QTableWidgetItem(QString::number(p.second)));
      }
    }
    loading_ = false;
  }
  AnimController* c_;
  QListWidget* list_;
  QTableWidget* points_;
  QCheckBox* smooth_;
  bool loading_ = false;
};

// ---------------------------------------------------------------------------------------------
// Bone Tools

std::vector<Node*> selectedBones(AnimController* c) {
  std::vector<Node*> out;
  for (Node* n : c->document()->selectedNodes())
    if (n->object && n->object->type() == "Bone") out.push_back(n);
  return out;
}

class BoneTools : public QDialog {
 public:
  explicit BoneTools(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Bone Tools"));
    auto* v = new QVBoxLayout(this);
    auto* edit = new QGroupBox(tr("Bone Editing Tools"), this);
    auto* ev = new QHBoxLayout(edit);
    auto* createEnd = new QPushButton(tr("Create End"), edit);
    auto* remove = new QPushButton(tr("Remove Bone"), edit);
    auto* refine = new QPushButton(tr("Refine"), edit);
    auto* realign = new QPushButton(tr("Realign"), edit);
    auto* mirror = new QPushButton(tr("Mirror"), edit);
    for (QPushButton* b : {createEnd, remove, refine, realign, mirror}) ev->addWidget(b);
    v->addWidget(edit);
    auto* fins = new QGroupBox(tr("Fin Adjustment Tools"), this);
    auto* ff = new QFormLayout(fins);
    width_ = new QDoubleSpinBox(fins);
    height_ = new QDoubleSpinBox(fins);
    taper_ = new QDoubleSpinBox(fins);
    for (QDoubleSpinBox* s : {width_, height_}) s->setRange(0, 1e5);
    taper_->setRange(-100, 100);
    width_->setValue(4);
    height_->setValue(4);
    taper_->setValue(90);
    sideFins_ = new QCheckBox(tr("Side Fins"), fins);
    frontFin_ = new QCheckBox(tr("Front Fin"), fins);
    backFin_ = new QCheckBox(tr("Back Fin"), fins);
    ff->addRow(tr("Width:"), width_);
    ff->addRow(tr("Height:"), height_);
    ff->addRow(tr("Taper %:"), taper_);
    ff->addRow(sideFins_);
    ff->addRow(frontFin_);
    ff->addRow(backFin_);
    auto* apply = new QPushButton(tr("Apply to Selected Bones"), fins);
    ff->addRow(apply);
    v->addWidget(fins);
    auto* mirrorAxis = new QComboBox(this);
    mirrorAxis->addItems({tr("Mirror Axis: X"), tr("Mirror Axis: Y"), tr("Mirror Axis: Z")});
    v->addWidget(mirrorAxis);
    connect(apply, &QPushButton::clicked, this, [this] {
      auto bones = selectedBones(c_);
      if (bones.empty()) return;
      SceneDocument* doc = c_->document();
      doc->beginEdit(tr("Bone Fins"));
      QVector<uint32_t> ids;
      for (Node* b : bones) {
        ParamBlock& p = b->object->params;
        p.setNumber("width", width_->value());
        p.setNumber("height", height_->value());
        p.setNumber("taper", taper_->value());
        p.setNumber("sideFins", sideFins_->isChecked());
        p.setNumber("frontFin", frontFin_->isChecked());
        p.setNumber("backFin", backFin_->isChecked());
        b->object->touch();
        ids.push_back(b->id);
      }
      doc->notifyNodesChanged(ids, ChangeGeometry);
      doc->endEdit();
    });
    connect(createEnd, &QPushButton::clicked, this, [this] {
      auto bones = selectedBones(c_);
      SceneDocument* doc = c_->document();
      if (bones.empty()) return;
      doc->beginEdit(tr("Create End Bone"));
      QVector<uint32_t> made;
      for (Node* b : bones) {
        float len = b->object->params.getFloat("length");
        Node* nub = doc->createNode("Bone");
        nub->name = c_->scene().uniqueNodeName("Bone");
        float w = b->object->params.getFloat("width");
        nub->object->params.setNumber("length", std::max(w, 1.0f));
        nub->object->params.setNumber("width", w);
        nub->object->params.setNumber("height", b->object->params.getFloat("height"));
        nub->renderable = false;
        nub->wireColor = b->wireColor;
        c_->scene().setParent(nub->id, b->id);
        nub->transform = Transform();
        nub->transform.position = Vec3(len, 0, 0);
        made.push_back(nub->id);
      }
      doc->notifyStructureChanged();
      doc->setSelection(made);
      doc->endEdit();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
      auto bones = selectedBones(c_);
      SceneDocument* doc = c_->document();
      if (bones.empty()) return;
      doc->beginEdit(tr("Remove Bone"));
      for (Node* b : bones) {
        uint32_t parent = b->parentId;
        uint32_t id = b->id;
        // Children reattach to the removed bone's parent (keeping their placement).
        for (Node* ch : c_->scene().children(id)) c_->scene().setParent(ch->id, parent);
        if (Node* p = c_->scene().findNode(parent); p && p->object && p->object->type() == "Bone") {
          std::vector<Node*> kids = c_->scene().children(parent);
          if (!kids.empty()) {
            // Stretch the parent to reach its new first child.
            p->object->params.setNumber("length", std::max(0.01f, length(kids[0]->transform.position)));
            p->object->touch();
            p->invalidate();
          }
        }
        c_->scene().removeNode(id);
      }
      doc->notifyStructureChanged();
      doc->endEdit();
    });
    connect(refine, &QPushButton::clicked, this, [this] {
      auto bones = selectedBones(c_);
      SceneDocument* doc = c_->document();
      if (bones.empty()) return;
      doc->beginEdit(tr("Refine Bone"));
      for (Node* b : bones) {
        float len = b->object->params.getFloat("length");
        std::vector<Node*> kids = c_->scene().children(b->id);
        Node* half = doc->createNode("Bone");
        half->name = c_->scene().uniqueNodeName("Bone");
        half->object->params.fromJson(b->object->params.toJson());
        half->object->params.setNumber("length", len * 0.5f);
        half->renderable = b->renderable;
        half->wireColor = b->wireColor;
        c_->scene().setParent(half->id, b->id);
        half->transform = Transform();
        half->transform.position = Vec3(len * 0.5f, 0, 0);
        b->object->params.setNumber("length", len * 0.5f);
        b->object->touch();
        b->invalidate();
        for (Node* k : kids) c_->scene().setParent(k->id, half->id);
      }
      doc->notifyStructureChanged();
      doc->endEdit();
    });
    connect(realign, &QPushButton::clicked, this, [this] {
      auto bones = selectedBones(c_);
      SceneDocument* doc = c_->document();
      if (bones.empty()) return;
      doc->beginEdit(tr("Realign Bone"));
      QVector<uint32_t> ids;
      for (Node* b : bones) {
        std::vector<Node*> kids = c_->scene().children(b->id);
        if (kids.empty()) continue;
        Vec3 from = b->worldPosition(), to = kids[0]->worldPosition();
        if (length(to - from) < 1e-5f) continue;
        std::vector<std::pair<Node*, Mat4>> keep;
        for (Node* k : kids) keep.emplace_back(k, k->worldMatrix());
        Mat4 w = b->worldMatrix();
        Vec3 x = normalize(to - from);
        Vec3 z = normalize(w.column(2));
        Vec3 y = cross(z, x);
        if (length2(y) < 1e-8f) y = cross(Vec3(0, 0, 1), x);
        y = normalize(y);
        z = cross(x, y);
        Vec3 s(length(w.column(0)), length(w.column(1)), length(w.column(2)));
        Mat4 nw;
        for (int r = 0; r < 3; ++r) {
          nw.m[r][0] = x[r] * s.x;
          nw.m[r][1] = y[r] * s.y;
          nw.m[r][2] = z[r] * s.z;
          nw.m[r][3] = from[r];
        }
        b->setWorldMatrix(nw);
        b->object->params.setNumber("length", length(to - from) / std::max(1e-6f, s.x));
        b->object->touch();
        for (auto& [k, m] : keep) k->setWorldMatrix(m);
        ids.push_back(b->id);
      }
      doc->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
      doc->endEdit();
    });
    connect(mirror, &QPushButton::clicked, this, [this, mirrorAxis] {
      auto bones = selectedBones(c_);
      SceneDocument* doc = c_->document();
      if (bones.empty()) return;
      int axis = mirrorAxis->currentIndex();
      doc->beginEdit(tr("Mirror Bones"));
      std::map<uint32_t, uint32_t> copies;
      QVector<uint32_t> made;
      for (Node* b : bones) {
        Node* m = c_->scene().addNode(b->clone());
        copies[b->id] = m->id;
        made.push_back(m->id);
      }
      for (Node* b : bones) {
        Node* m = c_->scene().findNode(copies[b->id]);
        uint32_t parent = copies.count(b->parentId) ? copies[b->parentId] : b->parentId;
        Mat4 w = b->worldMatrix();
        // Reflect the position; keep a right-handed frame by flipping the two other axes.
        Mat4 refl;
        refl.m[axis][axis] = -1.0f;
        Mat4 nw = refl * w;
        int a1 = (axis + 1) % 3;
        for (int r = 0; r < 3; ++r) nw.m[r][a1] = -nw.m[r][a1];
        m->parentId = 0;
        m->setWorldMatrix(nw);
        if (parent) c_->scene().setParent(m->id, parent);
        m->name = c_->scene().uniqueNodeName(b->name + "_Mirror");
      }
      doc->notifyStructureChanged();
      doc->setSelection(made);
      doc->endEdit();
    });
  }

 private:
  AnimController* c_;
  QDoubleSpinBox *width_, *height_, *taper_;
  QCheckBox *sideFins_, *frontFin_, *backFin_;
};

// ---------------------------------------------------------------------------------------------
// Walkthrough Assistant

class WalkthroughDialog : public QDialog {
 public:
  explicit WalkthroughDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Walkthrough Assistant"));
    auto* f = new QFormLayout(this);
    path_ = new QComboBox(this);
    for (const auto& up : c->scene().nodes)
      if (up->kind() == ObjectKind::Shape) path_->addItem(QString::fromStdString(up->name), up->id);
    eye_ = new QDoubleSpinBox(this);
    eye_->setRange(-1e5, 1e5);
    eye_->setValue(160);
    tilt_ = new QDoubleSpinBox(this);
    tilt_->setRange(-89, 89);
    turn_ = new QDoubleSpinBox(this);
    turn_->setRange(-180, 180);
    start_ = new QSpinBox(this);
    end_ = new QSpinBox(this);
    for (QSpinBox* s : {start_, end_}) s->setRange(-100000, 100000);
    start_->setValue(c->startFrame());
    end_->setValue(c->endFrame());
    constantSpeed_ = new QCheckBox(tr("Constant Speed"), this);
    constantSpeed_->setChecked(true);
    f->addRow(tr("Path:"), path_);
    f->addRow(tr("Eye Level:"), eye_);
    f->addRow(tr("Head Tilt Angle:"), tilt_);
    f->addRow(tr("Turn Head:"), turn_);
    f->addRow(tr("Start Frame:"), start_);
    f->addRow(tr("End Frame:"), end_);
    f->addRow(constantSpeed_);
    auto* bb = new QDialogButtonBox(this);
    QPushButton* create = bb->addButton(tr("Create New Camera"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Close);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(create, &QPushButton::clicked, this, [this] {
      if (path_->count() == 0) {
        QMessageBox::information(this, windowTitle(), tr("Create a path (a shape) first."));
        return;
      }
      createCamera();
      accept();
    });
  }

  void createCamera() {
    uint32_t pathId = path_->currentData().toUInt();
    SceneDocument* doc = c_->document();
    doc->beginEdit(tr("Walkthrough Camera"));
    Node* cam = doc->createNode("Camera");
    uint32_t id = cam ? cam->id : 0;
    if (cam) cam->name = c_->scene().uniqueNodeName("Walkthrough_Cam");
    doc->notifyStructureChanged();
    doc->endEdit();
    if (!id) return;
    double s0 = start_->value(), s1 = std::max(start_->value() + 1, end_->value());
    c_->editAnimation(tr("Walkthrough Camera"), [&] {
      Animation* a = c_->animation();
      Constraint con;
      con.type = ConstraintType::Path;
      con.targets.push_back(ConstraintTarget{pathId});
      con.follow = true;
      con.axis = 2;
      con.flip = true;
      con.constantVelocity = constantSpeed_->isChecked();
      con.keepOffset = true;
      con.positionOffset = Vec3(0, 0, static_cast<float>(eye_->value()));
      con.rotationOffset = Quat::fromAxisAngle(Vec3(0, 1, 0), radians(static_cast<float>(-turn_->value()))) *
                           Quat::fromAxisAngle(Vec3(1, 0, 0), radians(static_cast<float>(tilt_->value())));
      a->nodes[id].constraints.push_back(con);
      Track& t = a->tracks[Animation::nodePath(id, "c:0/percent")];
      t.setKey(s0, 0.0);
      t.setKey(s1, 100.0);
      for (auto& k : t.keys) k.inType = k.outType = TangentType::Linear;
    });
    c_->document()->setSelection({id});
    created = id;
  }
  uint32_t created = 0;

 private:
  AnimController* c_;
  QComboBox* path_;
  QDoubleSpinBox *eye_, *tilt_, *turn_;
  QSpinBox *start_, *end_;
  QCheckBox* constantSpeed_;
};

// ---------------------------------------------------------------------------------------------
// Rigid body simulation

class SimulateDialog : public QDialog {
 public:
  explicit SimulateDialog(AnimController* c, QWidget* parent) : QDialog(parent), c_(c) {
    setWindowTitle(tr("Simulate and Bake Rigid Bodies"));
    auto* f = new QFormLayout(this);
    start_ = new QSpinBox(this);
    end_ = new QSpinBox(this);
    for (QSpinBox* s : {start_, end_}) s->setRange(-100000, 100000);
    start_->setValue(c->startFrame());
    end_->setValue(c->endFrame());
    substeps_ = new QSpinBox(this);
    substeps_->setRange(1, 64);
    substeps_->setValue(4);
    iterations_ = new QSpinBox(this);
    iterations_->setRange(1, 200);
    iterations_->setValue(12);
    gravity_ = new QDoubleSpinBox(this);
    gravity_->setRange(-1e6, 1e6);
    gravity_->setValue(-980);
    ground_ = new QCheckBox(tr("Use Ground Collision (Z = 0)"), this);
    ground_->setChecked(true);
    groundBounce_ = new QDoubleSpinBox(this);
    groundBounce_->setRange(0, 1);
    groundBounce_->setSingleStep(0.05);
    groundBounce_->setValue(0.5);
    groundFriction_ = new QDoubleSpinBox(this);
    groundFriction_->setRange(0, 2);
    groundFriction_->setSingleStep(0.05);
    groundFriction_->setValue(0.5);
    f->addRow(tr("Start Frame:"), start_);
    f->addRow(tr("End Frame:"), end_);
    f->addRow(tr("Substeps per Frame:"), substeps_);
    f->addRow(tr("Solver Iterations:"), iterations_);
    f->addRow(tr("Gravity Z (units/s²):"), gravity_);
    f->addRow(ground_);
    f->addRow(tr("Ground Bounciness:"), groundBounce_);
    f->addRow(tr("Ground Friction:"), groundFriction_);
    auto* bb = new QDialogButtonBox(this);
    QPushButton* bake = bb->addButton(tr("Simulate and Bake"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    f->addRow(bb);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(bake, &QPushButton::clicked, this, [this] {
      run();
      accept();
    });
  }

  void run() {
    RigidWorldSettings s;
    s.gravity = Vec3(0, 0, static_cast<float>(gravity_->value()));
    s.groundPlane = ground_->isChecked();
    s.groundBounce = static_cast<float>(groundBounce_->value());
    s.groundFriction = static_cast<float>(groundFriction_->value());
    s.substeps = substeps_->value();
    s.iterations = iterations_->value();
    s.fps = c_->fps();
    int a = start_->value(), b = std::max(start_->value() + 1, end_->value());
    QProgressDialog progress(tr("Simulating rigid bodies..."), tr("Cancel"), a, b, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    BakeResult r;
    c_->editAnimation(tr("Simulate and Bake"), [&] {
      r = bakeRigidBodies(c_->scene(), a, b, s, [&](int f) {
        progress.setValue(f);
        QApplication::processEvents();
        return !progress.wasCanceled();
      });
    });
    progress.setValue(b);
    if (r.bodies == 0)
      QMessageBox::information(this, windowTitle(), tr("No dynamic rigid bodies. Add a Rigid Body modifier to objects first."));
  }

 private:
  AnimController* c_;
  QSpinBox *start_, *end_, *substeps_, *iterations_;
  QDoubleSpinBox *gravity_, *groundBounce_, *groundFriction_;
  QCheckBox* ground_;
};

void addRigidBody(AnimController* c, int bodyType) {
  SceneDocument* doc = c->document();
  QVector<uint32_t> ids;
  for (Node* n : doc->selectedNodes())
    if (n->kind() == ObjectKind::Geometry) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  doc->beginEdit(QObject::tr("Rigid Body"));
  for (uint32_t id : ids) {
    Node* n = c->scene().findNode(id);
    std::shared_ptr<Modifier> existing;
    for (auto& m : n->modifiers)
      if (m && m->type() == "RigidBody") existing = m;
    if (!existing) {
      existing = std::make_shared<RigidBodyModifier>();
      n->modifiers.push_back(existing);
    }
    existing->params.setNumber("bodyType", bodyType);
    existing->touch();
  }
  doc->notifyNodesChanged(ids, ChangeGeometry);
  doc->endEdit();
}

// ---------------------------------------------------------------------------------------------

template <class D>
void showModeless(QPointer<D>& ptr, AnimController* c, MainWindow& w) {
  if (!ptr) {
    ptr = new D(c, &w);
    ptr->setAttribute(Qt::WA_DeleteOnClose);
  }
  ptr->show();
  ptr->raise();
  ptr->activateWindow();
}

QPointer<ParameterEditor> g_paramEditor;
QPointer<ParameterCollector> g_collector;
QPointer<WireDialog> g_wire;
QPointer<LayersDialog> g_layers;
QPointer<ReactionDialog> g_reaction;
QPointer<BoneTools> g_boneTools;

void installAnimationMenu(MainWindow& w) {
  AnimController* c = AnimController::instance();
  if (!c) return;
  QMenu* m = w.menuAt("Animation");
  auto cmd = [&](QMenu* menu, const QString& id, const QString& text, const QKeySequence& key, std::function<void()> fn) {
    QAction* a = menu->addAction(text);
    if (!key.isEmpty()) {
      a->setShortcut(key);
      w.addAction(a);
    }
    QObject::connect(a, &QAction::triggered, &w, std::move(fn));
    if (!id.isEmpty()) w.registerCommand(id, a);
    return a;
  };

  cmd(m, "anim.loadAnimation", QObject::tr("Load Animation..."), {}, [&w, c] {
    QString f = QFileDialog::getOpenFileName(&w, QObject::tr("Load Animation"), QString(), QObject::tr("Animation clips (*.vanim *.json)"));
    if (f.isEmpty()) return;
    std::string err;
    int n = loadAnimationClip(c, f.toStdString(), 0.0, &err);
    if (n < 0) QMessageBox::warning(&w, QObject::tr("Load Animation"), QString::fromStdString(err));
    else if (n == 0) QMessageBox::information(&w, QObject::tr("Load Animation"), QObject::tr("No objects with matching names."));
  });
  cmd(m, "anim.saveAnimation", QObject::tr("Save Animation..."), {}, [&w, c] {
    QVector<uint32_t> sel = c->document()->selection();
    if (sel.isEmpty()) {
      QMessageBox::information(&w, QObject::tr("Save Animation"), QObject::tr("Select the animated objects to save."));
      return;
    }
    QString f = QFileDialog::getSaveFileName(&w, QObject::tr("Save Animation"), "animation.vanim", QObject::tr("Animation clips (*.vanim)"));
    if (f.isEmpty()) return;
    std::string err;
    if (!saveAnimationClip(c, std::vector<uint32_t>(sel.begin(), sel.end()), f.toStdString(), &err))
      QMessageBox::warning(&w, QObject::tr("Save Animation"), QString::fromStdString(err));
  });
  m->addSeparator();

  QMenu* ik = m->addMenu(QObject::tr("IK Solvers"));
  auto ikCmd = [&](const QString& id, const QString& text, int solver) {
    cmd(ik, id, text, {}, [&w, c, solver] {
      Node* start = c->document()->firstSelected();
      if (!start) {
        QMessageBox::information(&w, QObject::tr("IK Solver"), QObject::tr("Select the start joint (bone) of the chain first."));
        return;
      }
      uint32_t sid = start->id;
      startPickNode(w, QObject::tr("IK Solver: pick the end joint of the chain"),
                    [c, sid](const Node& n) { return n.id != sid && c->scene().isAncestor(sid, n.id); },
                    [c, sid, solver](uint32_t end) { createIkChain(c, sid, end, solver); });
    });
  };
  ikCmd("anim.ikHI", QObject::tr("HI Solver"), 0);
  ikCmd("anim.ikLimb", QObject::tr("IK Limb Solver"), 1);

  QMenu* cons = m->addMenu(QObject::tr("Constraints"));
  const std::pair<const char*, ConstraintType> constraints[] = {
      {"Attachment Constraint", ConstraintType::Attachment}, {"Surface Constraint", ConstraintType::Surface},
      {"Path Constraint", ConstraintType::Path},             {"Position Constraint", ConstraintType::Position},
      {"Link Constraint", ConstraintType::Link},             {"LookAt Constraint", ConstraintType::LookAt},
      {"Orientation Constraint", ConstraintType::Orientation}};
  for (const auto& [name, type] : constraints) {
    ConstraintType t = type;
    cmd(cons, QStringLiteral("anim.constraint.%1").arg(static_cast<int>(t)), QObject::tr(name), {}, [&w, t] { beginConstraintPick(w, t); });
  }

  auto floatMenu = [&](QMenu* parent, const QString& title, int group) {
    QMenu* sub = parent->addMenu(title);
    const std::pair<const char*, ControllerType> types[] = {{"Bezier", ControllerType::Bezier},       {"Linear", ControllerType::Linear},
                                                            {"TCB", ControllerType::TCB},             {"Noise", ControllerType::Noise},
                                                            {"Expression", ControllerType::Expression}, {"List", ControllerType::List}};
    for (const auto& [name, type] : types) {
      ControllerType t = type;
      sub->addAction(QObject::tr(name), &w, [c, t, group] {
        std::vector<std::string> paths;
        for (Node* n : c->document()->selectedNodes())
          for (int k = 0; k < 3; ++k) paths.push_back(Animation::nodePath(n->id, Animation::transformChannels()[static_cast<size_t>(group * 3 + k)]));
        if (group == 1)
          for (Node* n : c->document()->selectedNodes()) setRotationController(c, n->id, 0);
        assignFloatController(c, paths, t);
      });
    }
    return sub;
  };
  QMenu* tc = m->addMenu(QObject::tr("Transform Controllers"));
  tc->addAction(QObject::tr("Link Constraint"), &w, [&w] { beginConstraintPick(w, ConstraintType::Link); });
  tc->addAction(QObject::tr("Position/Rotation/Scale"), &w, [c] {
    for (Node* n : c->document()->selectedNodes()) {
      uint32_t id = n->id;
      c->editAnimation(QObject::tr("Position/Rotation/Scale"), [&] {
        c->animation()->nodes[id].constraints.clear();
        c->animation()->nodes[id].rotationController = 0;
      });
    }
  });
  floatMenu(m, QObject::tr("Position Controllers"), 0);
  QMenu* rot = floatMenu(m, QObject::tr("Rotation Controllers"), 1);
  rot->addSeparator();
  rot->addAction(QObject::tr("Euler XYZ"), &w, [c] {
    for (Node* n : c->document()->selectedNodes()) setRotationController(c, n->id, 0);
  });
  rot->addAction(QObject::tr("TCB Rotation"), &w, [c] {
    for (Node* n : c->document()->selectedNodes()) setRotationController(c, n->id, 1);
  });
  floatMenu(m, QObject::tr("Scale Controllers"), 2);
  m->addSeparator();

  cmd(m, "anim.parameterEditor", QObject::tr("Parameter Editor..."), QKeySequence("Alt+1"), [&w, c] { showModeless(g_paramEditor, c, w); });
  cmd(m, "anim.parameterCollector", QObject::tr("Parameter Collector..."), QKeySequence("Alt+2"), [&w, c] { showModeless(g_collector, c, w); });
  cmd(m, "anim.wireParameters", QObject::tr("Wire Parameters..."), QKeySequence("Ctrl+5"), [&w, c] { showModeless(g_wire, c, w); });
  cmd(m, "anim.layers", QObject::tr("Animation Layers..."), {}, [&w, c] { showModeless(g_layers, c, w); });
  cmd(m, "anim.reactionManager", QObject::tr("Reaction Manager..."), {}, [&w, c] { showModeless(g_reaction, c, w); });
  m->addSeparator();
  cmd(m, "anim.boneTools", QObject::tr("Bone Tools..."), {}, [&w, c] { showModeless(g_boneTools, c, w); });
  cmd(m, "anim.setSkinPose", QObject::tr("Set as Skin Pose"), {}, [c] {
    SceneDocument* doc = c->document();
    if (doc->selection().isEmpty()) return;
    doc->beginEdit(QObject::tr("Set as Skin Pose"));
    json& poses = c->scene().extensions["skinPose"];
    if (!poses.is_object()) poses = json::object();
    for (Node* n : doc->selectedNodes()) poses[std::to_string(n->id)] = n->transform.toJson();
    doc->endEdit();
  });
  cmd(m, "anim.assumeSkinPose", QObject::tr("Assume Skin Pose"), {}, [c] {
    SceneDocument* doc = c->document();
    const json& poses = c->scene().extensions.contains("skinPose") ? c->scene().extensions["skinPose"] : json();
    if (!poses.is_object()) return;
    doc->beginEdit(QObject::tr("Assume Skin Pose"));
    QVector<uint32_t> ids;
    for (Node* n : doc->selectedNodes()) {
      auto it = poses.find(std::to_string(n->id));
      if (it == poses.end()) continue;
      n->transform.fromJson(*it);
      ids.push_back(n->id);
    }
    doc->notifyNodesChanged(ids, ChangeTransform);
    doc->endEdit();
  });
  m->addSeparator();
  QMenu* sim = m->addMenu(QObject::tr("Simulation - Rigid Bodies"));
  sim->addAction(QObject::tr("Set Selected as Dynamic Rigid Body"), &w, [c] { addRigidBody(c, 0); });
  sim->addAction(QObject::tr("Set Selected as Kinematic Rigid Body"), &w, [c] { addRigidBody(c, 1); });
  sim->addAction(QObject::tr("Set Selected as Static Rigid Body"), &w, [c] { addRigidBody(c, 2); });
  sim->addSeparator();
  QAction* bake = sim->addAction(QObject::tr("Simulate and Bake..."), &w, [&w, c] {
    SimulateDialog d(c, &w);
    d.exec();
  });
  w.registerCommand("anim.simulate", bake);
  sim->addAction(QObject::tr("Unbake All"), &w, [c] {
    c->editAnimation(QObject::tr("Unbake All"), [&] { unbakeRigidBodies(c->scene()); });
  });
  m->addSeparator();
  cmd(m, "anim.walkthrough", QObject::tr("Walkthrough Assistant..."), {}, [&w, c] {
    WalkthroughDialog d(c, &w);
    d.exec();
  });
  m->addSeparator();
  cmd(m, "anim.deleteSelectedAnimation", QObject::tr("Delete Selected Animation"), {}, [c] {
    QVector<uint32_t> ids = c->document()->selection();
    if (ids.isEmpty()) return;
    c->editAnimation(QObject::tr("Delete Selected Animation"), [&] {
      for (uint32_t id : ids) c->animation()->deleteNodeAnimation(id);
    });
  });
}

}  // namespace

VERAA_APP_EXTENSION(animationMenu, 305, installAnimationMenu);

}  // namespace veraa
