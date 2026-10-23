#include "icons_ui.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>
#include <functional>

#include "theme.h"

namespace veraa::icons {

namespace {

using DrawFn = std::function<void(QPainter&)>;

QIcon make(const DrawFn& draw) {
  QIcon icon;
  for (int scale : {1, 2}) {
    QPixmap pm(24 * scale, 24 * scale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(scale, scale);
    draw(p);
    p.end();
    pm.setDevicePixelRatio(scale);
    icon.addPixmap(pm);
  }
  return icon;
}

const QColor kInk(0xdc, 0xdc, 0xdc);
const QColor kDim(0x9a, 0x9a, 0x9a);
const QColor kBlue(0x6f, 0xb0, 0xf0);
const QColor kYellow(0xf0, 0xc8, 0x40);

QPen pen(const QColor& c, qreal w = 1.6, Qt::PenStyle style = Qt::SolidLine) {
  QPen p(c, w, style);
  p.setCapStyle(Qt::RoundCap);
  p.setJoinStyle(Qt::RoundJoin);
  return p;
}

void arrowHead(QPainter& p, QPointF tip, QPointF dir, qreal size = 4.0) {
  QPointF n(-dir.y(), dir.x());
  QPolygonF tri;
  tri << tip << tip - dir * size + n * (size * 0.65) << tip - dir * size - n * (size * 0.65);
  p.drawPolygon(tri);
}

void cursorArrow(QPainter& p, QPointF at, qreal s = 1.0) {
  QPolygonF a;
  a << at << at + QPointF(0, 10 * s) << at + QPointF(2.6 * s, 7.6 * s) << at + QPointF(4.6 * s, 11.4 * s)
    << at + QPointF(6.2 * s, 10.6 * s) << at + QPointF(4.3 * s, 6.9 * s) << at + QPointF(7.6 * s, 6.6 * s);
  p.setPen(pen(QColor(30, 30, 30), 1.0));
  p.setBrush(kInk);
  p.drawPolygon(a);
}

void magnet(QPainter& p, QPointF c, qreal s, const QColor& col) {
  QPainterPath path;
  path.moveTo(c + QPointF(-s, -s));
  path.lineTo(c + QPointF(-s, 0));
  path.arcTo(QRectF(c.x() - s, c.y() - s, 2 * s, 2 * s), 180, 180);
  path.lineTo(c + QPointF(s, -s));
  p.setBrush(Qt::NoBrush);
  p.setPen(pen(col, 2.6));
  p.drawPath(path);
  p.setPen(pen(QColor(0xe0, 0x50, 0x50), 2.6));
  p.drawLine(c + QPointF(-s, -s), c + QPointF(-s, -s * 0.45));
  p.drawLine(c + QPointF(s, -s), c + QPointF(s, -s * 0.45));
}

void magnifier(QPainter& p, const QColor& c = kInk) {
  p.setPen(pen(c, 1.8));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QRectF(3, 3, 12, 12));
  p.setPen(pen(c, 3.0));
  p.drawLine(QPointF(14, 14), QPointF(20.5, 20.5));
}

void miniBox(QPainter& p, QRectF r, const QColor& c) {
  p.setPen(pen(c, 1.2));
  p.setBrush(c.darker(220));
  p.drawRect(r);
}

}  // namespace

QIcon selectByName() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.2));
    p.setBrush(QColor(0x30, 0x30, 0x30));
    p.drawRect(QRectF(3, 3, 14, 17));
    p.setPen(pen(kInk, 1.4));
    for (int i = 0; i < 4; ++i) p.drawLine(QPointF(5.5, 6.5 + i * 3.8), QPointF(14, 6.5 + i * 3.8));
    cursorArrow(p, QPointF(13, 11), 0.95);
  });
}

