#include "animicons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>
#include <functional>

namespace veraa::animicons {
namespace {

const QColor kInk(0xdc, 0xdc, 0xdc);
const QColor kBlue(0x6f, 0xb0, 0xf0);
const QColor kRed(0xe0, 0x50, 0x48);
const QColor kGreen(0x5c, 0xc8, 0x5c);

QIcon make(const std::function<void(QPainter&)>& draw) {
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

QPen pen(const QColor& c, qreal w = 1.6) {
  QPen p(c, w);
  p.setCapStyle(Qt::RoundCap);
  p.setJoinStyle(Qt::RoundJoin);
  return p;
}

void tri(QPainter& p, qreal x0, qreal x1, bool right) {
  QPolygonF poly;
  if (right) poly << QPointF(x0, 6) << QPointF(x1, 12) << QPointF(x0, 18);
  else poly << QPointF(x1, 6) << QPointF(x0, 12) << QPointF(x1, 18);
  p.setPen(Qt::NoPen);
  p.setBrush(kInk);
  p.drawPolygon(poly);
}

void bar(QPainter& p, qreal x) {
  p.setPen(Qt::NoPen);
  p.setBrush(kInk);
  p.drawRect(QRectF(x, 6, 2.2, 12));
}

void keyShape(QPainter& p, const QColor& c) {
  p.setPen(pen(c, 1.8));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QRectF(3.5, 7.5, 8, 8));
  p.drawLine(QPointF(11.5, 11.5), QPointF(20.5, 11.5));
  p.drawLine(QPointF(17.5, 11.5), QPointF(17.5, 15));
  p.drawLine(QPointF(20.5, 11.5), QPointF(20.5, 15.5));
}

}  // namespace

QIcon goStart() {
  return make([](QPainter& p) {
    bar(p, 5);
    tri(p, 8, 18, false);
  });
}
QIcon goEnd() {
  return make([](QPainter& p) {
    tri(p, 6, 16, true);
    bar(p, 17);
  });
}
QIcon prevFrame() {
  return make([](QPainter& p) {
    tri(p, 5, 13, false);
    bar(p, 15);
  });
}
QIcon nextFrame() {
  return make([](QPainter& p) {
    bar(p, 7);
    tri(p, 11, 19, true);
  });
}
QIcon play() {
  return make([](QPainter& p) { tri(p, 7, 18, true); });
}
QIcon stop() {
  return make([](QPainter& p) {
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    p.drawRect(QRectF(7, 7, 4, 10));
    p.drawRect(QRectF(13, 7, 4, 10));
  });
}
QIcon key() {
  return make([](QPainter& p) { keyShape(p, kInk); });
}
QIcon keyFilters() {
  return make([](QPainter& p) {
    keyShape(p, kInk);
    p.setPen(pen(kBlue, 1.4));
    p.drawLine(QPointF(3, 20), QPointF(21, 20));
  });
}
QIcon keyMode() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(4, 12), QPointF(20, 12));
    p.setBrush(kRed);
    p.setPen(Qt::NoPen);
    p.drawRect(QRectF(6, 8, 3, 8));
    p.drawRect(QRectF(15, 8, 3, 8));
  });
}
QIcon timeConfig() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4, 4, 16, 16));
    p.drawLine(QPointF(12, 12), QPointF(12, 7));
    p.drawLine(QPointF(12, 12), QPointF(16, 14));
  });
}
QIcon motion() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.5));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(3, 18);
    path.cubicTo(8, 4, 14, 22, 20, 6);
    p.drawPath(path);
    p.setBrush(kBlue);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(17.5, 3.5, 5, 5));
  });
}
QIcon curveEditor() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0x80, 0x80, 0x80), 1.0));
    p.drawRect(QRectF(2.5, 3.5, 19, 17));
    p.setPen(pen(kRed, 1.6));
    QPainterPath c;
    c.moveTo(4, 17);
    c.cubicTo(9, 17, 10, 6, 20, 6);
    p.drawPath(c);
    p.setPen(pen(kGreen, 1.6));
    QPainterPath d;
    d.moveTo(4, 9);
    d.cubicTo(10, 9, 14, 18, 20, 15);
    p.drawPath(d);
  });
}
QIcon dopeSheet() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0x80, 0x80, 0x80), 1.0));
    for (int y : {7, 12, 17}) p.drawLine(QPointF(3, y), QPointF(21, y));
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    p.drawRect(QRectF(5, 5, 3, 4));
    p.drawRect(QRectF(14, 10, 3, 4));
    p.drawRect(QRectF(9, 15, 3, 4));
  });
}
QIcon schematic() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.2));
    p.setBrush(QColor(0x40, 0x60, 0x90));
    p.drawRect(QRectF(8, 3, 8, 5));
    p.drawRect(QRectF(2.5, 15, 8, 5));
    p.drawRect(QRectF(13.5, 15, 8, 5));
    p.drawLine(QPointF(12, 8), QPointF(6.5, 15));
    p.drawLine(QPointF(12, 8), QPointF(17.5, 15));
  });
}
QIcon frameAll() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(4, 8), QPointF(4, 4));
    p.drawLine(QPointF(4, 4), QPointF(8, 4));
    p.drawLine(QPointF(16, 4), QPointF(20, 4));
    p.drawLine(QPointF(20, 4), QPointF(20, 8));
    p.drawLine(QPointF(4, 16), QPointF(4, 20));
    p.drawLine(QPointF(4, 20), QPointF(8, 20));
    p.drawLine(QPointF(16, 20), QPointF(20, 20));
    p.drawLine(QPointF(20, 20), QPointF(20, 16));
    p.setPen(pen(kRed, 1.4));
    p.drawLine(QPointF(7, 15), QPointF(17, 9));
  });
}
QIcon addKey() {
  return make([](QPainter& p) {
    p.setPen(pen(kRed, 1.6));
    QPainterPath c;
    c.moveTo(3, 18);
    c.cubicTo(9, 18, 13, 7, 21, 7);
    p.drawPath(c);
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    p.drawRect(QRectF(10, 9.5, 4, 4));
    p.setPen(pen(kGreen, 1.6));
    p.drawLine(QPointF(17, 14), QPointF(17, 21));
    p.drawLine(QPointF(13.5, 17.5), QPointF(20.5, 17.5));
  });
}
QIcon moveKeys() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.drawLine(QPointF(12, 3), QPointF(12, 21));
    p.drawLine(QPointF(3, 12), QPointF(21, 12));
    p.drawLine(QPointF(12, 3), QPointF(9.5, 5.5));
    p.drawLine(QPointF(12, 3), QPointF(14.5, 5.5));
    p.drawLine(QPointF(12, 21), QPointF(9.5, 18.5));
    p.drawLine(QPointF(12, 21), QPointF(14.5, 18.5));
    p.drawLine(QPointF(3, 12), QPointF(5.5, 9.5));
    p.drawLine(QPointF(3, 12), QPointF(5.5, 14.5));
    p.drawLine(QPointF(21, 12), QPointF(18.5, 9.5));
    p.drawLine(QPointF(21, 12), QPointF(18.5, 14.5));
  });
}

