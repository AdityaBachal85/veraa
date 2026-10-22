// ViewCube: the orientation cube in the top-right corner of each viewport. Clicking a face,
// edge or corner turns the view to look from that direction; dragging the cube orbits; the
// compass ring below it spins the view around Z; the house button returns to the home view.
#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>

#include "vmath.h"

class QPainter;

namespace veraa {

class ViewCamera;

class ViewCube {
 public:
  enum class Hit { None, Region, Home, Ring, Body };

  // Places the cube in the top-right corner of a widget of this size.
  void layout(const QSize& widget);
  QRectF rect() const { return rect_; }
  bool contains(QPointF p) const { return rect_.contains(p); }

  void paint(QPainter& p, const ViewCamera& cam, QPointF hover, bool active) const;
  // What is under p. For Region, dir receives the view direction (from target toward eye).
  Hit hitTest(const ViewCamera& cam, QPointF p, Vec3* dir = nullptr) const;

  // Name for a direction ("Top", "Front Left", ...), used for tooltips and tests.
  static QString directionName(const Vec3& dir);

 private:
  struct Cell {
    Vec3 dir;          // view direction for this cell
    Vec3 corners[4];   // cube-space corners
    Vec3 normal;       // face normal
  };
  QPointF toScreen(const ViewCamera& cam, const Vec3& v) const;
  void cells(Cell out[54]) const;

  QRectF rect_;
  QPointF center_;
  float scale_ = 22.0f;
};

}  // namespace veraa
