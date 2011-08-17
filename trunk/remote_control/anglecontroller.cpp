#include "anglecontroller.h"

#include <math.h>

#include <QDebug>
#include <QGraphicsSceneMouseEvent>

namespace {
double getAngle(const QPointF& vector) {
  return - atan2(vector.x(), vector.y()) * 180 / M_PI;
}
}

void AngleController::mousePressEvent(QGraphicsSceneMouseEvent * event) {
}

void AngleController::mouseMoveEvent(QGraphicsSceneMouseEvent * event) {
  QGraphicsEllipseItem::mouseMoveEvent(event);
  QPointF origin(parentItem()->mapFromItem(this, 0, 0));
  QPointF pos(parentItem()->mapFromItem(this, event->pos()));
  QPointF diff(pos - origin);
  double angle = getAngle(diff) - getAngle(pos_);

  setRotation(angle);
  emit turned(angle);
}