QIcon place() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.2));
    p.setBrush(QColor(0x50, 0x50, 0x50));
    QPolygonF ground;
    ground << QPointF(2, 19) << QPointF(12, 15) << QPointF(22, 19) << QPointF(12, 23);
    p.drawPolygon(ground);
    miniBox(p, QRectF(8, 6, 8, 9), kBlue);
    p.setPen(pen(kYellow, 1.6));
    p.drawLine(QPointF(12, 15), QPointF(12, 19));
    p.setBrush(kYellow);
    arrowHead(p, QPointF(12, 20), QPointF(0, 1), 3.5);
  });
}

QIcon scaleNonUniform() {
  return make([](QPainter& p) {
    miniBox(p, QRectF(4, 8, 16, 8), kBlue);
    p.setPen(pen(kInk, 1.4));
    p.setBrush(kInk);
    p.drawLine(QPointF(1, 12), QPointF(23, 12));
    arrowHead(p, QPointF(1, 12), QPointF(-1, 0), 3.5);
    arrowHead(p, QPointF(23, 12), QPointF(1, 0), 3.5);
  });
}

QIcon squash() {
  return make([](QPainter& p) {
    QPainterPath body;
    body.moveTo(4, 14);
    body.cubicTo(4, 9, 20, 9, 20, 14);
    body.cubicTo(20, 19, 4, 19, 4, 14);
    p.setPen(pen(kBlue, 1.2));
    p.setBrush(kBlue.darker(220));
    p.drawPath(body);
    p.setPen(pen(kInk, 1.4));
    p.setBrush(kInk);
    p.drawLine(QPointF(12, 1.5), QPointF(12, 7));
    arrowHead(p, QPointF(12, 8), QPointF(0, 1), 3.2);
    p.drawLine(QPointF(1, 14), QPointF(2.5, 14));
    p.drawLine(QPointF(21.5, 14), QPointF(23, 14));
  });
}

QIcon region(int shape) {
  return make([shape](QPainter& p) {
    p.setPen(pen(kInk, 1.3, Qt::DashLine));
    p.setBrush(QColor(0x6f, 0xb0, 0xf0, 50));
    switch (shape) {
      case 0: p.drawRect(QRectF(3, 4, 15, 12)); break;
      case 1: p.drawEllipse(QRectF(3, 2, 15, 15)); break;
      case 2: {
        QPolygonF f;
        f << QPointF(3, 5) << QPointF(13, 2) << QPointF(19, 9) << QPointF(12, 17) << QPointF(5, 13);
        p.drawPolygon(f);
        break;
      }
      case 3: {
        QPainterPath l;
        l.moveTo(4, 9);
        l.cubicTo(2, 2, 16, 0, 18, 6);
        l.cubicTo(20, 12, 14, 17, 9, 15);
        l.cubicTo(5, 14, 6, 11, 4, 9);
        p.drawPath(l);
        break;
      }
      default: {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x6f, 0xb0, 0xf0, 120));
        for (int i = 0; i < 5; ++i) p.drawEllipse(QPointF(5 + i * 2.8, 13 - std::sin(i * 0.9) * 5), 3.0, 3.0);
        p.setPen(pen(kInk, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(16.5, 9), 3.6, 3.6);
        break;
      }
    }
    cursorArrow(p, QPointF(14, 12), 0.85);
  });
}

QIcon windowCrossing(bool window) {
  return make([window](QPainter& p) {
    p.setPen(pen(kInk, 1.3, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(3, 3, 13, 13));
    p.setPen(pen(kBlue, 1.3));
    p.setBrush(kBlue.darker(200));
    if (window) p.drawEllipse(QRectF(6, 6, 7, 7));
    else p.drawEllipse(QRectF(11, 11, 10, 10));
  });
}

QIcon lock() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(7, 3, 10, 12), 0, 180 * 16);
    p.drawLine(QPointF(7, 9), QPointF(7, 11));
    p.drawLine(QPointF(17, 9), QPointF(17, 11));
    p.setPen(pen(kInk, 1.2));
    p.setBrush(kYellow.darker(130));
    p.drawRoundedRect(QRectF(5, 11, 14, 10), 2, 2);
    p.setPen(pen(QColor(40, 40, 40), 1.6));
    p.drawLine(QPointF(12, 14.5), QPointF(12, 17.5));
  });
}

