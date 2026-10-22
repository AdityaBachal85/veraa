#include "paramform.h"

#include "units.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace veraa {

namespace {
QHash<QString, bool>& collapsedGroups() {
  static QHash<QString, bool> h;
  return h;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

Rollout::Rollout(const QString& title, QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  header_ = new QToolButton(this);
  header_->setObjectName("RolloutHeader");
  header_->setText(title);
  header_->setCheckable(true);
  header_->setChecked(true);
  header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  header_->setArrowType(Qt::DownArrow);
  header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  body_ = new QFrame(this);
  body_->setObjectName("RolloutBody");
  form_ = new QFormLayout(body_);
  form_->setContentsMargins(6, 6, 6, 6);
  form_->setHorizontalSpacing(6);
  form_->setVerticalSpacing(3);
  form_->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  v->addWidget(header_);
  v->addWidget(body_);
  connect(header_, &QToolButton::toggled, this, [this, title](bool on) {
    setExpanded(on);
    collapsedGroups()[title] = !on;
  });
  if (collapsedGroups().value(title, false)) setExpanded(false);
}

void Rollout::setExpanded(bool on) {
  QSignalBlocker b(header_);
  header_->setChecked(on);
  header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
  body_->setVisible(on);
}

bool Rollout::isExpanded() const { return body_->isVisible(); }

// ---------------------------------------------------------------------------------------------

ColorSwatch::ColorSwatch(QWidget* parent) : QPushButton(parent) {
  setFixedHeight(20);
  setMinimumWidth(48);
  connect(this, &QPushButton::clicked, this, &ColorSwatch::pick);
}

void ColorSwatch::setColor(const Vec3& c) {
  color_ = c;
  setToolTip(QString("R %1  G %2  B %3").arg(c.x, 0, 'f', 3).arg(c.y, 0, 'f', 3).arg(c.z, 0, 'f', 3));
  update();
}

void ColorSwatch::paintEvent(QPaintEvent*) {
  QPainter p(this);
  QRect r = rect().adjusted(1, 1, -2, -2);
  p.setPen(QColor(20, 20, 20));
  p.setBrush(QColor::fromRgbF(saturate(color_.x), saturate(color_.y), saturate(color_.z)));
  p.drawRect(r);
  if (hasFocus()) {
    p.setPen(QColor(110, 170, 230));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(1, 1, -1, -1));
  }
}

void ColorSwatch::pick() {
  Vec3 old = color_;
  QColorDialog dlg(QColor::fromRgbF(saturate(old.x), saturate(old.y), saturate(old.z)), this);
  dlg.setOption(QColorDialog::NoButtons, false);
  connect(&dlg, &QColorDialog::currentColorChanged, this, [this](const QColor& c) {
    Vec3 v(static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF()));
    setColor(v);
    Q_EMIT colorEdited(v);
  });
  if (dlg.exec() != QDialog::Accepted) {
    setColor(old);
    Q_EMIT colorEdited(old);
  }
}

// ---------------------------------------------------------------------------------------------

namespace {

class SpinnerScrub : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* obj, QEvent* ev) override {
    auto* w = static_cast<QWidget*>(obj);
    auto* dsb = qobject_cast<QDoubleSpinBox*>(obj);
    auto* isb = qobject_cast<QSpinBox*>(obj);
    if (!dsb && !isb) return false;
    auto inArrows = [w](const QPointF& p) { return p.x() >= w->width() - 18; };
    switch (ev->type()) {
      case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (!inArrows(me->position())) return false;
        if (me->button() == Qt::RightButton) {
          // 3ds Max: right-click on the arrows resets to the lowest value (or zero).
          if (dsb) dsb->setValue(dsb->minimum() <= 0.0 && dsb->maximum() >= 0.0 ? 0.0 : dsb->minimum());
          if (isb) isb->setValue(isb->minimum() <= 0 && isb->maximum() >= 0 ? 0 : isb->minimum());
          return true;
        }
        if (me->button() == Qt::LeftButton) {
          startY_ = me->position().y();
          startValue_ = dsb ? dsb->value() : isb->value();
          scrubbing_ = false;
          armed_ = true;
        }
        return false;
      }
      case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (!armed_ || !(me->buttons() & Qt::LeftButton)) return false;
        double dy = startY_ - me->position().y();
        if (!scrubbing_ && std::fabs(dy) < 3) return false;
        scrubbing_ = true;
        double step = dsb ? dsb->singleStep() : isb->singleStep();
        double speed = (me->modifiers() & Qt::ControlModifier) ? 10.0 : (me->modifiers() & Qt::AltModifier) ? 0.1 : 1.0;
        double v = startValue_ + std::round(dy / 2.0) * step * speed;
        if (dsb) dsb->setValue(v);
        else isb->setValue(static_cast<int>(std::lround(v)));
        return true;
      }
      case QEvent::MouseButtonRelease:
        armed_ = false;
        scrubbing_ = false;
        return false;
      default: return false;
    }
  }

 private:
  double startY_ = 0, startValue_ = 0;
  bool scrubbing_ = false, armed_ = false;
};

}  // namespace

