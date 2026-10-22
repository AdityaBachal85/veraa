// Modify panel rollouts for Editable Poly, Edit Poly, Poly Select and Mesh Select: selection
// levels, soft selection, the per-level editing buttons with their settings caddies, Edit
// Geometry, material IDs and smoothing groups. Also installs the sub-object hotkeys, the
// Collapse Stack command and the sub-object entries of the viewport context menu.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include "commandpanel.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "object.h"
#include "paramform.h"
#include "polyedit.h"
#include "polymods.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

MainWindow*& mainWindow() {
  static MainWindow* w = nullptr;
  return w;
}

// ---------------------------------------------------------------------------------------------
// Level icons (drawn, so no image files are needed)

QIcon levelIcon(int level) {
  QPixmap pm(28, 28);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QColor line(210, 210, 210), red(255, 70, 60);
  p.setPen(QPen(line, 1.4));
  switch (level) {
    case PolyVertex:
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
          bool sel = i == 1 && j == 1;
          p.fillRect(QRectF(5 + i * 8, 5 + j * 8, sel ? 4 : 3, sel ? 4 : 3), sel ? red : line);
        }
      break;
    case PolyEdge:
      p.drawPolygon(QPolygonF({QPointF(4, 23), QPointF(24, 23), QPointF(14, 5)}));
      p.setPen(QPen(red, 2.4));
      p.drawLine(QPointF(4, 23), QPointF(14, 5));
      break;
    case PolyBorder:
      p.drawRect(QRectF(5, 5, 18, 18));
      p.setPen(QPen(red, 2.4));
      p.drawPolyline(QPolygonF({QPointF(5, 5), QPointF(5, 23), QPointF(23, 23), QPointF(23, 5)}));
      break;
    case PolyFace:
      p.setBrush(red);
      p.drawRect(QRectF(5, 5, 18, 18));
      break;
    case PolyElement:
      p.setBrush(red.darker(130));
      p.drawPolygon(QPolygonF({QPointF(4, 9), QPointF(14, 4), QPointF(24, 9), QPointF(24, 21), QPointF(14, 26),
                               QPointF(4, 21)}));
      p.drawLine(QPointF(4, 9), QPointF(14, 14));
      p.drawLine(QPointF(24, 9), QPointF(14, 14));
      p.drawLine(QPointF(14, 14), QPointF(14, 26));
      break;
    default: break;
  }
  return QIcon(pm);
}

const char* levelName(int l) {
  static const char* names[] = {"Object", "Vertex", "Edge", "Border", "Polygon", "Element"};
  return names[std::clamp(l, 0, 5)];
}

// ---------------------------------------------------------------------------------------------
// Operation settings ("caddies")

struct CaddyField {
  enum Type { Float, Int, Bool, Enum };
  QString key;
  QString label;
  Type type = Float;
  double minV = 0, maxV = 1e5, step = 1, def = 0;
  QStringList items;
};

struct OpSpec {
  QString key;    // button id, e.g. "chamferBorders"
  QString op;     // runPolyOp name, e.g. "chamferEdges"
  QString title;  // undo label
  std::vector<CaddyField> fields;
};

const std::vector<OpSpec>& opSpecs() {
  using F = CaddyField;
  static const std::vector<OpSpec> specs = {
      {"extrudeVerts", "extrudeVerts", "Extrude Vertices",
       {{"height", "Height", F::Float, -1e5, 1e5, 1, 10}, {"width", "Width", F::Float, 0.01, 1e5, 0.5, 4}}},
      {"weldVerts", "weldVerts", "Weld Vertices", {{"threshold", "Weld Threshold", F::Float, 0, 1e5, 0.1, 0.1}}},
      {"chamferVerts", "chamferVerts", "Chamfer Vertices",
       {{"amount", "Amount", F::Float, 0.001, 1e5, 0.5, 5}, {"open", "Open", F::Bool, 0, 1, 1, 0}}},
      {"extrudeEdges", "extrudeEdges", "Extrude Edges",
       {{"height", "Height", F::Float, -1e5, 1e5, 1, 10}, {"width", "Width", F::Float, 0.01, 1e5, 0.5, 4}}},
      {"weldEdges", "weldEdges", "Weld Edges", {{"threshold", "Weld Threshold", F::Float, 0, 1e5, 0.1, 0.1}}},
      {"chamferEdges", "chamferEdges", "Chamfer Edges",
       {{"amount", "Amount", F::Float, 0.001, 1e5, 0.5, 5}, {"open", "Open", F::Bool, 0, 1, 1, 0}}},
      {"bridgeEdges", "bridgeEdges", "Bridge Edges", {{"segments", "Segments", F::Int, 1, 100, 1, 1}}},
      {"connectEdges", "connectEdges", "Connect Edges",
       {{"segments", "Segments", F::Int, 1, 100, 1, 1},
        {"pinch", "Pinch", F::Int, -100, 100, 1, 0},
        {"slide", "Slide", F::Int, -100, 100, 1, 0}}},
      {"extrudeBorders", "extrudeBorders", "Extrude Borders",
       {{"height", "Height", F::Float, -1e5, 1e5, 1, 10}, {"width", "Width", F::Float, -1e5, 1e5, 0.5, 0}}},
      {"chamferBorders", "chamferEdges", "Chamfer Borders",
       {{"amount", "Amount", F::Float, 0.001, 1e5, 0.5, 5}, {"open", "Open", F::Bool, 0, 1, 1, 0}}},
      {"bridgeBorders", "bridgeBorders", "Bridge Borders", {{"segments", "Segments", F::Int, 1, 100, 1, 1}}},
      {"connectBorders", "connectEdges", "Connect Borders",
       {{"segments", "Segments", F::Int, 1, 100, 1, 1},
        {"pinch", "Pinch", F::Int, -100, 100, 1, 0},
        {"slide", "Slide", F::Int, -100, 100, 1, 0}}},
      {"extrudeFaces", "extrudeFaces", "Extrude Polygons",
       {{"mode", "Type", F::Enum, 0, 2, 1, 0, {"Group", "Local Normal", "By Polygon"}},
        {"height", "Height", F::Float, -1e5, 1e5, 1, 10}}},
      {"outlineFaces", "outlineFaces", "Outline Polygons", {{"amount", "Amount", F::Float, -1e5, 1e5, 0.5, 2}}},
      {"bevelFaces", "bevelFaces", "Bevel Polygons",
       {{"mode", "Type", F::Enum, 0, 2, 1, 0, {"Group", "Local Normal", "By Polygon"}},
        {"height", "Height", F::Float, -1e5, 1e5, 1, 10},
        {"outline", "Outline", F::Float, -1e5, 1e5, 0.5, -2}}},
      {"insetFaces", "insetFaces", "Inset Polygons",
       {{"byPolygon", "By Polygon", F::Bool, 0, 1, 1, 0}, {"amount", "Amount", F::Float, 0, 1e5, 0.5, 5}}},
      {"bridgeFaces", "bridgeFaces", "Bridge Polygons", {{"segments", "Segments", F::Int, 1, 100, 1, 1}}},
      {"hingeFaces", "hingeFaces", "Hinge From Edge",
       {{"angle", "Angle", F::Float, -360, 360, 5, 90}, {"segments", "Segments", F::Int, 1, 100, 1, 1}}},
      {"msmooth", "msmooth", "MSmooth", {{"iterations", "Iterations", F::Int, 1, 4, 1, 1}}},
      {"tessellate", "tessellate", "Tessellate", {{"iterations", "Iterations", F::Int, 1, 4, 1, 1}}},
      {"relax", "relax", "Relax",
       {{"amount", "Amount", F::Float, -1, 1, 0.05, 0.5},
        {"iterations", "Iterations", F::Int, 1, 100, 1, 1},
        {"keepBoundary", "Hold Boundary Points", F::Bool, 0, 1, 1, 1}}},
  };
  return specs;
}

const OpSpec* findSpec(const QString& key) {
  for (const auto& s : opSpecs())
    if (s.key == key) return &s;
  return nullptr;
}

// Last used caddy values per operation key.
std::map<QString, json>& caddyMemory() {
  static std::map<QString, json> m;
  return m;
}

json opWithValues(const OpSpec& spec, const json& extra = json::object()) {
  json op = {{"op", spec.op.toStdString()}};
  const json& mem = caddyMemory()[spec.key];
  for (const auto& f : spec.fields) {
    std::string k = f.key.toStdString();
    if (mem.contains(k)) op[k] = mem[k];
    else if (f.type == CaddyField::Bool) op[k] = f.def != 0;
    else if (f.type == CaddyField::Float) op[k] = f.def;
    else op[k] = static_cast<int>(f.def);
  }
  for (auto it = extra.begin(); it != extra.end(); ++it) op[it.key()] = it.value();
  return op;
}

class OpCaddy;
QPointer<OpCaddy>& currentCaddy() {
  static QPointer<OpCaddy> c;
  return c;
}