QIcon axisCycle() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(3, 3, 18, 18), 30 * 16, 300 * 16);
    p.setBrush(kInk);
    arrowHead(p, QPointF(19.6, 7.2), QPointF(0.5, -0.86), 3.5);
    QFont f;
    f.setPixelSize(8);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kYellow);
    p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, "XY");
  });
}

QIcon frameWindow() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x2a, 0x2a, 0x2a));
    p.drawRect(QRectF(2, 4, 20, 16));
    p.setBrush(kInk);
    p.drawRect(QRectF(2, 4, 20, 3));
    QLinearGradient g(QPointF(4, 8), QPointF(20, 19));
    g.setColorAt(0, QColor(0x80, 0xb8, 0xf0));
    g.setColorAt(1, QColor(0xe0, 0x90, 0x40));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRect(QRectF(4, 8.5, 16, 10));
  });
}

QIcon zoomExtentsAll() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    p.setBrush(QColor(0x40, 0x40, 0x40));
    for (int i = 0; i < 4; ++i) p.drawRect(QRectF(2 + (i % 2) * 11, 2 + (i / 2) * 11, 9, 9));
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 4; ++i) {
      QRectF r(4 + (i % 2) * 11, 4 + (i / 2) * 11, 5, 5);
      p.drawRect(r);
    }
  });
}

QIcon sceneExplorer() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.2));
    p.drawLine(QPointF(5, 6), QPointF(5, 19));
    p.drawLine(QPointF(5, 12), QPointF(9, 12));
    p.drawLine(QPointF(5, 19), QPointF(9, 19));
    miniBox(p, QRectF(2.5, 2.5, 5, 5), kBlue);
    miniBox(p, QRectF(10, 9.5, 5, 5), kInk);
    miniBox(p, QRectF(10, 16.5, 5, 5), kInk);
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(10, 5), QPointF(21, 5));
    p.drawLine(QPointF(17, 12), QPointF(22, 12));
    p.drawLine(QPointF(17, 19), QPointF(22, 19));
  });
}

QIcon layerExplorer() {
  return make([](QPainter& p) {
    for (int i = 2; i >= 0; --i) {
      QPolygonF layer;
      double y = 5 + i * 5;
      layer << QPointF(12, y - 3) << QPointF(21, y + 1) << QPointF(12, y + 5) << QPointF(3, y + 1);
      p.setPen(pen(kInk, 1.2));
      p.setBrush(i == 0 ? kBlue.darker(140) : QColor(0x50, 0x50, 0x50));
      p.drawPolygon(layer);
    }
  });
}

QIcon mirror() {
  return make([](QPainter& p) {
    p.setPen(pen(kYellow, 1.2, Qt::DashLine));
    p.drawLine(QPointF(12, 2), QPointF(12, 22));
    QPolygonF a, b;
    a << QPointF(10, 6) << QPointF(10, 18) << QPointF(3, 18);
    b << QPointF(14, 6) << QPointF(14, 18) << QPointF(21, 18);
    p.setPen(pen(kInk, 1.2));
    p.setBrush(kBlue.darker(130));
    p.drawPolygon(a);
    p.setBrush(QColor(0x50, 0x50, 0x50));
    p.drawPolygon(b);
  });
}

QIcon array() {
  return make([](QPainter& p) {
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x) miniBox(p, QRectF(2.5 + x * 7, 2.5 + y * 7, 5, 5), x == 0 && y == 0 ? kBlue : kInk);
  });
}

