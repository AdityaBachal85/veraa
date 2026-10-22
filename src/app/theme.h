// Dark theme and shared colors, close to modern 3ds Max.
#pragma once

#include <QColor>

class QApplication;

namespace veraa {

void applyDarkTheme(QApplication& app);

namespace theme {
inline QColor viewportBg() { return QColor(0x3f, 0x3f, 0x3f); }
inline QColor viewportBgTop() { return QColor(0x52, 0x55, 0x5a); }
inline QColor viewportBgBottom() { return QColor(0x2c, 0x2d, 0x2f); }
inline QColor gridMinor() { return QColor(0x56, 0x56, 0x56); }
inline QColor gridMajor() { return QColor(0x6a, 0x6a, 0x6a); }
inline QColor gridAxis() { return QColor(0x1c, 0x1c, 0x1c); }
inline QColor activeBorder() { return QColor(0xf5, 0xc5, 0x18); }
inline QColor selection() { return QColor(0xff, 0xff, 0xff); }
inline QColor accent() { return QColor(0x4f, 0x9b, 0xe0); }
inline QColor axisX() { return QColor(0xe0, 0x40, 0x40); }
inline QColor axisY() { return QColor(0x4c, 0xc8, 0x4c); }
inline QColor axisZ() { return QColor(0x45, 0x78, 0xf0); }
inline QColor gizmoHot() { return QColor(0xff, 0xe4, 0x2a); }
}  // namespace theme

}  // namespace veraa