class OpCaddy : public QFrame {
 public:
  OpCaddy(const PolyTarget& target, const OpSpec& spec, const json& extra, QWidget* anchor)
      : QFrame(mainWindow(), Qt::Tool | Qt::FramelessWindowHint), target_(target), spec_(spec), extra_(extra) {
    setObjectName(QString("caddy_%1").arg(spec.key));
    setFrameShape(QFrame::Box);
    setStyleSheet("QFrame#" + objectName() + " { background: #3c3c3c; border: 1px solid #6fa8e0; }");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 6);
    auto* title = new QLabel(spec.title, this);
    title->setStyleSheet("font-weight: bold; color: #e8e8e8;");
    v->addWidget(title);
    auto* form = new QFormLayout();
    form->setVerticalSpacing(3);
    const json& mem = caddyMemory()[spec.key];
    for (const auto& f : spec.fields) {
      std::string k = f.key.toStdString();
      QWidget* w = nullptr;
      if (f.type == CaddyField::Float) {
        auto* s = new QDoubleSpinBox(this);
        s->setRange(f.minV, f.maxV);
        s->setSingleStep(f.step);
        s->setDecimals(3);
        s->setValue(mem.contains(k) ? mem[k].get<double>() : f.def);
        s->setKeyboardTracking(false);
        installSpinnerScrub(s);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { preview(); });
        w = s;
      } else if (f.type == CaddyField::Int) {
        auto* s = new QSpinBox(this);
        s->setRange(static_cast<int>(f.minV), static_cast<int>(f.maxV));
        s->setValue(mem.contains(k) ? mem[k].get<int>() : static_cast<int>(f.def));
        s->setKeyboardTracking(false);
        installSpinnerScrub(s);
        connect(s, &QSpinBox::valueChanged, this, [this] { preview(); });
        w = s;
      } else if (f.type == CaddyField::Bool) {
        auto* c = new QCheckBox(this);
        c->setChecked(mem.contains(k) ? mem[k].get<bool>() : f.def != 0);
        connect(c, &QCheckBox::toggled, this, [this] { preview(); });
        w = c;
      } else {
        auto* c = new QComboBox(this);
        c->addItems(f.items);
        c->setCurrentIndex(mem.contains(k) ? mem[k].get<int>() : static_cast<int>(f.def));
        connect(c, &QComboBox::currentIndexChanged, this, [this] { preview(); });
        w = c;
      }
      w->setObjectName(QString("caddy_%1_%2").arg(spec.key, f.key));
      editors_.push_back(w);
      form->addRow(f.label, w);
    }
    v->addLayout(form);
    if (spec.key == "hingeFaces") {
      auto* pick = new QPushButton(tr("Pick Hinge"), this);
      pick->setObjectName("caddy_hingeFaces_pick");
      connect(pick, &QPushButton::clicked, this, [this] {
        auto tool = activePolyTool(target_.state());
        if (!tool) return;
        QPointer<OpCaddy> self(this);
        tool->onHingePicked = [self](int a, int b) {
          if (!self) return;
          self->extra_["a"] = a;
          self->extra_["b"] = b;
          self->preview();
        };
        tool->setPickMode(PolyPickMode::HingePick);
      });
      v->addWidget(pick);
    }
    auto* row = new QHBoxLayout();
    auto mk = [&](const QString& text, const QString& tip, const QString& name) {
      auto* b = new QPushButton(text, this);
      b->setToolTip(tip);
      b->setObjectName(name);
      b->setFixedWidth(42);
      row->addWidget(b);
      return b;
    };
    QPushButton* ok = mk(QString::fromUtf8("✔"), tr("OK"), "caddy_ok");
    QPushButton* apply = mk("+", tr("Apply and continue"), "caddy_apply");
    QPushButton* cancel = mk(QString::fromUtf8("✖"), tr("Cancel"), "caddy_cancel");
    v->addLayout(row);
    connect(ok, &QPushButton::clicked, this, [this] { finish(true); });
    connect(apply, &QPushButton::clicked, this, [this] { applyAndContinue(); });
    connect(cancel, &QPushButton::clicked, this, [this] { finish(false); });
    // The scene being replaced (undo/redo, file open) drops the preview.
    connect(target.doc(), &SceneDocument::structureChanged, this, [this] {
      if (done_) return;
      done_ = true;
      abandonPolyLive();
      close();
      deleteLater();
    });

    if (currentCaddy()) currentCaddy()->finish(true);
    currentCaddy() = this;
    target_.beginLive(spec.title);
    preview();
    adjustSize();
    if (anchor) {
      QPoint g = anchor->mapToGlobal(QPoint(0, 0));
      move(g.x() - width() - 12, g.y());
    }
    show();
    raise();
  }

  json values() const {
    json op = {{"op", spec_.op.toStdString()}};
    for (size_t i = 0; i < spec_.fields.size(); ++i) {
      const auto& f = spec_.fields[i];
      std::string k = f.key.toStdString();
      QWidget* w = editors_[i];
      if (auto* d = qobject_cast<QDoubleSpinBox*>(w)) op[k] = d->value();
      else if (auto* s = qobject_cast<QSpinBox*>(w)) op[k] = s->value();
      else if (auto* c = qobject_cast<QCheckBox*>(w)) op[k] = c->isChecked();
      else if (auto* cb = qobject_cast<QComboBox*>(w)) op[k] = cb->currentIndex();
    }
    for (auto it = extra_.begin(); it != extra_.end(); ++it) op[it.key()] = it.value();
    return op;
  }

  void remember() {
    json mem = json::object();
    json op = values();
    for (const auto& f : spec_.fields) mem[f.key.toStdString()] = op[f.key.toStdString()];
    caddyMemory()[spec_.key] = mem;
  }

  void preview() {
    if (done_) return;
    target_.updateLive(values());
  }

  void finish(bool commit) {
    if (done_) return;
    done_ = true;
    if (commit) remember();
    target_.endLive(commit);
    close();
    deleteLater();
  }

  void applyAndContinue() {
    if (done_) return;
    remember();
    target_.endLive(true);
    target_.beginLive(spec_.title);
    preview();
  }

 protected:
  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Escape) finish(false);
    else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) finish(true);
    else QFrame::keyPressEvent(e);
  }

 private:
  PolyTarget target_;
  OpSpec spec_;
  json extra_;
  std::vector<QWidget*> editors_;
  bool done_ = false;
};

// ---------------------------------------------------------------------------------------------
// Soft selection curve preview

class SoftCurve : public QWidget {
 public:
  explicit SoftCurve(QWidget* parent) : QWidget(parent) { setFixedHeight(54); }
  void setSoft(const SoftSelection& s) {
    soft_ = s;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(45, 45, 45));
    p.setPen(QColor(90, 90, 90));
    p.drawLine(QPointF(width() * 0.5, 4), QPointF(width() * 0.5, height() - 4));
    QPolygonF poly;
    for (int i = 0; i <= 80; ++i) {
      float x = -1.0f + 2.0f * i / 80.0f;
      float t = std::fabs(x);
      float base = 0.5f * (1.0f + std::cos(kPi * t));
      float y = std::pow(base, 1.0f + std::max(0.0f, soft_.pinch) * 2.0f);
      if (soft_.pinch < 0.0f) y = std::pow(base, 1.0f / (1.0f - soft_.pinch * 2.0f));
      y += soft_.bubble * 0.5f * std::sin(kPi * (1.0f - t)) * (1.0f - y);
      y = clampf(y, 0.0f, 1.0f);
      poly << QPointF(4 + (width() - 8) * (x + 1) * 0.5, height() - 4 - (height() - 8) * y);
    }
    p.setPen(QPen(soft_.enabled ? QColor(255, 170, 60) : QColor(130, 130, 130), 1.5));
    p.drawPolyline(poly);
  }

 private:
  SoftSelection soft_;
};

// ---------------------------------------------------------------------------------------------
// Dialogs

class DetachDialog : public QDialog {
 public:
  DetachDialog(const QString& name, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Detach"));
    auto* f = new QFormLayout(this);
    name_ = new QLineEdit(name, this);
    f->addRow(tr("Detach as:"), name_);
    toElement_ = new QCheckBox(tr("Detach To Element"), this);
    asClone_ = new QCheckBox(tr("Detach As Clone"), this);
    f->addRow(toElement_);
    f->addRow(asClone_);
    connect(toElement_, &QCheckBox::toggled, name_, &QWidget::setDisabled);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
  }
  QLineEdit* name_;
  QCheckBox* toElement_;
  QCheckBox* asClone_;
};

// ---------------------------------------------------------------------------------------------
// The panel

QPointer<class PolyPanel>& currentPanel();