QIcon align() {
  return make([](QPainter& p) {
    p.setPen(pen(kYellow, 1.4));
    p.drawLine(QPointF(3, 21), QPointF(21, 21));
    miniBox(p, QRectF(4, 9, 7, 12), kBlue);
    miniBox(p, QRectF(13, 13, 7, 8), kInk);
    p.setPen(pen(kInk, 1.2));
    p.setBrush(kInk);
    p.drawLine(QPointF(16.5, 3), QPointF(16.5, 10));
    arrowHead(p, QPointF(16.5, 11.5), QPointF(0, 1), 3.0);
  });
}

QIcon snap(int dims) {
  return make([dims](QPainter& p) {
    magnet(p, QPointF(10, 9), 5.5, kInk);
    QFont f;
    f.setPixelSize(dims == 25 ? 8 : 10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kYellow);
    p.drawText(QRectF(10, 12, 14, 12), Qt::AlignCenter, dims == 25 ? "2.5" : QString::number(dims));
  });
}

QIcon angleSnap() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(3, 20), QPointF(21, 20));
    p.drawLine(QPointF(3, 20), QPointF(17, 6));
    p.setPen(pen(kYellow, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(-5, 12, 16, 16), 0, 45 * 16);
    magnet(p, QPointF(17, 14), 3.5, kInk);
  });
}

QIcon percentSnap() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 3, 6, 6));
    p.drawEllipse(QRectF(10, 10, 6, 6));
    p.drawLine(QPointF(15, 3), QPointF(4, 17));
    magnet(p, QPointF(19, 17), 3.5, kInk);
  });
}

QIcon spinnerSnap() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.2));
    p.setBrush(QColor(0x30, 0x30, 0x30));
    p.drawRect(QRectF(2, 7, 12, 10));
    p.setPen(pen(kInk, 1.2));
    p.setBrush(kInk);
    QPolygonF up, down;
    up << QPointF(8, 8.5) << QPointF(11, 11) << QPointF(5, 11);
    down << QPointF(8, 15.5) << QPointF(11, 13) << QPointF(5, 13);
    p.drawPolygon(up);
    p.drawPolygon(down);
    magnet(p, QPointF(18.5, 13), 3.5, kInk);
  });
}

QIcon curveEditor() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    p.drawLine(QPointF(3, 3), QPointF(3, 21));
    p.drawLine(QPointF(3, 21), QPointF(21, 21));
    QPainterPath c;
    c.moveTo(4, 18);
    c.cubicTo(9, 18, 9, 5, 13, 5);
    c.cubicTo(17, 5, 17, 14, 21, 12);
    p.setPen(pen(kYellow, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawPath(c);
    p.setPen(pen(kInk, 1.0));
    p.setBrush(kInk);
    p.drawRect(QRectF(11.5, 3.5, 3, 3));
  });
}

QIcon schematic() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.3));
    p.drawLine(QPointF(7, 8), QPointF(7, 16));
    p.drawLine(QPointF(7, 16), QPointF(15, 16));
    p.drawLine(QPointF(7, 8), QPointF(15, 8));
    miniBox(p, QRectF(2, 4, 9, 6), kBlue);
    miniBox(p, QRectF(13, 5, 9, 6), kInk);
    miniBox(p, QRectF(13, 13, 9, 6), kInk);
  });
}

QIcon hierarchy() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(12, 7), QPointF(12, 12));
    p.drawLine(QPointF(5, 12), QPointF(19, 12));
    p.drawLine(QPointF(5, 12), QPointF(5, 16));
    p.drawLine(QPointF(19, 12), QPointF(19, 16));
    p.setBrush(kBlue);
    p.drawEllipse(QPointF(12, 5), 3, 3);
    p.setBrush(kInk.darker(140));
    p.drawEllipse(QPointF(5, 18.5), 2.8, 2.8);
    p.drawEllipse(QPointF(19, 18.5), 2.8, 2.8);
  });
}

