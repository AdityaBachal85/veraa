#include "icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <cmath>
#include <functional>

#include "theme.h"

namespace veraa::icons {

namespace {

using DrawFn = std::function<void(QPainter&)>;

// Draws on a 24x24 logical canvas at 1x and 2x.
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
const QColor kBlue(0x6f, 0xb0, 0xf0);

QPen pen(const QColor& c, qreal w = 1.6) {
  QPen p(c, w);
  p.setCapStyle(Qt::RoundCap);
  p.setJoinStyle(Qt::RoundJoin);
  return p;
}

void arrowHead(QPainter& p, QPointF tip, QPointF dir) {
  QPointF n(-dir.y(), dir.x());
  QPolygonF tri;
  tri << tip << tip - dir * 4.5 + n * 3.0 << tip - dir * 4.5 - n * 3.0;
  p.drawPolygon(tri);
}

void teapot(QPainter& p, const QColor& c) {
  p.setPen(pen(c, 1.4));
  p.setBrush(c.darker(160));
  QPainterPath body;
  body.addEllipse(QRectF(6, 9, 12, 9));
  p.drawPath(body);
  p.drawLine(QPointF(9, 9), QPointF(15, 9));
  p.drawEllipse(QRectF(11, 6.5, 2, 2));
  QPainterPath spout;
  spout.moveTo(17, 14);
  spout.cubicTo(20, 13, 20, 10, 22, 8.5);
  p.setBrush(Qt::NoBrush);
  p.drawPath(spout);
  QPainterPath handle;
  handle.moveTo(6.5, 11);
  handle.cubicTo(2.5, 10.5, 2.5, 16, 6.8, 15.5);
  p.drawPath(handle);
}

}  // namespace

QIcon app() {
  QIcon icon;
  for (int s : {16, 32, 48, 64, 128, 256}) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(s / 64.0, s / 64.0);
    QLinearGradient bg(0, 0, 0, 64);
    bg.setColorAt(0, QColor(0x2f, 0x6f, 0xb5));
    bg.setColorAt(1, QColor(0x14, 0x2f, 0x52));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12);
    QRadialGradient g(QPointF(26, 24), 22);
    g.setColorAt(0, QColor(255, 250, 235));
    g.setColorAt(0.5, QColor(0xf2, 0xb3, 0x3d));
    g.setColorAt(1, QColor(0x8a, 0x4a, 0x10));
    p.setBrush(g);
    p.drawEllipse(QRectF(14, 12, 36, 36));
    QFont f("Sans", 15, QFont::Black);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(QRectF(2, 40, 60, 22), Qt::AlignCenter, "V");
    p.end();
    icon.addPixmap(pm);
  }
  return icon;
}

QIcon undo() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(7, 9);
    path.cubicTo(13, 4, 22, 8, 19, 15);
    path.cubicTo(17, 19, 12, 19, 10, 18);
    p.drawPath(path);
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(4.5, 11), QPointF(-0.6, 0.8));
  });
}

QIcon redo() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2));
    QPainterPath path;
    path.moveTo(17, 9);
    path.cubicTo(11, 4, 2, 8, 5, 15);
    path.cubicTo(7, 19, 12, 19, 14, 18);
    p.drawPath(path);
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(19.5, 11), QPointF(0.6, 0.8));
  });
}

QIcon select() {
  return make([](QPainter& p) {
    QPolygonF cursor;
    cursor << QPointF(6, 3) << QPointF(6, 19) << QPointF(10, 15) << QPointF(13, 21) << QPointF(15.5, 20)
           << QPointF(12.5, 14) << QPointF(18, 14);
    p.setPen(pen(QColor(30, 30, 30), 1.2));
    p.setBrush(kInk);
    p.drawPolygon(cursor);
  });
}

QIcon move() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.8));
    p.drawLine(QPointF(12, 4), QPointF(12, 20));
    p.drawLine(QPointF(4, 12), QPointF(20, 12));
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(12, 2.5), QPointF(0, -1));
    arrowHead(p, QPointF(12, 21.5), QPointF(0, 1));
    arrowHead(p, QPointF(2.5, 12), QPointF(-1, 0));
    arrowHead(p, QPointF(21.5, 12), QPointF(1, 0));
  });
}

QIcon rotate() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.8));
    p.drawArc(QRectF(4, 4, 16, 16), 30 * 16, 290 * 16);
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(19.5, 7.5), QPointF(0.3, -1));
    p.setPen(pen(kBlue, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(9, 9, 6, 6));
  });
}

QIcon scale() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(3.5, 9.5, 11, 11));
    p.setPen(pen(kBlue, 1.4));
    p.drawRect(QRectF(3.5, 14.5, 6, 6));
    p.setPen(pen(kInk, 1.8));
    p.drawLine(QPointF(11, 13), QPointF(19, 5));
    p.setBrush(kInk);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(20.5, 3.5), QPointF(0.7, -0.7));
  });
}

