// Display units (Customize > Units Setup). Scene values are stored in system units; one system
// unit is `systemUnitCm` centimeters (1 by default). Length fields show and accept values in
// the chosen display unit, e.g. "25.4mm", "1'6\"", "0.5m".
#pragma once

#include <QDoubleSpinBox>
#include <QObject>
#include <QString>

namespace veraa {

struct ParamDef;

enum class DisplayUnits { Generic, Millimeters, Centimeters, Meters, Kilometers, Inches, Feet, FeetInches };

class Units : public QObject {
  Q_OBJECT
 public:
  static Units& instance();

  DisplayUnits display() const { return display_; }
  double systemUnitCm() const { return systemCm_; }
  void setDisplay(DisplayUnits u);
  void setSystemUnitCm(double cm);

  // System units <-> display unit value.
  double toDisplay(double systemValue) const;
  double fromDisplay(double displayValue) const;
  QString suffix() const;  // "mm", "cm", "m", "km", "\"", "'", "" (generic / feet+inches)
  // Formats a length in system units, e.g. "10.0mm" or "1'6 1/2\"".
  QString format(double systemValue, int decimals = 3) const;
  // Parses user text; accepts any unit suffix (mm, cm, m, km, in, ", ft, ', feet-inch forms) and
  // plain numbers in the display unit. Returns system units.
  bool parse(const QString& text, double* systemValue) const;

  static QString unitName(DisplayUnits u);

  void load();
  void save() const;

 Q_SIGNALS:
  void changed();

 private:
  Units();
  double displayPerCm() const;
  DisplayUnits display_ = DisplayUnits::Centimeters;
  double systemCm_ = 1.0;
};

// True when a parameter holds a length (explicit ParamDef::unit or a common length key).
bool isLengthParam(const ParamDef& d);

// Double spin box whose value is in system units but which shows and accepts display units.
class UnitSpinBox : public QDoubleSpinBox {
  Q_OBJECT
 public:
  explicit UnitSpinBox(QWidget* parent = nullptr);
  // Length mode (default) shows display units; otherwise it is a plain number box (angles,
  // percentages) that still shares the type-in styling.
  void setLengthMode(bool on);
  bool lengthMode() const { return length_; }
  QString textFromValue(double value) const override;
  double valueFromText(const QString& text) const override;
  QValidator::State validate(QString& text, int& pos) const override;
  void fixup(QString& input) const override;

 private:
  bool length_ = true;
};

}  // namespace veraa