QIcon display() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(QColor(0x2c, 0x3c, 0x50));
    p.drawRoundedRect(QRectF(2.5, 3.5, 19, 13), 1.5, 1.5);
    p.drawLine(QPointF(12, 16.5), QPointF(12, 20));
    p.drawLine(QPointF(7, 20.5), QPointF(17, 20.5));
    p.setPen(Qt::NoPen);
    p.setBrush(kBlue);
    p.drawEllipse(QPointF(12, 10), 3.5, 3.5);
  });
}

QIcon utilities() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 3.0));
    p.drawLine(QPointF(5, 19), QPointF(14, 10));
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    QPainterPath head;
    head.arcMoveTo(QRectF(12, 3, 9, 9), 200);
    head.arcTo(QRectF(12, 3, 9, 9), 200, 290);
    p.drawPath(head);
  });
}

QIcon link() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2.0));
    p.setBrush(Qt::NoBrush);
    p.save();
    p.translate(12, 12);
    p.rotate(-45);
    p.drawRoundedRect(QRectF(-10, -3.5, 11, 7), 3.5, 3.5);
    p.drawRoundedRect(QRectF(-1, -3.5, 11, 7), 3.5, 3.5);
    p.restore();
  });
}

QIcon unlink() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2.0));
    p.setBrush(Qt::NoBrush);
    p.save();
    p.translate(12, 12);
    p.rotate(-45);
    p.drawRoundedRect(QRectF(-11, -3.5, 9, 7), 3.5, 3.5);
    p.drawRoundedRect(QRectF(2, -3.5, 9, 7), 3.5, 3.5);
    p.restore();
    p.setPen(pen(QColor(0xe0, 0x50, 0x50), 1.6));
    p.drawLine(QPointF(14, 3), QPointF(16, 7));
    p.drawLine(QPointF(8, 17), QPointF(10, 21));
  });
}

QIcon bindSpaceWarp() {
  return make([](QPainter& p) {
    QPainterPath wave;
    wave.moveTo(2, 18);
    for (int i = 0; i <= 8; ++i) wave.lineTo(2 + i * 2.5, 18 + std::sin(i * 1.4) * 2.5);
    p.setPen(pen(kBlue, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(wave);
    p.setPen(pen(kInk, 1.8));
    p.save();
    p.translate(12, 8);
    p.rotate(-30);
    p.drawRoundedRect(QRectF(-8, -3, 9, 6), 3, 3);
    p.drawRoundedRect(QRectF(-1, -3, 9, 6), 3, 3);
    p.restore();
  });
}

QIcon manipulate() {
  return make([](QPainter& p) {
    p.setPen(pen(kYellow, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 3, 14, 14));
    p.setPen(pen(kBlue, 1.6));
    p.drawLine(QPointF(10, 10), QPointF(10, 3));
    p.setBrush(kYellow);
    p.drawEllipse(QPointF(10, 3), 2.0, 2.0);
    cursorArrow(p, QPointF(13, 11), 0.9);
  });
}

QIcon keyboardOverride() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x40, 0x40, 0x40));
    p.drawRoundedRect(QRectF(2, 6, 20, 12), 2, 2);
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    for (int r = 0; r < 2; ++r)
      for (int c = 0; c < 6; ++c) p.drawRect(QRectF(4 + c * 2.8, 8.5 + r * 3, 1.8, 1.8));
    p.drawRect(QRectF(7, 14.5, 10, 1.6));
  });
}

QIcon namedSets() {
  return make([](QPainter& p) {
    QFont f;
    f.setPixelSize(18);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kInk);
    p.drawText(QRectF(0, -1, 24, 24), Qt::AlignCenter, "{ }");
    p.setPen(Qt::NoPen);
    p.setBrush(kYellow);
    p.drawEllipse(QPointF(12, 12), 2.2, 2.2);
  });
}