QIcon tangent(int type) {
  return make([type](QPainter& p) {
    p.setPen(pen(kInk, 1.6));
    QPainterPath c;
    switch (type) {
      case 0:  // auto
        c.moveTo(3, 18);
        c.cubicTo(9, 18, 15, 6, 21, 6);
        break;
      case 1:  // smooth
        c.moveTo(3, 19);
        c.cubicTo(8, 12, 16, 12, 21, 5);
        break;
      case 2:  // linear
        c.moveTo(3, 19);
        c.lineTo(21, 5);
        break;
      case 3:  // step
        c.moveTo(3, 18);
        c.lineTo(12, 18);
        c.lineTo(12, 6);
        c.lineTo(21, 6);
        break;
      case 4:  // fast
        c.moveTo(3, 18);
        c.cubicTo(12, 18, 10, 6, 12, 6);
        c.lineTo(21, 6);
        break;
      case 5:  // slow
        c.moveTo(3, 18);
        c.cubicTo(12, 18, 12, 6, 21, 6);
        break;
      default:  // custom
        c.moveTo(3, 18);
        c.cubicTo(6, 4, 18, 20, 21, 6);
        break;
    }
    p.drawPath(c);
    p.setPen(Qt::NoPen);
    p.setBrush(kRed);
    p.drawRect(QRectF(10.5, 10.5, 3, 3));
  });
}

QIcon outOfRange(int type) {
  return make([type](QPainter& p) {
    p.setPen(pen(QColor(0x80, 0x80, 0x80), 1.0));
    p.drawLine(QPointF(8, 3), QPointF(8, 21));
    p.drawLine(QPointF(16, 3), QPointF(16, 21));
    p.setPen(pen(kInk, 1.6));
    QPainterPath c;
    c.moveTo(8, 17);
    c.lineTo(16, 7);
    p.drawPath(c);
    p.setPen(pen(kBlue, 1.4));
    QPainterPath e;
    switch (type) {
      case 0:  // constant
        e.moveTo(2, 17);
        e.lineTo(8, 17);
        e.moveTo(16, 7);
        e.lineTo(22, 7);
        break;
      case 1:  // cycle
      case 2:  // loop
        e.moveTo(16, 17);
        e.lineTo(22, 10);
        e.moveTo(2, 10);
        e.lineTo(8, 7);
        break;
      case 3:  // ping pong
        e.moveTo(16, 7);
        e.lineTo(22, 15);
        e.moveTo(2, 9);
        e.lineTo(8, 17);
        break;
      case 4:  // linear
        e.moveTo(2, 22);
        e.lineTo(8, 17);
        e.moveTo(16, 7);
        e.lineTo(22, 2);
        break;
      default:  // relative repeat
        e.moveTo(16, 7);
        e.lineTo(22, 2);
        e.moveTo(2, 22);
        e.lineTo(8, 17);
        break;
    }
    p.drawPath(e);
  });
}

}  // namespace veraa::animicons
