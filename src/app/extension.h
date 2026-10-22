// Plug-in points for feature modules (modeling tools, animation, rendering, interface).
//
// A module adds its menus, toolbars, panels and viewport behaviour from its own files:
//
//   static void installMyFeature(MainWindow& w) {
//     QMenu* m = w.menuAt("Tools");
//     m->addAction(...);
//   }
//   VERAA_APP_EXTENSION(myFeature, 100, installMyFeature);
//
// Extensions run once, in ascending order, after the main window has built its standard UI.
#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

class QKeyEvent;
class QMouseEvent;
class QPainter;
class QWheelEvent;
class QWidget;

namespace veraa {

class EditorState;
class LineBatch;
class MainWindow;
class Node;
class SceneDocument;
class Viewport;

// An interactive viewport mode (sub-object editing, Line creation, measuring, ...). While set
// on EditorState, it sees mouse and key events before the default select/transform handling.
// Return true from an event handler to consume the event.
class ViewportTool {
 public:
  virtual ~ViewportTool() = default;
  virtual bool mousePress(Viewport& vp, QMouseEvent* e) { return false; }
  virtual bool mouseMove(Viewport& vp, QMouseEvent* e) { return false; }
  virtual bool mouseRelease(Viewport& vp, QMouseEvent* e) { return false; }
  virtual bool mouseDoubleClick(Viewport& vp, QMouseEvent* e) { return false; }
  virtual bool keyPress(Viewport& vp, QKeyEvent* e) { return false; }
  // 3D lines in world space, drawn after the scene (depth tested).
  virtual void drawLines(Viewport& vp, LineBatch& lines) {}
  // 2D overlay in widget pixels, drawn last.
  virtual void drawOverlay(Viewport& vp, QPainter& p) {}
  // Esc / tool change. Return true if something was cancelled.
  virtual bool cancel() { return false; }
  // Whether the move/rotate/scale gizmo should still be shown and usable.
  virtual bool allowsGizmo() const { return false; }
  virtual QString prompt() const { return QString(); }
};

// Always-on viewport drawing (snap markers, motion paths, sub-object display, ...).
class ViewportDecorator {
 public:
  virtual ~ViewportDecorator() = default;
  virtual void drawLines(Viewport& vp, LineBatch& lines) {}
  virtual void drawOverlay(Viewport& vp, QPainter& p) {}
};

using ExtensionInstaller = void (*)(MainWindow& window);

struct ExtensionRegistrar {
  ExtensionRegistrar(const char* name, int order, ExtensionInstaller installer);
};

// Runs every registered extension (called by MainWindow at the end of its constructor).
void runAppExtensions(MainWindow& window);

// Custom creation for an object type (e.g. Line places knots with clicks). When the user starts
// creating this type, the factory's tool becomes the active viewport tool.
using CreationToolFactory = std::function<std::shared_ptr<ViewportTool>()>;
void registerCreationTool(const QString& objectType, CreationToolFactory factory);
CreationToolFactory creationToolFor(const QString& objectType);

// Extra Modify panel rollouts for an object or modifier type (Editable Poly selection levels,
// Edit Spline tools, FFD control points, Unwrap UVW, ...). The factory builds a widget shown
// above the generated parameter rollouts while that stack entry is selected. stackIndex is -1
// for the base object, otherwise the modifier index (bottom = 0).
struct ModifyPanelContext {
  SceneDocument* doc = nullptr;
  EditorState* state = nullptr;
  uint32_t nodeId = 0;
  int stackIndex = -1;
};
using ModifyRolloutFactory = std::function<QWidget*(const ModifyPanelContext& ctx, QWidget* parent)>;
void registerModifyRollouts(const QString& type, ModifyRolloutFactory factory);
ModifyRolloutFactory modifyRolloutsFor(const QString& type);

}  // namespace veraa

#define VERAA_APP_EXTENSION(name, order, installer) \
  static ::veraa::ExtensionRegistrar veraaAppExtension_##name(#name, order, installer)