QIcon pivotCenter(int mode) {
  return make([mode](QPainter& p) {
    auto cross = [&](QPointF c, const QColor& col) {
      p.setPen(pen(col, 1.4));
      p.drawLine(c - QPointF(3, 0), c + QPointF(3, 0));
      p.drawLine(c - QPointF(0, 3), c + QPointF(0, 3));
    };
    if (mode == 2) {
      p.setPen(pen(kDim, 1.0));
      p.drawLine(QPointF(2, 21), QPointF(22, 21));
      p.drawLine(QPointF(3, 2), QPointF(3, 22));
    }
    miniBox(p, QRectF(4, 5, 7, 7), kInk);
    miniBox(p, QRectF(13, 11, 7, 7), kInk);
    if (mode == 0) {
      cross(QPointF(7.5, 8.5), kYellow);
      cross(QPointF(16.5, 14.5), kYellow);
    } else if (mode == 1) {
      cross(QPointF(12, 11.5), kYellow);
    } else {
      cross(QPointF(3, 21), kYellow);
    }
  });
}

QIcon isolate() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.0, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(2, 2, 20, 20));
    p.setPen(pen(kYellow, 1.4));
    p.setBrush(kYellow.darker(160));
    p.drawEllipse(QRectF(7, 4, 10, 10));
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(10, 16), QPointF(14, 16));
    p.drawLine(QPointF(10, 18.5), QPointF(14, 18.5));
  });
}

QIcon zoom() { return make([](QPainter& p) { magnifier(p); }); }

QIcon zoomAll() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 4; ++i) p.drawRect(QRectF(1 + (i % 2) * 12, 1 + (i / 2) * 12, 10, 10));
    magnifier(p);
  });
}

QIcon zoomRegion() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.2, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(2, 2, 12, 9));
    p.save();
    p.translate(5, 5);
    p.scale(0.8, 0.8);
    magnifier(p);
    p.restore();
  });
}

QIcon fieldOfView() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(QColor(0x6f, 0xb0, 0xf0, 60));
    QPolygonF cone;
    cone << QPointF(3, 12) << QPointF(21, 3) << QPointF(21, 21);
    p.drawPolygon(cone);
    p.setBrush(Qt::NoBrush);
    p.setPen(pen(kYellow, 1.2));
    p.drawArc(QRectF(-4, 5, 14, 14), -30 * 16, 60 * 16);
  });
}

QIcon pan() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(kInk.darker(200));
    QPainterPath hand;
    hand.moveTo(7, 21);
    hand.lineTo(5, 14);
    hand.lineTo(4, 9);
    hand.lineTo(6.5, 9);
    hand.lineTo(8, 13);
    hand.lineTo(8, 4);
    hand.lineTo(10.5, 4);
    hand.lineTo(10.5, 11);
    hand.lineTo(11, 3);
    hand.lineTo(13.5, 3);
    hand.lineTo(13.5, 11);
    hand.lineTo(14.5, 4.5);
    hand.lineTo(17, 4.5);
    hand.lineTo(16.5, 12);
    hand.lineTo(18, 8);
    hand.lineTo(20, 8.5);
    hand.lineTo(18, 17);
    hand.lineTo(16, 21);
    hand.closeSubpath();
    p.drawPath(hand);
  });
}

QIcon orbit(int mode) {
  return make([mode](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 7, 18, 10));
    p.setBrush(kInk);
    arrowHead(p, QPointF(20.5, 13), QPointF(0.3, 1), 3.5);
    QColor center = mode == 0 ? kInk : mode == 1 ? kBlue : kYellow;
    p.setPen(pen(center, 1.2));
    p.setBrush(center.darker(mode == 0 ? 250 : 130));
    p.drawEllipse(QPointF(12, 12), 3.5, 3.5);
  });
}

QIcon home() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.5));
    p.setBrush(QColor(0x50, 0x50, 0x50));
    QPolygonF h;
    h << QPointF(12, 3) << QPointF(21, 11) << QPointF(18.5, 11) << QPointF(18.5, 20) << QPointF(5.5, 20)
      << QPointF(5.5, 11) << QPointF(3, 11);
    p.drawPolygon(h);
  });
}