class PolyPanel : public QWidget {
 public:
  PolyPanel(const ModifyPanelContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setObjectName("PolyPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);
    PolyTarget t = target();
    bool edit = t.canEdit();

    buildSelection(v);
    buildSoft(v);
    if (edit) {
      buildEditVertices(v);
      buildEditEdges(v);
      buildEditBorders(v);
      buildEditPolygons(v);
      buildEditElements(v);
      buildEditGeometry(v);
      buildMaterialIds(v);
      buildSmoothingGroups(v);
    }
    connect(ctx_.doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int) {
      if (ids.contains(ctx_.nodeId)) refresh();
    });
    connect(ctx_.state, &EditorState::viewportToolChanged, this, [this] { refreshPickButtons(); });
    currentPanel() = this;
    installTool();
    refresh();
  }

  ~PolyPanel() override {
    if (currentPanel() == this || currentPanel().isNull()) {
      auto tool = activePolyTool(ctx_.state);
      if (tool && tool->target().sameAs(target())) ctx_.state->setViewportTool(nullptr);
      if (SlicePlaneState::get().nodeId == ctx_.nodeId) SlicePlaneState::get().active = false;
    }
  }

  PolyTarget target() const { return PolyTarget(ctx_.doc, ctx_.state, ctx_.nodeId, ctx_.stackIndex); }

  // Clicking the active level again returns to the object level. With convert, the current
  // selection is converted to the new level (Ctrl+click on a level button).
  void setLevel(int level, bool convert = false) {
    PolyTarget t = target();
    if (!t.valid()) return;
    cancelPolyCaddy();
    int cur = t.level();
    if (level == cur && !convert) level = PolyObject;
    if (convert && cur != PolyObject && level != PolyObject) {
      PolySelection s = t.selection();
      convertSelection(*t.mesh(), s, level);
      t.setSelection(s, tr("Convert Selection"));
    } else {
      t.setLevel(level);
    }
    installTool();
    refresh();
    t.showMessage(level == PolyObject ? tr("Object level") : tr("%1 sub-object level").arg(levelName(level)));
  }

  void installTool() {
    PolyTarget t = target();
    EditorState* st = ctx_.state;
    auto cur = activePolyTool(st);
    bool want = t.valid() && t.level() != PolyObject;
    if (cur && cur->target().sameAs(t)) {
      if (!want && cur->pickMode() == PolyPickMode::None) st->setViewportTool(nullptr);
      else bindTool(cur);
      return;
    }
    if (!want) return;
    auto tool = std::make_shared<PolyEditTool>(t);
    bindTool(tool);
    st->setViewportTool(tool);
  }

  // Tool for a pick mode; installed even at the object level.
  std::shared_ptr<PolyEditTool> toolForPick() {
    PolyTarget t = target();
    auto cur = activePolyTool(ctx_.state);
    if (cur && cur->target().sameAs(t)) return cur;
    auto tool = std::make_shared<PolyEditTool>(t);
    bindTool(tool);
    ctx_.state->setViewportTool(tool);
    return tool;
  }

  void bindTool(const std::shared_ptr<PolyEditTool>& tool) {
    QPointer<PolyPanel> self(this);
    tool->onPickModeEnded = [self] {
      if (!self) return;
      self->refreshPickButtons();
      // Leaving a pick mode at the object level removes the tool again.
      if (self->target().level() == PolyObject) {
        auto cur = activePolyTool(self->ctx_.state);
        if (cur && cur->target().sameAs(self->target()) && cur->pickMode() == PolyPickMode::None)
          self->ctx_.state->setViewportTool(nullptr);
      }
    };
  }

  void startPick(PolyPickMode mode, QPushButton* button) {
    auto cur = activePolyTool(ctx_.state);
    if (cur && cur->target().sameAs(target()) && cur->pickMode() == mode) {
      cur->setPickMode(PolyPickMode::None);
      if (cur->onPickModeEnded) cur->onPickModeEnded();
      return;
    }
    cancelPolyCaddy();
    auto tool = toolForPick();
    tool->setPickMode(mode);
    if (button) button->setChecked(true);
    refreshPickButtons();
  }

  void refreshPickButtons() {
    auto cur = activePolyTool(ctx_.state);
    PolyPickMode mode = cur && cur->target().sameAs(target()) ? cur->pickMode() : PolyPickMode::None;
    for (auto& [btn, m] : pickButtons_)
      if (btn) {
        QSignalBlocker b(btn);
        btn->setChecked(m == mode && mode != PolyPickMode::None);
      }
  }

  void refresh() {
    PolyTarget t = target();
    if (!t.valid()) return;
    const PolyMesh* m = t.mesh();
    PolySelection s = t.selection();
    int level = s.level;
    for (int l = 1; l <= 5; ++l) {
      QSignalBlocker b(levelBtn_[l]);
      levelBtn_[l]->setChecked(l == level);
    }
    // Selection info.
    QString info;
    if (level == PolyVertex) info = tr("%1 Vertices Selected").arg(s.verts.size());
    else if (level == PolyEdge || level == PolyBorder) info = tr("%1 Edges Selected").arg(s.edges.size());
    else if (level == PolyFace || level == PolyElement) info = tr("%1 Polygons Selected").arg(s.faces.size());
    else info = m ? tr("%1 vertices, %2 polygons").arg(m->positions.size()).arg(m->faces.size()) : QString();
    if (!s.hiddenFaces.empty() || !s.hiddenVerts.empty())
      info += tr(" (%1 hidden)").arg(s.hiddenFaces.size() + s.hiddenVerts.size());
    selInfo_->setText(info);
    // Soft selection.
    {
      QSignalBlocker b1(softOn_), b2(falloff_), b3(pinch_), b4(bubble_);
      softOn_->setChecked(s.soft.enabled);
      falloff_->setValue(s.soft.falloff);
      pinch_->setValue(s.soft.pinch);
      bubble_->setValue(s.soft.bubble);
      falloff_->setEnabled(s.soft.enabled);
      pinch_->setEnabled(s.soft.enabled);
      bubble_->setEnabled(s.soft.enabled);
      curve_->setSoft(s.soft);
    }
    if (!t.canEdit()) return;
    vertRoll_->setVisible(level == PolyVertex);
    edgeRoll_->setVisible(level == PolyEdge);
    borderRoll_->setVisible(level == PolyBorder);
    polyRoll_->setVisible(level == PolyFace);
    elemRoll_->setVisible(level == PolyElement);
    bool faces = level == PolyFace || level == PolyElement;
    matRoll_->setVisible(faces);
    sgRoll_->setVisible(faces);
    if (faces && m) {
      // Smoothing groups and material ID shared by the selected polygons.
      uint32_t all = ~0u, any = 0;
      int id = -1;
      bool sameId = true;
      for (int f : s.faces) {
        if (f < 0 || f >= static_cast<int>(m->faces.size())) continue;
        all &= m->faces[f].smoothGroups;
        any |= m->faces[f].smoothGroups;
        if (id < 0) id = m->faces[f].matId;
        else if (id != m->faces[f].matId) sameId = false;
      }
      if (s.faces.empty()) all = 0;
      for (int g = 0; g < 32; ++g) {
        QSignalBlocker b(sgBtn_[g]);
        bool on = (all >> g) & 1u;
        sgBtn_[g]->setChecked(on);
        sgBtn_[g]->setStyleSheet(!on && ((any >> g) & 1u) ? "QToolButton { color: #ffcc66; }" : QString());
      }
      if (id >= 0 && sameId) {
        QSignalBlocker b(setId_);
        setId_->setValue(id + 1);
      }
    }
    sliceBox_->setVisible(SlicePlaneState::get().active && SlicePlaneState::get().nodeId == ctx_.nodeId);
    {
      QSignalBlocker b(slicePlaneBtn_);
      slicePlaneBtn_->setChecked(sliceBox_->isVisible());
    }
    refreshPickButtons();
  }

  // ---- Running operations ----

  bool runOp(const json& op, const QString& label) {
    cancelPolyCaddy();
    return target().run(op, label);
  }

  void runSpec(const QString& key, const json& extra = json::object()) {
    const OpSpec* spec = findSpec(key);
    if (!spec) return;
    runOp(opWithValues(*spec, extra), spec->title);
  }

  void openCaddy(const QString& key, QWidget* anchor, const json& extra = json::object()) {
    const OpSpec* spec = findSpec(key);
    PolyTarget t = target();
    if (!spec || !t.valid()) return;
    new OpCaddy(t, *spec, extra, anchor);
  }

 private:
  // ---- Widgets helpers ----