QIcon axis(const QString& label) {
  return make([label](QPainter& p) {
    QColor c = label == "X" ? theme::axisX() : label == "Y" ? theme::axisY() : label == "Z" ? theme::axisZ() : kInk;
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(label.size() > 1 ? 10 : 13);
    p.setFont(f);
    p.setPen(c.lighter(115));
    p.drawText(QRectF(0, 0, 24, 24), Qt::AlignCenter, label);
  });
}

QIcon materialEditor() {
  return make([](QPainter& p) {
    QRadialGradient g(QPointF(9, 8), 13);
    g.setColorAt(0, QColor(250, 250, 250));
    g.setColorAt(0.35, QColor(0xd9, 0x6b, 0x3a));
    g.setColorAt(1, QColor(0x3a, 0x15, 0x05));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(QRectF(3, 3, 18, 18));
  });
}

QIcon renderSetup() {
  return make([](QPainter& p) {
    teapot(p, kInk);
    p.setPen(pen(kBlue, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(15, 15, 6, 6));
    for (int i = 0; i < 6; ++i) {
      double a = i * M_PI / 3;
      p.drawLine(QPointF(18 + 3.2 * std::cos(a), 18 + 3.2 * std::sin(a)),
                 QPointF(18 + 4.8 * std::cos(a), 18 + 4.8 * std::sin(a)));
    }
  });
}

QIcon render() {
  return make([](QPainter& p) { teapot(p, QColor(0xf2, 0xb3, 0x3d)); });
}

QIcon newFile() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(QColor(0x5a, 0x5a, 0x5a));
    QPolygonF page;
    page << QPointF(6, 3) << QPointF(14, 3) << QPointF(19, 8) << QPointF(19, 21) << QPointF(6, 21);
    p.drawPolygon(page);
    p.drawLine(QPointF(14, 3), QPointF(14, 8));
    p.drawLine(QPointF(14, 8), QPointF(19, 8));
  });
}

QIcon open() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0xc8, 0x9a, 0x45));
    QPolygonF folder;
    folder << QPointF(3, 6) << QPointF(9, 6) << QPointF(11, 8) << QPointF(20, 8) << QPointF(20, 19)
           << QPointF(3, 19);
    p.drawPolygon(folder);
  });
}

QIcon save() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x3f, 0x6e, 0xa8));
    p.drawRoundedRect(QRectF(4, 4, 16, 16), 2, 2);
    p.setBrush(QColor(0xe8, 0xe8, 0xe8));
    p.drawRect(QRectF(7.5, 4, 9, 6));
    p.setBrush(QColor(0x2a, 0x2a, 0x2a));
    p.drawRect(QRectF(7, 13, 10, 7));
  });
}

QIcon import() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x5a, 0x5a, 0x5a));
    p.drawRect(QRectF(4, 11, 16, 9));
    p.setPen(pen(kBlue, 2));
    p.drawLine(QPointF(12, 2), QPointF(12, 13));
    p.setBrush(kBlue);
    p.setPen(Qt::NoPen);
    arrowHead(p, QPointF(12, 15), QPointF(0, 1));
  });
}

QIcon geometry() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x6f, 0x8f, 0xb8));
    QPolygonF top, left, right;
    top << QPointF(12, 3) << QPointF(20, 7) << QPointF(12, 11) << QPointF(4, 7);
    left << QPointF(4, 7) << QPointF(12, 11) << QPointF(12, 21) << QPointF(4, 17);
    right << QPointF(12, 11) << QPointF(20, 7) << QPointF(20, 17) << QPointF(12, 21);
    p.drawPolygon(top);
    p.setBrush(QColor(0x4a, 0x63, 0x85));
    p.drawPolygon(left);
    p.setBrush(QColor(0x38, 0x4c, 0x66));
    p.drawPolygon(right);
  });
}

QIcon lights() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0x30, 0x30, 0x30), 1.0));
    p.setBrush(QColor(0xf5, 0xd0, 0x40));
    p.drawEllipse(QRectF(6, 3, 12, 12));
    p.setBrush(QColor(0x99, 0x99, 0x99));
    p.drawRect(QRectF(9, 15, 6, 5));
    p.setPen(pen(QColor(0xf5, 0xd0, 0x40), 1.3));
    p.drawLine(QPointF(2, 9), QPointF(4, 9));
    p.drawLine(QPointF(20, 9), QPointF(22, 9));
    p.drawLine(QPointF(12, 0.5), QPointF(12, 2));
  });
}

QIcon shapes() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0x7f, 0xd0, 0xf0), 1.8));
    p.setBrush(Qt::NoBrush);
    QPainterPath path(QPointF(3, 18));
    path.cubicTo(QPointF(6, 4), QPointF(12, 22), QPointF(21, 5));
    p.drawPath(path);
    p.setPen(pen(kInk, 1.2));
    p.setBrush(kInk);
    for (QPointF k : {QPointF(3, 18), QPointF(21, 5)}) p.drawRect(QRectF(k.x() - 1.5, k.y() - 1.5, 3, 3));
  });
}