QIcon script() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.2));
    p.setBrush(QColor(0x30, 0x30, 0x30));
    p.drawRect(QRectF(3, 3, 18, 18));
    QFont f;
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kYellow);
    p.drawText(QRectF(3, 3, 18, 18), Qt::AlignCenter, ">_");
  });
}

QIcon absoluteMode(bool absolute) {
  return make([absolute](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    p.drawLine(QPointF(4, 20), QPointF(20, 20));
    p.drawLine(QPointF(4, 20), QPointF(4, 4));
    p.setPen(pen(absolute ? kYellow : kBlue, 1.6));
    p.setBrush(absolute ? kYellow : kBlue);
    if (absolute) {
      p.drawLine(QPointF(4, 20), QPointF(16, 8));
      arrowHead(p, QPointF(17, 7), QPointF(0.707, -0.707), 3.5);
    } else {
      p.drawLine(QPointF(10, 16), QPointF(18, 8));
      arrowHead(p, QPointF(19, 7), QPointF(0.707, -0.707), 3.5);
      p.setBrush(Qt::NoBrush);
      p.drawEllipse(QPointF(9, 17), 1.6, 1.6);
    }
  });
}

QIcon adaptive() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 3, 18, 18));
    p.setPen(pen(kYellow, 2.0));
    p.drawLine(QPointF(12, 12), QPointF(17, 7));
    p.setPen(pen(kDim, 1.0));
    for (int i = 0; i < 5; ++i) {
      double a = (200 + i * 35) * 3.14159265 / 180.0;
      p.drawLine(QPointF(12 + std::cos(a) * 6.5, 12 - std::sin(a) * 6.5), QPointF(12 + std::cos(a) * 8.5, 12 - std::sin(a) * 8.5));
    }
  });
}

// ---- Layout additions ----

QIcon snapAxisConstraint() {
  return make([](QPainter& p) {
    QFont f;
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.setPen(kInk);
    p.drawText(QRectF(1, 3, 12, 12), Qt::AlignCenter, "X");
    magnet(p, QPointF(16, 14), 4.0, kInk);
    p.setPen(pen(kYellow, 1.4));
    p.drawLine(QPointF(2, 20), QPointF(12, 20));
  });
}

QIcon slateEditor() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.2));
    p.drawLine(QPointF(8, 8), QPointF(14, 14));
    p.drawLine(QPointF(8, 17), QPointF(14, 14));
    miniBox(p, QRectF(2, 4, 8, 6), kInk);
    miniBox(p, QRectF(2, 14, 8, 6), kInk);
    QRadialGradient g(QPointF(17, 12), 6, QPointF(15, 10));
    g.setColorAt(0, QColor(0xf0, 0xf0, 0xf0));
    g.setColorAt(1, QColor(0x50, 0x80, 0xc0));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(QPointF(17, 14), 5, 5);
  });
}

QIcon activeShade() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x2a, 0x2a, 0x2a));
    p.drawRect(QRectF(2, 4, 20, 16));
    QRadialGradient g(QPointF(10, 12), 7, QPointF(8, 10));
    g.setColorAt(0, QColor(0xf8, 0xe0, 0x90));
    g.setColorAt(1, QColor(0xc0, 0x60, 0x20));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(QPointF(10, 12), 5.5, 5.5);
    p.setPen(pen(kBlue, 1.6));
    p.drawArc(QRectF(13, 8, 7, 8), 300 * 16, 240 * 16);
  });
}