void installSpinnerScrub(QWidget* spinBox) {
  auto* filter = new SpinnerScrub(spinBox);
  spinBox->installEventFilter(filter);
}

// ---------------------------------------------------------------------------------------------

ParamForm::ParamForm(QWidget* parent) : QWidget(parent) {
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(4);
}

void ParamForm::setBlock(ParamBlock* block, const QStringList& exclude) {
  block_ = block;
  exclude_ = exclude;
  rebuild();
}

void ParamForm::apply(const std::string& key, const std::function<bool(ParamBlock&)>& set) {
  if (refreshing_ || !block_) return;
  if (beforeChange) beforeChange(key);
  if (!block_) return;
  bool changed = set(*block_);
  if (changed && afterChange) afterChange(key);
}

void ParamForm::refresh() {
  refreshing_ = true;
  for (auto& r : refreshers_) r();
  refreshing_ = false;
}

void ParamForm::rebuild() {
  refreshers_.clear();
  while (QLayoutItem* item = layout_->takeAt(0)) {
    if (QWidget* w = item->widget()) {
      w->hide();  // deleteLater runs later; don't let old rollouts show through meanwhile
      w->deleteLater();
    }
    delete item;
  }
  if (!block_) return;

  std::vector<std::string> groups;
  for (const auto& d : block_->defs())
    if (std::find(groups.begin(), groups.end(), d.group) == groups.end()) groups.push_back(d.group);

  for (const std::string& group : groups) {
    bool any = false;
    for (const ParamDef& d : block_->defs())
      any |= d.group == group && !exclude_.contains(QString::fromStdString(d.key));
    if (!any) continue;
    auto* roll = new Rollout(QString::fromStdString(group), this);
    QFormLayout* form = roll->form();
    for (const ParamDef& d : block_->defs()) {
      if (d.group != group || exclude_.contains(QString::fromStdString(d.key))) continue;
      const std::string key = d.key;
      QString label = QString::fromStdString(d.label);
      QWidget* field = nullptr;
      switch (d.type) {
        case ParamType::Float: {
          // Lengths show and accept display units (Customize > Units Setup).
          auto* sb = isLengthParam(d) ? new UnitSpinBox(roll->body()) : new QDoubleSpinBox(roll->body());
          sb->setRange(std::max(d.minValue, -1e9), std::min(d.maxValue, 1e9));
          sb->setDecimals(isLengthParam(d) ? std::max(d.decimals, 3) : d.decimals);
          sb->setSingleStep(d.step);
          sb->setKeyboardTracking(false);
          sb->setAccelerated(true);
          sb->setMinimumWidth(70);
          installSpinnerScrub(sb);
          connect(sb, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
            apply(key, [&](ParamBlock& b) { return b.setNumber(key, v); });
          });
          refreshers_.push_back([this, sb, key] {
            QSignalBlocker blk(sb);
            sb->setValue(block_->getFloat(key));
          });
          field = sb;
          break;
        }
        case ParamType::Int: {
          auto* sb = new QSpinBox(roll->body());
          sb->setRange(static_cast<int>(std::max(d.minValue, -2e9)), static_cast<int>(std::min(d.maxValue, 2e9)));
          sb->setKeyboardTracking(false);
          sb->setMinimumWidth(70);
          installSpinnerScrub(sb);
          connect(sb, &QSpinBox::valueChanged, this, [this, key](int v) {
            apply(key, [&](ParamBlock& b) { return b.setNumber(key, v); });
          });
          refreshers_.push_back([this, sb, key] {
            QSignalBlocker blk(sb);
            sb->setValue(block_->getInt(key));
          });
          field = sb;
          break;
        }
        case ParamType::Bool: {
          auto* cb = new QCheckBox(label, roll->body());
          connect(cb, &QCheckBox::toggled, this, [this, key](bool on) {
            apply(key, [&](ParamBlock& b) { return b.setNumber(key, on ? 1.0 : 0.0); });
          });
          refreshers_.push_back([this, cb, key] {
            QSignalBlocker blk(cb);
            cb->setChecked(block_->getBool(key));
          });
          form->addRow(QString(), cb);
          continue;
        }
        case ParamType::Color: {
          auto* sw = new ColorSwatch(roll->body());
          connect(sw, &ColorSwatch::colorEdited, this, [this, key](const Vec3& c) {
            apply(key, [&](ParamBlock& b) { return b.setColor(key, c); });
          });
          refreshers_.push_back([this, sw, key] { sw->setColor(block_->getColor(key)); });
          field = sw;
          break;
        }
        case ParamType::Enum: {
          auto* combo = new QComboBox(roll->body());
          for (const auto& item : d.items) combo->addItem(QString::fromStdString(item));
          connect(combo, &QComboBox::currentIndexChanged, this, [this, key](int i) {
            apply(key, [&](ParamBlock& b) { return b.setNumber(key, i); });
          });
          refreshers_.push_back([this, combo, key] {
            QSignalBlocker blk(combo);
            combo->setCurrentIndex(block_->getInt(key));
          });
          field = combo;
          break;
        }
        case ParamType::String: {
          auto* le = new QLineEdit(roll->body());
          connect(le, &QLineEdit::editingFinished, this, [this, le, key] {
            std::string s = le->text().toStdString();
            apply(key, [&](ParamBlock& b) { return b.setString(key, s); });
          });
          refreshers_.push_back([this, le, key] {
            QSignalBlocker blk(le);
            le->setText(QString::fromStdString(block_->getString(key)));
          });
          field = le;
          break;
        }
        case ParamType::File: {
          auto* box = new QWidget(roll->body());
          auto* h = new QHBoxLayout(box);
          h->setContentsMargins(0, 0, 0, 0);
          h->setSpacing(2);
          auto* le = new QLineEdit(box);
          le->setPlaceholderText(tr("None"));
          auto* browse = new QToolButton(box);
          browse->setText("...");
          browse->setToolTip(tr("Browse"));
          auto* clear = new QToolButton(box);
          clear->setText(QString::fromUtf8("×"));
          clear->setToolTip(tr("Clear"));
          h->addWidget(le, 1);
          h->addWidget(browse);
          h->addWidget(clear);
          QString filter = QString::fromStdString(d.fileFilter);
          connect(le, &QLineEdit::editingFinished, this, [this, le, key] {
            std::string s = le->text().toStdString();
            apply(key, [&](ParamBlock& b) { return b.setString(key, s); });
          });
          connect(browse, &QToolButton::clicked, this, [this, le, key, filter] {
            QString start = le->text().isEmpty() ? browseDir : QFileInfo(le->text()).absolutePath();
            QString f = QFileDialog::getOpenFileName(this, tr("Choose File"), start,
                                                     filter + tr(";;All files (*)"));
            if (f.isEmpty()) return;
            le->setText(f);
            std::string s = f.toStdString();
            apply(key, [&](ParamBlock& b) { return b.setString(key, s); });
          });
          connect(clear, &QToolButton::clicked, this, [this, le, key] {
            le->clear();
            apply(key, [&](ParamBlock& b) { return b.setString(key, std::string()); });
          });
          refreshers_.push_back([this, le, key] {
            QSignalBlocker blk(le);
            QString path = QString::fromStdString(block_->getString(key));
            le->setText(path);
            le->setToolTip(path);
          });
          field = box;
          break;
        }
      }
      if (!d.tooltip.empty()) field->setToolTip(QString::fromStdString(d.tooltip));
      form->addRow(label + ":", field);
    }
    layout_->addWidget(roll);
  }
  refresh();
}

}  // namespace veraa
