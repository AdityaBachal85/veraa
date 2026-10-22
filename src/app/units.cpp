#include "units.h"

#include <QRegularExpression>
#include <QSettings>

#include <cmath>

#include "params.h"

namespace veraa {

Units& Units::instance() {
  static Units* u = new Units();
  return *u;
}

Units::Units() { load(); }

void Units::setDisplay(DisplayUnits u) {
  if (u == display_) return;
  display_ = u;
  save();
  Q_EMIT changed();
}

void Units::setSystemUnitCm(double cm) {
  if (cm <= 0 || cm == systemCm_) return;
  systemCm_ = cm;
  save();
  Q_EMIT changed();
}

double Units::displayPerCm() const {
  switch (display_) {
    case DisplayUnits::Generic: return 1.0 / systemCm_;  // generic shows raw system units
    case DisplayUnits::Millimeters: return 10.0;
    case DisplayUnits::Centimeters: return 1.0;
    case DisplayUnits::Meters: return 0.01;
    case DisplayUnits::Kilometers: return 0.00001;
    case DisplayUnits::Inches:
    case DisplayUnits::FeetInches: return 1.0 / 2.54;
    case DisplayUnits::Feet: return 1.0 / 30.48;
  }
  return 1.0;
}

double Units::toDisplay(double v) const { return v * systemCm_ * displayPerCm(); }

double Units::fromDisplay(double v) const { return v / (systemCm_ * displayPerCm()); }

QString Units::suffix() const {
  switch (display_) {
    case DisplayUnits::Millimeters: return "mm";
    case DisplayUnits::Centimeters: return "cm";
    case DisplayUnits::Meters: return "m";
    case DisplayUnits::Kilometers: return "km";
    case DisplayUnits::Inches: return "\"";
    case DisplayUnits::Feet: return "'";
    default: return QString();
  }
}

QString Units::unitName(DisplayUnits u) {
  switch (u) {
    case DisplayUnits::Generic: return tr("Generic Units");
    case DisplayUnits::Millimeters: return tr("Millimeters");
    case DisplayUnits::Centimeters: return tr("Centimeters");
    case DisplayUnits::Meters: return tr("Meters");
    case DisplayUnits::Kilometers: return tr("Kilometers");
    case DisplayUnits::Inches: return tr("Decimal Inches");
    case DisplayUnits::Feet: return tr("Decimal Feet");
    case DisplayUnits::FeetInches: return tr("Feet w/Fractional Inches");
  }
  return QString();
}

QString Units::format(double v, int decimals) const {
  if (display_ == DisplayUnits::FeetInches) {
    double inches = v * systemCm_ / 2.54;
    bool neg = inches < 0;
    inches = std::fabs(inches);
    int feet = static_cast<int>(std::floor(inches / 12.0 + 1e-9));
    double rest = inches - feet * 12.0;
    // Nearest 1/8 inch.
    int eighths = static_cast<int>(std::lround(rest * 8.0));
    if (eighths >= 96) {
      ++feet;
      eighths -= 96;
    }
    int whole = eighths / 8, frac = eighths % 8;
    QString s = QString("%1%2'").arg(neg ? "-" : "").arg(feet);
    s += QString::number(whole);
    if (frac) {
      int num = frac, den = 8;
      while (num % 2 == 0) {
        num /= 2;
        den /= 2;
      }
      s += QString(" %1/%2").arg(num).arg(den);
    }
    return s + "\"";
  }
  double d = toDisplay(v);
  QString s = QString::number(d, 'f', decimals);
  // Trim trailing zeros but keep one decimal, like "10.0mm".
  if (s.contains('.')) {
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s += '0';
  }
  return s + suffix();
}

bool Units::parse(const QString& textIn, double* out) const {
  QString text = textIn.trimmed();
  text.remove(' ');
  if (text.isEmpty()) return false;
  // Feet and inches: 1'6", 1'6-1/2", 1', 6".
  static const QRegularExpression feetInch(R"(^(-?\d+(?:\.\d+)?)'(?:(\d+(?:\.\d+)?)(?:-?(\d+)/(\d+))?\"?)?$)");
  QRegularExpressionMatch m = feetInch.match(text);
  if (m.hasMatch()) {
    double inches = m.captured(1).toDouble() * 12.0;
    double sign = inches < 0 || text.startsWith('-') ? -1.0 : 1.0;
    inches = std::fabs(inches);
    if (!m.captured(2).isEmpty()) inches += m.captured(2).toDouble();
    if (!m.captured(3).isEmpty() && m.captured(4).toDouble() != 0)
      inches += m.captured(3).toDouble() / m.captured(4).toDouble();
    *out = sign * inches * 2.54 / systemCm_;
    return true;
  }
  static const QRegularExpression numUnit(R"(^([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)([a-zA-Z"']*)$)");
  m = numUnit.match(text);
  if (!m.hasMatch()) return false;
  bool ok = false;
  double value = m.captured(1).toDouble(&ok);
  if (!ok) return false;
  QString u = m.captured(2).toLower();
  double cm;
  if (u.isEmpty()) {
    *out = fromDisplay(value);
    return true;
  }
  if (u == "mm") cm = value * 0.1;
  else if (u == "cm") cm = value;
  else if (u == "m") cm = value * 100.0;
  else if (u == "km") cm = value * 100000.0;
  else if (u == "in" || u == "\"") cm = value * 2.54;
  else if (u == "ft" || u == "'") cm = value * 30.48;
  else if (u == "u") {
    *out = value;
    return true;
  } else {
    return false;
  }
  *out = cm / systemCm_;
  return true;
}

void Units::load() {
  QSettings s("Veraa", "Veraa");
  display_ = static_cast<DisplayUnits>(s.value("units/display", static_cast<int>(DisplayUnits::Centimeters)).toInt());
  systemCm_ = s.value("units/systemCm", 1.0).toDouble();
  if (systemCm_ <= 0) systemCm_ = 1.0;
}

void Units::save() const {
  QSettings s("Veraa", "Veraa");
  s.setValue("units/display", static_cast<int>(display_));
  s.setValue("units/systemCm", systemCm_);
}

bool isLengthParam(const ParamDef& d) {
  if (d.type != ParamType::Float) return false;
  if (!d.unit.empty()) return d.unit == "length";
  static const char* const keys[] = {"length", "width", "height", "radius", "radius1", "radius2", "depth",
                                     "thickness", "fillet", "inner", "outer", "focusDistance", "offset",
                                     "lengthSize", "widthSize", "heightSize"};
  for (const char* k : keys)
    if (d.key == k) return true;
  return false;
}

UnitSpinBox::UnitSpinBox(QWidget* parent) : QDoubleSpinBox(parent) {
  connect(&Units::instance(), &Units::changed, this, [this] {
    // Re-render the text in the new unit without changing the stored value.
    double v = value();
    blockSignals(true);
    setValue(v + 1.0);
    setValue(v);
    blockSignals(false);
  });
}

void UnitSpinBox::setLengthMode(bool on) {
  if (length_ == on) return;
  length_ = on;
  double v = value();
  blockSignals(true);
  setValue(v + 1.0);
  setValue(v);
  blockSignals(false);
}

QString UnitSpinBox::textFromValue(double value) const {
  if (!length_) return QDoubleSpinBox::textFromValue(value);
  return Units::instance().format(value, decimals());
}

double UnitSpinBox::valueFromText(const QString& text) const {
  if (!length_) return QDoubleSpinBox::valueFromText(text);
  double v = 0;
  if (Units::instance().parse(text, &v)) return v;
  return value();
}

QValidator::State UnitSpinBox::validate(QString& text, int& pos) const {
  if (!length_) return QDoubleSpinBox::validate(text, pos);
  double v;
  if (Units::instance().parse(text, &v)) return QValidator::Acceptable;
  static const QRegularExpression partial(R"(^[-+]?[\d.]*[a-zA-Z"'/ \-\d]*$)");
  return partial.match(text).hasMatch() ? QValidator::Intermediate : QValidator::Invalid;
}

void UnitSpinBox::fixup(QString& input) const {
  if (!length_) {
    QDoubleSpinBox::fixup(input);
    return;
  }
  double v;
  if (!Units::instance().parse(input, &v)) input = textFromValue(value());
}

}  // namespace veraa