QIcon renderIterative() {
  return make([](QPainter& p) {
    QRadialGradient g(QPointF(10, 13), 8, QPointF(8, 10));
    g.setColorAt(0, QColor(0xf0, 0xf0, 0xf0));
    g.setColorAt(1, QColor(0x70, 0x70, 0x70));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(QPointF(10, 13), 7, 6);
    p.setPen(pen(kYellow, 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(13, 3, 9, 9), 30 * 16, 270 * 16);
    p.setBrush(kYellow);
    arrowHead(p, QPointF(21.5, 6.5), QPointF(0.2, 1.0), 3.0);
  });
}

QIcon ribbon() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.2));
    p.setBrush(QColor(0x30, 0x30, 0x30));
    p.drawRect(QRectF(2, 4, 20, 16));
    p.setBrush(kInk);
    p.drawRect(QRectF(2, 4, 20, 3));
    p.setBrush(QColor(0x58, 0x58, 0x58));
    p.drawRect(QRectF(4, 9, 7, 4));
    p.drawRect(QRectF(13, 9, 7, 4));
    p.drawRect(QRectF(4, 15, 7, 3));
    p.drawRect(QRectF(13, 15, 7, 3));
  });
}

QIcon safeScript() {
  return make([](QPainter& p) {
    QPainterPath s;
    s.moveTo(12, 2.5);
    s.lineTo(20, 5.5);
    s.cubicTo(20, 13, 17, 18.5, 12, 21.5);
    s.cubicTo(7, 18.5, 4, 13, 4, 5.5);
    s.closeSubpath();
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x38, 0x38, 0x38));
    p.drawPath(s);
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    p.drawRect(QRectF(12, 5, 6, 7));
    p.drawRect(QRectF(6, 12, 6, 6));
  });
}

QIcon timeTag() {
  return make([](QPainter& p) {
    QPolygonF tag;
    tag << QPointF(3, 8) << QPointF(9, 3) << QPointF(21, 3) << QPointF(21, 13) << QPointF(9, 13);
    p.setPen(pen(kInk, 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(tag);
    p.drawEllipse(QPointF(8, 8), 1.5, 1.5);
    p.setPen(pen(kYellow, 1.6));
    p.drawLine(QPointF(12, 16), QPointF(12, 22));
    p.drawLine(QPointF(9, 19), QPointF(15, 19));
  });
}

QIcon miniCurveEditor() {
  return make([](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    for (int i = 0; i < 4; ++i) p.drawLine(QPointF(3, 5 + i * 4), QPointF(6, 5 + i * 4));
    QPainterPath c;
    c.moveTo(8, 16);
    c.cubicTo(11, 16, 11, 6, 14, 6);
    c.cubicTo(17, 6, 17, 18, 21, 14);
    p.setPen(pen(QColor(0x40, 0xc8, 0xb0), 1.7));
    p.setBrush(Qt::NoBrush);
    p.drawPath(c);
  });
}

QIcon tangent(int kind) {
  return make([kind](QPainter& p) {
    p.setPen(pen(kDim, 1.0));
    p.drawLine(QPointF(3, 21), QPointF(21, 21));
    p.setPen(pen(kInk, 1.7));
    p.setBrush(Qt::NoBrush);
    if (kind == 1) {
      p.drawLine(QPointF(4, 18), QPointF(12, 8));
      p.drawLine(QPointF(12, 8), QPointF(20, 16));
    } else if (kind == 2) {
      p.drawLine(QPointF(4, 18), QPointF(12, 18));
      p.drawLine(QPointF(12, 18), QPointF(12, 7));
      p.drawLine(QPointF(12, 7), QPointF(20, 7));
    } else {
      QPainterPath c;
      c.moveTo(4, 18);
      c.cubicTo(8, 18, 9, 8, 12, 8);
      c.cubicTo(15, 8, 16, 16, 20, 16);
      p.drawPath(c);
    }
    p.setPen(pen(kind == 0 ? kYellow : kBlue, 1.0));
    p.setBrush(kind == 0 ? kYellow : kBlue);
    p.drawRect(QRectF(10.5, 6.5, 3, 3));
  });
}

}  // namespace veraa::icons