QIcon helpers() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0x60, 0xd0, 0x70), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(5, 5, 14, 14));
    p.drawLine(QPointF(12, 2), QPointF(12, 22));
    p.drawLine(QPointF(2, 12), QPointF(22, 12));
  });
}

QIcon spaceWarps() {
  return make([](QPainter& p) {
    p.setPen(pen(QColor(0xa0, 0xa8, 0xf0), 1.6));
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < 3; ++i) {
      QPainterPath w(QPointF(3, 6 + i * 6));
      w.cubicTo(QPointF(8, 2 + i * 6), QPointF(14, 10 + i * 6), QPointF(21, 6 + i * 6));
      p.drawPath(w);
    }
  });
}

QIcon systems() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(QColor(0x90, 0x90, 0x90));
    p.drawEllipse(QRectF(7, 7, 10, 10));
    for (int i = 0; i < 8; ++i) {
      double a = i * 3.14159265 / 4;
      p.drawLine(QPointF(12 + 5 * std::cos(a), 12 + 5 * std::sin(a)), QPointF(12 + 9 * std::cos(a), 12 + 9 * std::sin(a)));
    }
    p.setBrush(QColor(0x30, 0x30, 0x30));
    p.drawEllipse(QRectF(10, 10, 4, 4));
  });
}

QIcon cameras() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.3));
    p.setBrush(QColor(0x55, 0x55, 0x55));
    p.drawRoundedRect(QRectF(2, 8, 14, 10), 2, 2);
    QPolygonF lens;
    lens << QPointF(16, 11) << QPointF(22, 8) << QPointF(22, 18) << QPointF(16, 15);
    p.drawPolygon(lens);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(3, 2, 6, 6));
    p.drawEllipse(QRectF(9, 2, 6, 6));
  });
}

QIcon create() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 2.2));
    p.drawLine(QPointF(12, 5), QPointF(12, 19));
    p.drawLine(QPointF(5, 12), QPointF(19, 12));
  });
}

QIcon modify() {
  return make([](QPainter& p) {
    p.setPen(pen(kBlue, 1.6));
    p.setBrush(QColor(0x3f, 0x6e, 0xa8));
    QPainterPath path;
    path.moveTo(4, 20);
    path.cubicTo(8, 4, 16, 4, 20, 20);
    path.closeSubpath();
    p.drawPath(path);
  });
}

QIcon trash() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(5, 7), QPointF(19, 7));
    p.drawLine(QPointF(10, 4), QPointF(14, 4));
    p.drawRect(QRectF(7, 7, 10, 13));
    p.drawLine(QPointF(10.5, 10), QPointF(10.5, 17));
    p.drawLine(QPointF(13.5, 10), QPointF(13.5, 17));
  });
}

QIcon arrowUp() {
  return make([](QPainter& p) {
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    QPolygonF t;
    t << QPointF(12, 6) << QPointF(19, 16) << QPointF(5, 16);
    p.drawPolygon(t);
  });
}

QIcon arrowDown() {
  return make([](QPainter& p) {
    p.setPen(Qt::NoPen);
    p.setBrush(kInk);
    QPolygonF t;
    t << QPointF(12, 18) << QPointF(19, 8) << QPointF(5, 8);
    p.drawPolygon(t);
  });
}

QIcon light() { return lights(); }
QIcon camera() { return cameras(); }

QIcon object() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.2));
    p.setBrush(QColor(0x6f, 0x8f, 0xb8));
    p.drawRect(QRectF(5, 7, 12, 12));
    p.drawLine(QPointF(5, 7), QPointF(9, 3));
    p.drawLine(QPointF(17, 7), QPointF(21, 3));
    p.drawLine(QPointF(9, 3), QPointF(21, 3));
    p.drawLine(QPointF(21, 3), QPointF(21, 15));
    p.drawLine(QPointF(21, 15), QPointF(17, 19));
  });
}

QIcon eye(bool openEye) {
  return make([openEye](QPainter& p) {
    p.setPen(pen(openEye ? kInk : QColor(0x77, 0x77, 0x77), 1.4));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(3, 12);
    path.quadTo(12, 3, 21, 12);
    path.quadTo(12, 21, 3, 12);
    p.drawPath(path);
    if (openEye) {
      p.setBrush(kInk);
      p.drawEllipse(QRectF(9.5, 9.5, 5, 5));
    } else {
      p.drawLine(QPointF(4, 20), QPointF(20, 4));
    }
  });
}

QIcon stop() {
  return make([](QPainter& p) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xe0, 0x50, 0x50));
    p.drawRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
  });
}

QIcon zoomExtents() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(4, 4, 16, 16));
    p.setPen(pen(kBlue, 1.5));
    p.drawRect(QRectF(8, 8, 8, 8));
  });
}

QIcon maximize() {
  return make([](QPainter& p) {
    p.setPen(pen(kInk, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(4, 4, 7, 7));
    p.drawRect(QRectF(13, 4, 7, 7));
    p.drawRect(QRectF(4, 13, 7, 7));
    p.setBrush(kBlue);
    p.drawRect(QRectF(13, 13, 7, 7));
  });
}

}  // namespace veraa::icons
