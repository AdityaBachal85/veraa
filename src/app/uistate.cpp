#include "uistate.h"

namespace veraa {

QString refCoordSysName(RefCoordSys s) {
  switch (s) {
    case RefCoordSys::View: return QObject::tr("View");
    case RefCoordSys::Screen: return QObject::tr("Screen");
    case RefCoordSys::World: return QObject::tr("World");
    case RefCoordSys::Parent: return QObject::tr("Parent");
    case RefCoordSys::Local: return QObject::tr("Local");
    case RefCoordSys::Gimbal: return QObject::tr("Gimbal");
    case RefCoordSys::Grid: return QObject::tr("Grid");
    case RefCoordSys::Working: return QObject::tr("Working");
    case RefCoordSys::LocalAligned: return QObject::tr("Local Aligned");
    case RefCoordSys::Pick: return QObject::tr("Pick");
  }
  return QString();
}

UiState& UiState::instance() {
  static UiState* s = new UiState();
  return *s;
}

#define VERAA_UISTATE_SETTER(Name, member, Type) \
  void UiState::Name(Type v) {                   \
    if (member == v) return;                     \
    member = v;                                  \
    Q_EMIT changed();                            \
  }

VERAA_UISTATE_SETTER(setRegionShape, regionShape_, RegionShape)
VERAA_UISTATE_SETTER(setWindowSelection, window_, bool)
VERAA_UISTATE_SETTER(setPaintBrushRadius, paintRadius_, float)
VERAA_UISTATE_SETTER(setRefCoordSys, refCoord_, RefCoordSys)
VERAA_UISTATE_SETTER(setPickCoordNode, pickNode_, uint32_t)
VERAA_UISTATE_SETTER(setPivotCenter, pivotCenter_, PivotCenter)
VERAA_UISTATE_SETTER(setScaleMode, scaleMode_, ScaleMode)
VERAA_UISTATE_SETTER(setSelectionLocked, selLock_, bool)
VERAA_UISTATE_SETTER(setShortcutOverride, shortcutOverride_, bool)
VERAA_UISTATE_SETTER(setAbsoluteTypeIn, absoluteTypeIn_, bool)
VERAA_UISTATE_SETTER(setAdaptiveDegradation, adaptive_, bool)
VERAA_UISTATE_SETTER(setShowViewCube, viewCube_, bool)
VERAA_UISTATE_SETTER(setShowTransformGizmo, showGizmo_, bool)
VERAA_UISTATE_SETTER(setShadeSelected, shadeSelected_, bool)
VERAA_UISTATE_SETTER(setShowSelectionBrackets, brackets_, bool)
VERAA_UISTATE_SETTER(setGizmoSize, gizmoSize_, float)
VERAA_UISTATE_SETTER(setShowFrozenInGray, frozenGray_, bool)
VERAA_UISTATE_SETTER(setHiddenCategories, hiddenCategories_, int)
VERAA_UISTATE_SETTER(setHideFrozen, hideFrozen_, bool)
VERAA_UISTATE_SETTER(setSnapUseAxisConstraint, snapAxisConstraint_, bool)

#undef VERAA_UISTATE_SETTER

void UiState::setDisplayColors(bool wireObject, bool shadedObject) {
  if (wireObject == wireObjectColor_ && shadedObject == shadedObjectColor_) return;
  wireObjectColor_ = wireObject;
  shadedObjectColor_ = shadedObject;
  Q_EMIT changed();
}

}  // namespace veraa