  QPushButton* button(QWidget* parent, const QString& text, const QString& name, std::function<void()> fn,
                      const QString& tip = QString()) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    if (!tip.isEmpty()) b->setToolTip(tip);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(b, &QPushButton::clicked, this, [fn] { fn(); });
    return b;
  }

  // Button with a settings box that opens the caddy.
  QWidget* opButton(QWidget* parent, const QString& text, const QString& key, std::function<void()> fn) {
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(1);
    QPushButton* b = button(w, text, "poly_" + key, std::move(fn));
    auto* s = new QToolButton(w);
    s->setObjectName("poly_" + key + "_settings");
    s->setText(QString::fromUtf8("□"));
    s->setToolTip(tr("%1 Settings").arg(text));
    s->setFixedWidth(18);
    connect(s, &QToolButton::clicked, this, [this, key, s] { openCaddy(key, s); });
    h->addWidget(b);
    h->addWidget(s);
    return w;
  }

  QPushButton* pickButton(QWidget* parent, const QString& text, const QString& name, PolyPickMode mode) {
    auto* b = new QPushButton(text, parent);
    b->setObjectName(name);
    b->setCheckable(true);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(b, &QPushButton::clicked, this, [this, mode, b] { startPick(mode, b); });
    pickButtons_.push_back({QPointer<QPushButton>(b), mode});
    return b;
  }

  Rollout* rollout(QVBoxLayout* v, const QString& title, QGridLayout*& grid) {
    auto* r = new Rollout(title, this);
    auto* host = new QWidget(r->body());
    grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(3);
    grid->setVerticalSpacing(3);
    r->form()->addRow(host);
    v->addWidget(r);
    return r;
  }

  // ---- Rollouts ----

  void buildSelection(QVBoxLayout* v) {
    QGridLayout* g;
    rollout(v, tr("Selection"), g);
    QWidget* host = g->parentWidget();
    auto* levels = new QHBoxLayout();
    levels->setSpacing(2);
    for (int l = 1; l <= 5; ++l) {
      auto* b = new QToolButton(host);
      b->setObjectName(QString("poly_level_%1").arg(l));
      b->setCheckable(true);
      b->setIcon(levelIcon(l));
      b->setIconSize(QSize(28, 28));
      b->setToolTip(tr("%1 (%2) - Ctrl+click converts the selection").arg(levelName(l)).arg(l));
      connect(b, &QToolButton::clicked, this, [this, l] {
        setLevel(l, QApplication::keyboardModifiers() & Qt::ControlModifier);
      });
      levelBtn_[l] = b;
      levels->addWidget(b);
    }
    levels->addStretch();
    g->addLayout(levels, 0, 0, 1, 2);
    auto* byVertex = new QCheckBox(tr("By Vertex"), host);
    auto* backface = new QCheckBox(tr("Ignore Backfacing"), host);
    auto* byAngle = new QCheckBox(tr("By Angle:"), host);
    auto* angle = new QDoubleSpinBox(host);
    byVertex->setObjectName("poly_byVertex");
    backface->setObjectName("poly_ignoreBackfacing");
    byAngle->setObjectName("poly_byAngle");
    angle->setRange(0, 180);
    angle->setValue(PolyToolSettings::get().angle);
    byVertex->setChecked(PolyToolSettings::get().byVertex);
    backface->setChecked(PolyToolSettings::get().ignoreBackfacing);
    byAngle->setChecked(PolyToolSettings::get().byAngle);
    connect(byVertex, &QCheckBox::toggled, this, [](bool on) { PolyToolSettings::get().byVertex = on; });
    connect(backface, &QCheckBox::toggled, this, [](bool on) { PolyToolSettings::get().ignoreBackfacing = on; });
    connect(byAngle, &QCheckBox::toggled, this, [](bool on) { PolyToolSettings::get().byAngle = on; });
    connect(angle, &QDoubleSpinBox::valueChanged, this, [](double a) { PolyToolSettings::get().angle = static_cast<float>(a); });
    g->addWidget(byVertex, 1, 0, 1, 2);
    g->addWidget(byAngle, 2, 0);
    g->addWidget(angle, 2, 1);
    g->addWidget(backface, 8, 0, 1, 2);
    auto selOp = [this](const char* what) {
      PolyTarget t = target();
      const PolyMesh* m = t.mesh();
      if (!m) return;
      PolySelection s = t.selection();
      std::string w = what;
      if (w == "grow") growSelection(*m, s);
      else if (w == "shrink") shrinkSelection(*m, s);
      else if (w == "ring") selectRing(*m, s);
      else if (w == "loop") selectLoop(*m, s);
      t.setSelection(s, tr("Select"));
    };
    g->addWidget(button(host, tr("Shrink"), "poly_shrink", [selOp] { selOp("shrink"); }, tr("Shrink (Ctrl+Page Down)")), 3, 0);
    g->addWidget(button(host, tr("Grow"), "poly_grow", [selOp] { selOp("grow"); }, tr("Grow (Ctrl+Page Up)")), 3, 1);
    g->addWidget(button(host, tr("Ring"), "poly_ring", [selOp] { selOp("ring"); }, tr("Ring (Alt+R)")), 4, 0);
    g->addWidget(button(host, tr("Loop"), "poly_loop", [selOp] { selOp("loop"); }, tr("Loop (Alt+L)")), 4, 1);
    g->addWidget(button(host, tr("Hide Selected"), "poly_hide", [this] { runOp({{"op", "hide"}}, tr("Hide")); }), 5, 0);
    g->addWidget(button(host, tr("Unhide All"), "poly_unhide", [this] { runOp({{"op", "unhideAll"}}, tr("Unhide All")); }), 5, 1);
    g->addWidget(button(host, tr("Hide Unselected"), "poly_hideUnselected",
                        [this] { runOp({{"op", "hideUnselected"}}, tr("Hide Unselected")); }),
                 6, 0, 1, 2);
    selInfo_ = new QLabel(host);
    selInfo_->setObjectName("poly_selInfo");
    selInfo_->setAlignment(Qt::AlignCenter);
    selInfo_->setStyleSheet("color: #c8c8c8;");
    g->addWidget(selInfo_, 7, 0, 1, 2);
  }

  void buildSoft(QVBoxLayout* v) {
    QGridLayout* g;
    rollout(v, tr("Soft Selection"), g);
    QWidget* host = g->parentWidget();
    softOn_ = new QCheckBox(tr("Use Soft Selection"), host);
    softOn_->setObjectName("poly_softOn");
    auto spin = [&](const QString& name, double lo, double hi, double step) {
      auto* s = new QDoubleSpinBox(host);
      s->setObjectName(name);
      s->setRange(lo, hi);
      s->setSingleStep(step);
      s->setDecimals(2);
      installSpinnerScrub(s);
      return s;
    };
    falloff_ = spin("poly_falloff", 0, 1e6, 1);
    pinch_ = spin("poly_pinch", -10, 10, 0.1);
    bubble_ = spin("poly_bubble", -10, 10, 0.1);
    curve_ = new SoftCurve(host);
    g->addWidget(softOn_, 0, 0, 1, 2);
    g->addWidget(new QLabel(tr("Falloff:"), host), 1, 0);
    g->addWidget(falloff_, 1, 1);
    g->addWidget(new QLabel(tr("Pinch:"), host), 2, 0);
    g->addWidget(pinch_, 2, 1);
    g->addWidget(new QLabel(tr("Bubble:"), host), 3, 0);
    g->addWidget(bubble_, 3, 1);
    g->addWidget(curve_, 4, 0, 1, 2);
    auto apply = [this] {
      SoftSelection s;
      s.enabled = softOn_->isChecked();
      s.falloff = static_cast<float>(falloff_->value());
      s.pinch = static_cast<float>(pinch_->value());
      s.bubble = static_cast<float>(bubble_->value());
      target().setSoftSelection(s);
    };
    connect(softOn_, &QCheckBox::toggled, this, apply);
    connect(falloff_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(pinch_, &QDoubleSpinBox::valueChanged, this, apply);
    connect(bubble_, &QDoubleSpinBox::valueChanged, this, apply);
  }

  void buildEditVertices(QVBoxLayout* v) {
    QGridLayout* g;
    vertRoll_ = rollout(v, tr("Edit Vertices"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(button(h, tr("Remove"), "poly_removeVerts", [this] { runOp({{"op", "removeVerts"}}, tr("Remove Vertices")); },
                        tr("Remove vertices and merge their polygons (Backspace)")),
                 0, 0);
    g->addWidget(button(h, tr("Break"), "poly_breakVerts", [this] { runOp({{"op", "breakVerts"}}, tr("Break Vertices")); }), 0, 1);
    g->addWidget(opButton(h, tr("Extrude"), "extrudeVerts", [this] { runSpec("extrudeVerts"); }), 1, 0);
    g->addWidget(opButton(h, tr("Weld"), "weldVerts", [this] { runSpec("weldVerts"); }), 1, 1);
    g->addWidget(opButton(h, tr("Chamfer"), "chamferVerts", [this] { runSpec("chamferVerts"); }), 2, 0);
    g->addWidget(pickButton(h, tr("Target Weld"), "poly_targetWeldVerts", PolyPickMode::TargetWeld), 2, 1);
    g->addWidget(button(h, tr("Connect"), "poly_connectVerts", [this] { runOp({{"op", "connectVerts"}}, tr("Connect Vertices")); }),
                 3, 0, 1, 2);
    g->addWidget(button(h, tr("Remove Isolated Vertices"), "poly_removeIsolated",
                        [this] { runOp({{"op", "removeIsolated"}}, tr("Remove Isolated Vertices")); }),
                 4, 0, 1, 2);
  }

  void buildEditEdges(QVBoxLayout* v) {
    QGridLayout* g;
    edgeRoll_ = rollout(v, tr("Edit Edges"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(pickButton(h, tr("Insert Vertex"), "poly_insertVertexEdge", PolyPickMode::InsertVertex), 0, 0, 1, 2);
    g->addWidget(button(h, tr("Remove"), "poly_removeEdges", [this] {
                   runOp({{"op", "removeEdges"}, {"removeVerts", bool(QApplication::keyboardModifiers() & Qt::ControlModifier)}},
                         tr("Remove Edges"));
                 }, tr("Remove edges (Ctrl+click also removes their vertices)")),
                 1, 0);
    g->addWidget(button(h, tr("Split"), "poly_splitEdges", [this] { runOp({{"op", "splitEdges"}}, tr("Split Edges")); }), 1, 1);
    g->addWidget(opButton(h, tr("Extrude"), "extrudeEdges", [this] { runSpec("extrudeEdges"); }), 2, 0);
    g->addWidget(opButton(h, tr("Weld"), "weldEdges", [this] { runSpec("weldEdges"); }), 2, 1);
    g->addWidget(opButton(h, tr("Chamfer"), "chamferEdges", [this] { runSpec("chamferEdges"); }), 3, 0);
    g->addWidget(pickButton(h, tr("Target Weld"), "poly_targetWeldEdges", PolyPickMode::TargetWeld), 3, 1);
    g->addWidget(opButton(h, tr("Bridge"), "bridgeEdges", [this] { runSpec("bridgeEdges"); }), 4, 0);
    g->addWidget(opButton(h, tr("Connect"), "connectEdges", [this] { runSpec("connectEdges"); }), 4, 1);
    g->addWidget(button(h, tr("Create Shape From Selection"), "poly_createShape", [this] { createShape(); }), 5, 0, 1, 2);
    g->addWidget(button(h, tr("Turn"), "poly_turnEdge", [this] { turnEdges(); },
                        tr("Turns the selected edges that sit between two triangles")),
                 6, 0, 1, 2);
  }

  void buildEditBorders(QVBoxLayout* v) {
    QGridLayout* g;
    borderRoll_ = rollout(v, tr("Edit Borders"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(opButton(h, tr("Extrude"), "extrudeBorders", [this] { runSpec("extrudeBorders"); }), 0, 0);
    g->addWidget(pickButton(h, tr("Insert Vertex"), "poly_insertVertexBorder", PolyPickMode::InsertVertex), 0, 1);
    g->addWidget(opButton(h, tr("Chamfer"), "chamferBorders", [this] { runSpec("chamferBorders"); }), 1, 0);
    g->addWidget(button(h, tr("Cap"), "poly_capBorders", [this] { runOp({{"op", "capBorders"}}, tr("Cap")); }), 1, 1);
    g->addWidget(opButton(h, tr("Bridge"), "bridgeBorders", [this] { runSpec("bridgeBorders"); }), 2, 0);
    g->addWidget(opButton(h, tr("Connect"), "connectBorders", [this] { runSpec("connectBorders"); }), 2, 1);
  }

  void buildEditPolygons(QVBoxLayout* v) {
    QGridLayout* g;
    polyRoll_ = rollout(v, tr("Edit Polygons"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(pickButton(h, tr("Insert Vertex"), "poly_insertVertexPoly", PolyPickMode::InsertVertex), 0, 0, 1, 2);
    g->addWidget(opButton(h, tr("Extrude"), "extrudeFaces", [this] { runSpec("extrudeFaces"); }), 1, 0);
    g->addWidget(opButton(h, tr("Outline"), "outlineFaces", [this] { runSpec("outlineFaces"); }), 1, 1);
    g->addWidget(opButton(h, tr("Bevel"), "bevelFaces", [this] { runSpec("bevelFaces"); }), 2, 0);
    g->addWidget(opButton(h, tr("Inset"), "insetFaces", [this] { runSpec("insetFaces"); }), 2, 1);
    g->addWidget(opButton(h, tr("Bridge"), "bridgeFaces", [this] { runSpec("bridgeFaces"); }), 3, 0);
    g->addWidget(button(h, tr("Flip"), "poly_flipFaces", [this] { runOp({{"op", "flipFaces"}}, tr("Flip Normals")); }), 3, 1);
    g->addWidget(opButton(h, tr("Hinge From Edge"), "hingeFaces", [this] { runSpec("hingeFaces"); }), 4, 0, 1, 2);
    g->addWidget(button(h, tr("Retriangulate"), "poly_retriangulate",
                        [this] { runOp({{"op", "triangulateFaces"}}, tr("Retriangulate")); },
                        tr("Splits the selected polygons into triangles")),
                 5, 0);
    g->addWidget(button(h, tr("Turn"), "poly_turnFaces", [this] { turnEdges(); },
                        tr("Turns the edge shared by two selected triangles")),
                 5, 1);
  }

  void buildEditElements(QVBoxLayout* v) {
    QGridLayout* g;
    elemRoll_ = rollout(v, tr("Edit Elements"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(pickButton(h, tr("Insert Vertex"), "poly_insertVertexElem", PolyPickMode::InsertVertex), 0, 0, 1, 2);
    g->addWidget(button(h, tr("Flip"), "poly_flipElements", [this] { runOp({{"op", "flipFaces"}}, tr("Flip Normals")); }), 1, 0);
    g->addWidget(button(h, tr("Retriangulate"), "poly_retriangulateElem",
                        [this] { runOp({{"op", "triangulateFaces"}}, tr("Retriangulate")); }),
                 1, 1);
  }

  void buildEditGeometry(QVBoxLayout* v) {
    QGridLayout* g;
    rollout(v, tr("Edit Geometry"), g);
    QWidget* h = g->parentWidget();
    g->addWidget(button(h, tr("Repeat Last"), "poly_repeatLast", [this] {
                   const RepeatLast& r = RepeatLast::get();
                   if (r.op.is_null()) target().showMessage(tr("Nothing to repeat yet"));
                   else runOp(r.op, r.label);
                 }),
                 0, 0, 1, 2);
    g->addWidget(pickButton(h, tr("Create"), "poly_create", PolyPickMode::Create), 1, 0);
    g->addWidget(button(h, tr("Collapse"), "poly_collapse", [this] { runOp({{"op", "collapse"}}, tr("Collapse")); }), 1, 1);
    {
      auto* w = new QWidget(h);
      auto* hl = new QHBoxLayout(w);
      hl->setContentsMargins(0, 0, 0, 0);
      hl->setSpacing(1);
      hl->addWidget(pickButton(w, tr("Attach"), "poly_attach", PolyPickMode::Attach));
      auto* list = new QToolButton(w);
      list->setObjectName("poly_attachList");
      list->setText(QString::fromUtf8("□"));
      list->setToolTip(tr("Attach List"));
      list->setFixedWidth(18);
      connect(list, &QToolButton::clicked, this, [this] { attachList(); });
      hl->addWidget(list);
      g->addWidget(w, 2, 0);
    }
    g->addWidget(button(h, tr("Detach"), "poly_detach", [this] { detach(); }), 2, 1);

    slicePlaneBtn_ = new QPushButton(tr("Slice Plane"), h);
    slicePlaneBtn_->setObjectName("poly_slicePlane");
    slicePlaneBtn_->setCheckable(true);
    connect(slicePlaneBtn_, &QPushButton::toggled, this, [this](bool on) { toggleSlicePlane(on); });
    g->addWidget(slicePlaneBtn_, 3, 0);
    auto* split = new QCheckBox(tr("Split"), h);
    split->setObjectName("poly_sliceSplit");
    sliceSplit_ = split;
    g->addWidget(split, 3, 1);
    // Slice plane placement (object space), shown while Slice Plane is on.
    sliceBox_ = new QWidget(h);
    auto* sg = new QGridLayout(sliceBox_);
    sg->setContentsMargins(0, 0, 0, 0);
    sg->setSpacing(2);
    const char* axes[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
      slicePos_[i] = new QDoubleSpinBox(sliceBox_);
      slicePos_[i]->setRange(-1e7, 1e7);
      slicePos_[i]->setObjectName(QString("poly_slicePos%1").arg(axes[i]));
      installSpinnerScrub(slicePos_[i]);
      sliceRot_[i] = new QDoubleSpinBox(sliceBox_);
      sliceRot_[i]->setRange(-360, 360);
      sliceRot_[i]->setSingleStep(5);
      sliceRot_[i]->setObjectName(QString("poly_sliceRot%1").arg(axes[i]));
      installSpinnerScrub(sliceRot_[i]);
      sg->addWidget(new QLabel(tr("Pos %1").arg(axes[i]), sliceBox_), i, 0);
      sg->addWidget(slicePos_[i], i, 1);
      sg->addWidget(new QLabel(tr("Rot %1").arg(axes[i]), sliceBox_), i, 2);
      sg->addWidget(sliceRot_[i], i, 3);
      auto upd = [this] { updateSlicePlane(); };
      connect(slicePos_[i], &QDoubleSpinBox::valueChanged, this, upd);
      connect(sliceRot_[i], &QDoubleSpinBox::valueChanged, this, upd);
    }
    g->addWidget(sliceBox_, 4, 0, 1, 2);
    g->addWidget(button(h, tr("Slice"), "poly_slice", [this] { slice(); }), 5, 0);
    g->addWidget(button(h, tr("Reset Plane"), "poly_resetPlane", [this] { resetSlicePlane(); }), 5, 1);
    g->addWidget(pickButton(h, tr("QuickSlice"), "poly_quickSlice", PolyPickMode::QuickSlice), 6, 0);
    g->addWidget(pickButton(h, tr("Cut"), "poly_cut", PolyPickMode::Cut), 6, 1);
    g->addWidget(opButton(h, tr("MSmooth"), "msmooth", [this] { runSpec("msmooth"); }), 7, 0);
    g->addWidget(opButton(h, tr("Tessellate"), "tessellate", [this] { runSpec("tessellate"); }), 7, 1);
    g->addWidget(pickButton(h, tr("Swift Loop"), "poly_swiftLoop", PolyPickMode::SwiftLoop), 8, 0, 1, 2);
    {
      auto* w = new QWidget(h);
      auto* hl = new QHBoxLayout(w);
      hl->setContentsMargins(0, 0, 0, 0);
      hl->setSpacing(1);
      hl->addWidget(button(w, tr("Make Planar"), "poly_makePlanar", [this] { runOp({{"op", "makePlanar"}}, tr("Make Planar")); }));
      for (int a = 0; a < 3; ++a) {
        auto* b = button(w, axes[a], QString("poly_makePlanar%1").arg(axes[a]),
                         [this, a] { runOp({{"op", "makePlanar"}, {"axis", a}}, tr("Make Planar")); });
        b->setFixedWidth(28);
        hl->addWidget(b);
      }
      g->addWidget(w, 9, 0, 1, 2);
    }
    g->addWidget(button(h, tr("View Align"), "poly_viewAlign", [this] { viewAlign(false); }), 10, 0);
    g->addWidget(button(h, tr("Grid Align"), "poly_gridAlign", [this] { viewAlign(true); }), 10, 1);
    g->addWidget(opButton(h, tr("Relax"), "relax", [this] { runSpec("relax"); }), 11, 0, 1, 2);
  }

  void buildMaterialIds(QVBoxLayout* v) {
    QGridLayout* g;
    matRoll_ = rollout(v, tr("Polygon: Material IDs"), g);
    QWidget* h = g->parentWidget();
    setId_ = new QSpinBox(h);
    setId_->setObjectName("poly_setId");
    setId_->setRange(1, 65535);
    setId_->setKeyboardTracking(false);
    selId_ = new QSpinBox(h);
    selId_->setObjectName("poly_selectIdValue");
    selId_->setRange(1, 65535);
    clearSel_ = new QCheckBox(tr("Clear Selection"), h);
    clearSel_->setChecked(true);
    g->addWidget(new QLabel(tr("Set ID:"), h), 0, 0);
    g->addWidget(setId_, 0, 1);
    g->addWidget(button(h, tr("Set"), "poly_setIdApply", [this] {
                   runOp({{"op", "setMaterialId"}, {"id", setId_->value()}}, tr("Set Material ID"));
                 }),
                 0, 2);
    connect(setId_, &QSpinBox::editingFinished, this, [this] {
      PolyTarget t = target();
      if (!t.selection().faces.empty()) runOp({{"op", "setMaterialId"}, {"id", setId_->value()}}, tr("Set Material ID"));
    });
    g->addWidget(button(h, tr("Select ID"), "poly_selectId", [this] {
                   PolyTarget t = target();
                   const PolyMesh* m = t.mesh();
                   if (!m) return;
                   PolySelection s = t.selection();
                   std::vector<int> faces = facesWithMaterial(*m, selId_->value() - 1);
                   if (clearSel_->isChecked()) s.faces = faces;
                   else s.faces.insert(s.faces.end(), faces.begin(), faces.end());
                   s.normalize();
                   t.setSelection(s, tr("Select by Material ID"));
                 }),
                 1, 0);
    g->addWidget(selId_, 1, 1, 1, 2);
    g->addWidget(clearSel_, 2, 0, 1, 3);
  }

  void buildSmoothingGroups(QVBoxLayout* v) {
    QGridLayout* g;
    sgRoll_ = rollout(v, tr("Polygon: Smoothing Groups"), g);
    QWidget* h = g->parentWidget();
    auto* grid = new QGridLayout();
    grid->setSpacing(1);
    for (int i = 0; i < 32; ++i) {
      auto* b = new QToolButton(h);
      b->setObjectName(QString("poly_sg%1").arg(i + 1));
      b->setText(QString::number(i + 1));
      b->setCheckable(true);
      b->setFixedSize(26, 20);
      connect(b, &QToolButton::toggled, this, [this, i](bool on) {
        runOp({{"op", "setSmoothing"}, {"group", i + 1}, {"on", on}}, tr("Set Smoothing Group"));
      });
      sgBtn_[i] = b;
      grid->addWidget(b, i / 8, i % 8);
    }
    g->addLayout(grid, 0, 0, 1, 3);
    g->addWidget(button(h, tr("Select By SG"), "poly_selectBySG", [this] { selectBySG(); }), 1, 0);
    g->addWidget(button(h, tr("Clear All"), "poly_clearSG", [this] { runOp({{"op", "clearSmoothing"}}, tr("Clear Smoothing")); }), 1, 1, 1, 2);
    autoAngle_ = new QDoubleSpinBox(h);
    autoAngle_->setObjectName("poly_autoSmoothAngle");
    autoAngle_->setRange(0, 180);
    autoAngle_->setValue(45);
    g->addWidget(button(h, tr("Auto Smooth"), "poly_autoSmooth",
                        [this] { runOp({{"op", "autoSmooth"}, {"angle", autoAngle_->value()}}, tr("Auto Smooth")); }),
                 2, 0);
    g->addWidget(autoAngle_, 2, 1, 1, 2);
  }

  // ---- Commands ----

  void turnEdges() {
    PolyTarget t = target();
    const PolyMesh* m = t.mesh();
    if (!m) return;
    PolySelection s = t.selection();
    std::vector<uint64_t> edges = s.edges;
    if (s.level == PolyFace || s.level == PolyElement) {
      // The edge shared by two selected triangles.
      edges.clear();
      std::map<uint64_t, int> count;
      for (int f : s.faces)
        if (f >= 0 && f < static_cast<int>(m->faces.size()) && m->faces[f].v.size() == 3)
          for (int k = 0; k < 3; ++k) count[edgeKey(m->faces[f].v[k], m->faces[f].v[(k + 1) % 3])]++;
      for (const auto& [k, n] : count)
        if (n == 2) edges.push_back(k);
    }
    if (edges.empty()) {
      t.showMessage(tr("Turn needs an edge between two triangles"));
      return;
    }
    runOp({{"op", "turnEdge"}, {"a", edgeKeyA(edges[0])}, {"b", edgeKeyB(edges[0])}}, tr("Turn"));
  }

  void createShape() {
    PolyTarget t = target();
    const PolyMesh* m = t.mesh();
    Node* src = t.node();
    if (!m || !src) return;
    PolySelection s = t.selection();
    if (s.edges.empty()) {
      t.showMessage(tr("Select edges to make a shape from"));
      return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Create Shape"));
    auto* f = new QFormLayout(&dlg);
    auto* name = new QLineEdit(QString::fromStdString(ctx_.doc->scene().uniqueNodeName("Shape")), &dlg);
    auto* smooth = new QRadioButton(tr("Smooth"), &dlg);
    auto* linear = new QRadioButton(tr("Linear"), &dlg);
    linear->setChecked(true);
    f->addRow(tr("Curve Name:"), name);
    f->addRow(smooth);
    f->addRow(linear);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<bool> closed;
    auto chains = edgeChains(*m, s.edges, &closed);
    auto shape = std::make_unique<EditableSplineObject>();
    for (size_t i = 0; i < chains.size(); ++i) {
      Spline sp;
      sp.closed = closed[i];
      for (const Vec3& p : chains[i]) {
        Knot k = cornerKnot(p);
        if (smooth->isChecked()) k.type = KnotType::Smooth;
        sp.knots.push_back(k);
      }
      sp.updateAutoHandles();
      shape->shape.splines.push_back(sp);
    }
    ctx_.doc->beginEdit(tr("Create Shape From Selection"));
    auto node = std::make_unique<Node>();
    node->name = name->text().toStdString();
    node->object = std::move(shape);
    node->wireColor = Vec3(0.1f, 0.6f, 0.9f);
    Node* added = ctx_.doc->scene().addNode(std::move(node));
    added->setWorldMatrix(src->objectWorldMatrix());
    ctx_.doc->notifyStructureChanged();
    ctx_.doc->endEdit();
    t.showMessage(tr("Created shape %1 with %2 spline(s)").arg(QString::fromStdString(added->name)).arg(chains.size()));
  }

  void detach() {
    PolyTarget t = target();
    const PolyMesh* m = t.mesh();
    Node* src = t.node();
    if (!m || !src) return;
    PolySelection s = t.selection();
    std::vector<int> faces = facesFromSelection(*m, s);
    if (faces.empty()) {
      t.showMessage(tr("Select polygons or elements to detach"));
      return;
    }
    DetachDialog dlg(QString::fromStdString(ctx_.doc->scene().uniqueNodeName("Object")), this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (dlg.toElement_->isChecked()) {
      if (dlg.asClone_->isChecked()) {
        // Clone the polygons as a new element of the same object.
        PolyMesh part = extractFaces(*m, faces);
        runOp({{"op", "attach"}, {"mesh", polyMeshToJson(part)}}, tr("Detach As Clone"));
      } else {
        runOp({{"op", "detachElement"}}, tr("Detach To Element"));
      }
      return;
    }
    PolyMesh part = extractFaces(*m, faces);
    uint32_t srcId = src->id;
    ctx_.doc->beginEdit(tr("Detach"));
    auto obj = std::make_shared<EditablePolyObject>();
    obj->mesh = std::move(part);
    auto node = std::make_unique<Node>();
    node->name = dlg.name_->text().toStdString();
    node->object = obj;
    node->materialId = src->materialId;
    node->wireColor = src->wireColor;
    Node* added = ctx_.doc->scene().addNode(std::move(node));
    if (Node* again = ctx_.doc->scene().findNode(srcId)) added->setWorldMatrix(again->objectWorldMatrix());
    if (!dlg.asClone_->isChecked()) {
      PolySelection del = s;
      del.level = PolyFace;
      del.faces = faces;
      t.setSelection(del, tr("Detach"));
      t.run({{"op", "delete"}}, tr("Detach"));
    }
    ctx_.doc->notifyStructureChanged();
    ctx_.doc->endEdit();
  }

  void attachList() {
    PolyTarget t = target();
    Node* self = t.node();
    if (!self) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Attach List"));
    auto* v = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    list->setObjectName("poly_attachListWidget");
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto& n : ctx_.doc->scene().nodes)
      if (n.get() != self && n->kind() == ObjectKind::Geometry && !n->hidden) {
        auto* item = new QListWidgetItem(QString::fromStdString(n->name), list);
        item->setData(Qt::UserRole, n->id);
      }
    v->addWidget(list);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Attach"));
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<uint32_t> ids;
    for (auto* item : list->selectedItems()) ids.push_back(item->data(Qt::UserRole).toUInt());
    attachNodes(ids);
  }

  void attachNodes(const std::vector<uint32_t>& ids) {
    PolyTarget t = target();
    if (ids.empty() || !t.valid()) return;
    SceneDocument* doc = ctx_.doc;
    doc->beginEdit(tr("Attach"));
    for (uint32_t id : ids) {
      Node* self = t.node();
      Node* other = doc->scene().findNode(id);
      if (!self || !other) continue;
      PolyMesh om = other->mesh();
      Mat4 rel = self->objectWorldMatrix().inverse() * other->objectWorldMatrix();
      om.transform(rel);
      if (rel.determinant3x3() < 0.0f) om.flipFaces();
      if (t.run({{"op", "attach"}, {"mesh", polyMeshToJson(om)}}, tr("Attach"))) doc->scene().removeNode(id);
    }
    doc->notifyStructureChanged();
    doc->endEdit();
  }

 public:
  // ---- Slice plane ----
  void toggleSlicePlane(bool on) {
    SlicePlaneState& sp = SlicePlaneState::get();
    if (on) {
      sp.active = true;
      sp.nodeId = ctx_.nodeId;
      sp.stackIndex = ctx_.stackIndex;
      resetSlicePlane();
      // The plane is drawn by the sub-object tool.
      toolForPick();
    } else if (sp.nodeId == ctx_.nodeId) {
      sp.active = false;
      installTool();
    }
    refresh();
    ctx_.state->requestRedraw();
  }

  void resetSlicePlane() {
    PolyTarget t = target();
    const PolyMesh* m = t.mesh();
    SlicePlaneState& sp = SlicePlaneState::get();
    Aabb b;
    if (m) {
      PolySelection s = t.selection();
      std::vector<int> verts = selectedVertsAtLevel(*m, s);
      if (verts.empty()) b = m->bounds();
      else
        for (int v : verts) b.expand(m->positions[v]);
    }
    sp.center = b.valid() ? b.center() : Vec3();
    sp.rotation = Vec3();
    sp.size = m && m->bounds().valid() ? std::max(10.0f, m->bounds().radius() * 1.1f) : 100.0f;
    for (int i = 0; i < 3; ++i) {
      QSignalBlocker b1(slicePos_[i]), b2(sliceRot_[i]);
      slicePos_[i]->setValue(sp.center[i]);
      sliceRot_[i]->setValue(0);
    }
    ctx_.state->requestRedraw();
  }

  void updateSlicePlane() {
    SlicePlaneState& sp = SlicePlaneState::get();
    for (int i = 0; i < 3; ++i) {
      sp.center[i] = static_cast<float>(slicePos_[i]->value());
      sp.rotation[i] = static_cast<float>(sliceRot_[i]->value());
    }
    ctx_.state->requestRedraw();
  }

  void slice() {
    PolyTarget t = target();
    SlicePlaneState& sp = SlicePlaneState::get();
    if (!sp.active || sp.nodeId != ctx_.nodeId) {
      t.showMessage(tr("Turn on Slice Plane first"));
      return;
    }
    PolySelection s = t.selection();
    bool onlySel = (s.level == PolyFace || s.level == PolyElement) && !s.faces.empty();
    runOp({{"op", "slice"},
           {"point", vec3ToJson(sp.center)},
           {"normal", vec3ToJson(sp.normal())},
           {"selectedOnly", onlySel},
           {"split", sliceSplit_->isChecked()}},
          tr("Slice"));
  }

 private:
  void viewAlign(bool grid) {
    MainWindow* w = mainWindow();
    PolyTarget t = target();
    if (!w || !t.valid()) return;
    Viewport* vp = w->viewports()->active();
    vp->updateCamera();
    const ViewCamera& cam = vp->camera();
    Mat4 ow = t.objectToWorld();
    Vec3 n, p;
    if (grid) cam.constructionPlane(p, n);
    else n = cam.forward();
    // Plane normals transform with the transpose of the object-to-world matrix.
    Vec3 ln = normalize(Vec3(ow.m[0][0] * n.x + ow.m[1][0] * n.y + ow.m[2][0] * n.z,
                             ow.m[0][1] * n.x + ow.m[1][1] * n.y + ow.m[2][1] * n.z,
                             ow.m[0][2] * n.x + ow.m[1][2] * n.y + ow.m[2][2] * n.z));
    json op = {{"op", "makePlanar"}, {"normal", vec3ToJson(ln)}};
    if (grid) op["point"] = vec3ToJson(ow.inverse().transformPoint(p));
    runOp(op, grid ? tr("Grid Align") : tr("View Align"));
  }

  void selectBySG() {
    PolyTarget t = target();
    const PolyMesh* m = t.mesh();
    if (!m) return;
    uint32_t present = 0;
    for (const auto& f : m->faces) present |= f.smoothGroups;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Select By Smooth Groups"));
    auto* v = new QVBoxLayout(&dlg);
    auto* grid = new QGridLayout();
    QToolButton* btns[32];
    for (int i = 0; i < 32; ++i) {
      btns[i] = new QToolButton(&dlg);
      btns[i]->setText(QString::number(i + 1));
      btns[i]->setCheckable(true);
      btns[i]->setEnabled((present >> i) & 1u);
      btns[i]->setFixedSize(26, 20);
      grid->addWidget(btns[i], i / 8, i % 8);
    }
    v->addLayout(grid);
    auto* clear = new QCheckBox(tr("Clear Selection"), &dlg);
    clear->setChecked(true);
    v->addWidget(clear);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    PolySelection s = t.selection();
    if (clear->isChecked()) s.faces.clear();
    for (int i = 0; i < 32; ++i)
      if (btns[i]->isChecked()) {
        auto faces = facesWithSmoothingGroup(*m, i + 1);
        s.faces.insert(s.faces.end(), faces.begin(), faces.end());
      }
    s.normalize();
    t.setSelection(s, tr("Select By Smoothing Group"));
  }

  ModifyPanelContext ctx_;
  QToolButton* levelBtn_[6] = {};
  QLabel* selInfo_ = nullptr;
  QCheckBox* softOn_ = nullptr;
  QDoubleSpinBox *falloff_ = nullptr, *pinch_ = nullptr, *bubble_ = nullptr;
  SoftCurve* curve_ = nullptr;
  Rollout *vertRoll_ = nullptr, *edgeRoll_ = nullptr, *borderRoll_ = nullptr, *polyRoll_ = nullptr, *elemRoll_ = nullptr;
  Rollout *matRoll_ = nullptr, *sgRoll_ = nullptr;
  QToolButton* sgBtn_[32] = {};
  QSpinBox *setId_ = nullptr, *selId_ = nullptr;
  QCheckBox* clearSel_ = nullptr;
  QDoubleSpinBox* autoAngle_ = nullptr;
  QPushButton* slicePlaneBtn_ = nullptr;
  QCheckBox* sliceSplit_ = nullptr;
  QWidget* sliceBox_ = nullptr;
  QDoubleSpinBox* slicePos_[3] = {};
  QDoubleSpinBox* sliceRot_[3] = {};
  std::vector<std::pair<QPointer<QPushButton>, PolyPickMode>> pickButtons_;
};

QPointer<PolyPanel>& currentPanel() {
  static QPointer<PolyPanel> p;
  return p;
}

// The panel for the node the user is working on (single selection, Modify panel built).
PolyPanel* activePanel() {
  PolyPanel* p = currentPanel();
  if (!p || !p->target().valid()) return nullptr;
  return p;
}

// ---------------------------------------------------------------------------------------------
// Installer

void collapseStack(MainWindow& w, bool keepIfPoly) {
  SceneDocument* doc = w.document();
  QVector<uint32_t> ids;
  for (Node* n : doc->selectedNodes())
    if (n->kind() == ObjectKind::Geometry) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  doc->beginEdit(keepIfPoly ? QObject::tr("Collapse Stack") : QObject::tr("Convert to Editable Poly"));
  for (uint32_t id : ids) {
    Node* n = doc->scene().findNode(id);
    if (!n) continue;
    if (keepIfPoly && n->modifiers.empty()) continue;
    auto poly = std::make_shared<EditablePolyObject>();
    poly->mesh = n->mesh();
    n->object = poly;
    n->modifiers.clear();
  }
  doc->notifyStructureChanged();  // first: panels drop references to the replaced objects
  doc->notifyNodesChanged(ids, ChangeGeometry);
  doc->endEdit();
}

bool selectionIsPolyEditable(MainWindow& w) {
  auto nodes = w.document()->selectedNodes();
  if (nodes.size() != 1) return false;
  Node* n = nodes[0];
  if (dynamic_cast<EditablePolyObject*>(n->object.get())) return true;
  for (const auto& m : n->modifiers)
    if (dynamic_cast<SubObjectModifier*>(m.get())) return true;
  return false;
}

void installPolyPanel(MainWindow& w) {
  mainWindow() = &w;
  auto factory = [](const ModifyPanelContext& ctx, QWidget* parent) -> QWidget* { return new PolyPanel(ctx, parent); };
  registerModifyRollouts("EditablePoly", factory);
  registerModifyRollouts("EditPoly", factory);
  registerModifyRollouts("PolySelect", factory);
  registerModifyRollouts("MeshSelect", factory);

  // Sub-object level hotkeys (1-5, 6 = object level) and context menu entries.
  std::vector<QAction*> levelActions;
  for (int l = 0; l <= 5; ++l) {
    QString text = l == 0 ? QObject::tr("Top-level") : QObject::tr(levelName(l));
    auto* a = new QAction(text, &w);
    a->setShortcut(QKeySequence(QString::number(l == 0 ? 6 : l)));
    a->setProperty("quadMenu", true);
    a->setVisible(false);
    a->setObjectName(QString("poly.level%1").arg(l));
    w.addAction(a);
    QObject::connect(a, &QAction::triggered, &w, [&w, l] {
      if (!selectionIsPolyEditable(w)) return;
      w.commandPanel()->showModify();
      if (PolyPanel* p = activePanel()) {
        int cur = p->target().level();
        if (l == PolyObject) {
          if (cur != PolyObject) p->setLevel(cur);  // toggles off
        } else if (cur != l) {
          p->setLevel(l);
        }
      }
    });
    levelActions.push_back(a);
  }
  QObject::connect(w.document(), &SceneDocument::selectionChanged, &w, [&w, levelActions] {
    bool on = selectionIsPolyEditable(w);
    for (QAction* a : levelActions) a->setVisible(on);
  });
  QObject::connect(w.document(), &SceneDocument::structureChanged, &w, [&w, levelActions] {
    bool on = selectionIsPolyEditable(w);
    for (QAction* a : levelActions) a->setVisible(on);
  });

  // Ctrl+B toggles between the object level and the last sub-object level.
  auto* toggle = new QAction(QObject::tr("Sub-object Toggle"), &w);
  toggle->setShortcut(QKeySequence("Ctrl+B"));
  w.addAction(toggle);
  QObject::connect(toggle, &QAction::triggered, &w, [&w] {
    static int last = PolyVertex;
    PolyPanel* p = activePanel();
    if (!p) return;
    w.commandPanel()->showModify();
    int cur = p->target().level();
    if (cur != PolyObject) {
      last = cur;
      p->setLevel(cur);
    } else {
      p->setLevel(last);
    }
  });

  // Selection and editing hotkeys.
  struct Hotkey {
    const char* key;
    const char* text;
    std::function<void(PolyPanel*)> fn;
  };
  auto selOp = [](PolyPanel* p, int which) {
    PolyTarget t = p->target();
    const PolyMesh* m = t.mesh();
    if (!m) return;
    PolySelection s = t.selection();
    if (which == 0) growSelection(*m, s);
    else if (which == 1) shrinkSelection(*m, s);
    else if (which == 2) selectLoop(*m, s);
    else selectRing(*m, s);
    t.setSelection(s, QObject::tr("Select"));
  };
  std::vector<Hotkey> keys = {
      {"Ctrl+PgUp", "Grow Selection", [selOp](PolyPanel* p) { selOp(p, 0); }},
      {"Ctrl+PgDown", "Shrink Selection", [selOp](PolyPanel* p) { selOp(p, 1); }},
      {"Alt+L", "Select Loop", [selOp](PolyPanel* p) { selOp(p, 2); }},
      {"Alt+R", "Select Ring", [selOp](PolyPanel* p) { selOp(p, 3); }},
      {"Shift+E", "Extrude",
       [](PolyPanel* p) {
         int l = p->target().level();
         p->runSpec(l == PolyVertex ? "extrudeVerts" : l == PolyEdge ? "extrudeEdges" : l == PolyBorder ? "extrudeBorders" : "extrudeFaces");
       }},
      {"Ctrl+Shift+E", "Connect",
       [](PolyPanel* p) {
         int l = p->target().level();
         if (l == PolyVertex) p->runOp({{"op", "connectVerts"}}, QObject::tr("Connect Vertices"));
         else p->runSpec(l == PolyBorder ? "connectBorders" : "connectEdges");
       }},
      {"Ctrl+Shift+C", "Chamfer",
       [](PolyPanel* p) {
         int l = p->target().level();
         p->runSpec(l == PolyVertex ? "chamferVerts" : l == PolyBorder ? "chamferBorders" : "chamferEdges");
       }},
  };
  for (const auto& k : keys) {
    auto* a = new QAction(QObject::tr(k.text), &w);
    a->setShortcut(QKeySequence(k.key));
    w.addAction(a);
    auto fn = k.fn;
    QObject::connect(a, &QAction::triggered, &w, [fn] {
      if (PolyPanel* p = activePanel(); p && p->target().level() != PolyObject) fn(p);
    });
  }

  // Tools > Collapse Stack (also in the viewport context menu).
  QMenu* tools = w.menuAt("Tools");
  QAction* collapse = tools->addAction(QObject::tr("Collapse Stack"));
  collapse->setObjectName("tools.collapseStack");
  collapse->setProperty("quadMenu", true);
  QObject::connect(collapse, &QAction::triggered, &w, [&w] { collapseStack(w, true); });
  w.registerCommand("tools.collapseStack", collapse);
}

}  // namespace

bool cancelPolyCaddy() {
  OpCaddy* c = currentCaddy();
  if (!c) return false;
  c->finish(false);
  return true;
}

VERAA_APP_EXTENSION(polyPanel, 200, installPolyPanel);

}  // namespace veraa
