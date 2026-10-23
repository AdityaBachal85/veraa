// Final layout of the menu bar, the main toolbar and the axis constraints toolbar.
//
// The main window and the feature modules add their actions to menus and to the main toolbar
// while they are installed, so on their own the order would follow the install order.
// arrangeInterface() runs once after every module and lays menus and toolbars out in the
// 3ds Max 2022 order recorded in docs/UI_PARITY.md, by command id. Actions a module added that
// the layout does not name stay at the end of their menu (after a separator), so new features
// remain reachable. Items of the reference layout that Veraa does not have yet are placeholders
// that open a "not available yet" message.
#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSet>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>

#include <functional>
#include <vector>

#include "assetlibrary.h"
#include "icons.h"
#include "icons_ui.h"
#include "mainwindow.h"
#include "renderdialogs.h"
#include "rendertools.h"
#include "uistate.h"

namespace veraa {

namespace {

QString plainText(QString t) {
  t.remove('&');
  int tab = t.indexOf('\t');
  if (tab >= 0) t.truncate(tab);
  return t.trimmed();
}

// Feature name for messages: "Render to Texture..." -> "Render to Texture".
QString featureName(const QString& text) {
  QString t = plainText(text);
  while (t.endsWith('.')) t.chop(1);
  return t.trimmed();
}

// One entry of a menu layout.
struct Item {
  enum Kind { Separator, Command, Submenu, Keep, Todo, Text, Drop };
  Kind kind = Separator;
  QString key;   // command id, submenu title, placeholder text or action text
  QString text;  // label override for a command (shown through an alias action)
  std::vector<Item> items;
};

Item sep() { return Item(); }
Item id(const char* key, const char* text = nullptr) {
  Item i;
  i.kind = Item::Command;
  i.key = QString::fromLatin1(key);
  if (text) i.text = QObject::tr(text);
  return i;
}
// A submenu: an existing one with this title (arranged by `items` when given) or a new one.
Item sub(const char* title, std::vector<Item> items = {}) {
  Item i;
  i.kind = Item::Submenu;
  i.key = QObject::tr(title);
  i.items = std::move(items);
  return i;
}
// An existing submenu kept as its module built it (left out when the module is missing).
Item keep(const char* title) {
  Item i;
  i.kind = Item::Keep;
  i.key = QObject::tr(title);
  return i;
}
// A feature Veraa does not have yet.
Item todo(const char* text) {
  Item i;
  i.kind = Item::Todo;
  i.key = QObject::tr(text);
  return i;
}
// A submenu for a feature Veraa does not have yet.
Item subTodo(const char* title) {
  Item s = sub(title);
  Item t;
  t.kind = Item::Todo;
  t.key = QObject::tr(title);
  t.text = QObject::tr("Not Available Yet");
  s.items.push_back(t);
  return s;
}
// An existing action without a command id, found by its text.
Item text(const char* t) {
  Item i;
  i.kind = Item::Text;
  i.key = QObject::tr(t);
  return i;
}
// A command (or action text) left out of this menu (still reachable elsewhere).
Item drop(const char* key) {
  Item i;
  i.kind = Item::Drop;
  i.key = QString::fromLatin1(key);
  return i;
}

void tidySeparators(QMenu* menu) {
  QList<QAction*> acts = menu->actions();
  bool lastWasSep = true;  // drops leading separators
  for (QAction* a : acts) {
    if (a->isSeparator()) {
      if (lastWasSep) menu->removeAction(a);
      lastWasSep = true;
    } else {
      lastWasSep = false;
    }
  }
  acts = menu->actions();
  while (!acts.isEmpty() && acts.back()->isSeparator()) {
    menu->removeAction(acts.back());
    acts.pop_back();
  }
}

class MenuArranger {
 public:
  explicit MenuArranger(MainWindow& w, std::function<QAction*(const QString&, const QString&)> alias)
      : w_(w), alias_(std::move(alias)) {}

  // Lays out a top-level menu. Items placed anywhere in its tree, and dropped ones, are not
  // appended again as leftovers.
  void arrangeTop(QMenu* menu, const std::vector<Item>& spec) {
    used.clear();
    placed.clear();
    dropped.clear();
    collectDrops(spec);
    arrange(menu, spec);
  }

 private:
  void collectDrops(const std::vector<Item>& spec) {
    for (const Item& it : spec) {
      if (it.kind == Item::Drop) dropped.insert(it.key);
      collectDrops(it.items);
    }
  }

  void arrange(QMenu* menu, const std::vector<Item>& spec) {
    QList<QAction*> old = menu->actions();
    for (QAction* a : old) menu->removeAction(a);
    // The submenu with this title; a second one of the same title (two modules each made one)
    // is merged into the first.
    auto takeSubmenu = [&](const QString& title) -> QMenu* {
      QMenu* first = nullptr;
      for (QAction* a : old) {
        if (!a->menu() || plainText(a->text()) != plainText(title)) continue;
        used.insert(a);
        if (!first) {
          first = a->menu();
          continue;
        }
        for (QAction* x : a->menu()->actions()) {
          a->menu()->removeAction(x);
          if (!first->actions().contains(x)) first->addAction(x);
        }
      }
      return first;
    };
    for (const Item& it : spec) {
      switch (it.kind) {
        case Item::Separator: menu->addSeparator(); break;
        case Item::Command: {
          QAction* real = w_.command(it.key);
          QAction* a = real;
          if (real && !it.text.isEmpty() && plainText(real->text()) != plainText(it.text)) a = alias_(it.key, it.text);
          if (!a) a = w_.placeholderAction(it.text.isEmpty() ? it.key : it.text);
          menu->addAction(a);
          used.insert(a);
          if (real) used.insert(real);
          placed.insert(it.key);
          break;
        }
        case Item::Submenu:
        case Item::Keep: {
          QMenu* sub = takeSubmenu(it.key);
          if (!sub && it.kind == Item::Keep) break;
          if (!sub) sub = new QMenu(it.key, menu);
          if (!it.items.empty()) arrange(sub, it.items);
          menu->addMenu(sub);
          break;
        }
        case Item::Todo: {
          QAction* a = w_.placeholderAction(it.key);
          if (!it.text.isEmpty()) a->setText(it.text);
          menu->addAction(a);
          break;
        }
        case Item::Text:
          for (QAction* a : old)
            if (!used.contains(a) && !a->isSeparator() && plainText(a->text()) == plainText(it.key)) {
              menu->addAction(a);
              used.insert(a);
              break;
            }
          break;
        case Item::Drop: break;
      }
    }
    // Keep what modules added that the layout does not name.
    bool first = true;
    for (QAction* a : old) {
      if (used.contains(a)) continue;
      if (a->isSeparator()) {
        if (a->parent() == menu) a->deleteLater();
        continue;
      }
      QString cid = a->property("commandId").toString();
      QString pid = a->property("proxyId").toString();
      if (!cid.isEmpty() && (placed.contains(cid) || dropped.contains(cid))) continue;
      if (!pid.isEmpty() && (placed.contains(pid) || dropped.contains(pid))) continue;
      if (!pid.isEmpty() && used.contains(w_.command(pid))) continue;
      if (dropped.contains(plainText(a->text()))) continue;
      if (a->menu() && a->menu()->actions().isEmpty()) continue;
      if (first) {
        menu->addSeparator();
        first = false;
      }
      menu->addAction(a);
    }
    tidySeparators(menu);
  }

  MainWindow& w_;
  std::function<QAction*(const QString&, const QString&)> alias_;
  QSet<QAction*> used;
  QSet<QString> placed, dropped;
};

// A tool button whose click repeats the last action picked from its menu (3ds Max flyout).
QToolButton* flyoutButton(QToolBar* tb, const QString& name, const QList<QAction*>& actions) {
  auto* b = new QToolButton(tb);
  b->setObjectName(name);
  auto* menu = new QMenu(b);
  for (QAction* a : actions) menu->addAction(a);
  b->setMenu(menu);
  b->setPopupMode(QToolButton::MenuButtonPopup);
  b->setDefaultAction(actions.front());
  b->setAutoRaise(true);
  b->setIconSize(tb->iconSize());
  QObject::connect(tb, &QToolBar::iconSizeChanged, b, &QToolButton::setIconSize);
  QObject::connect(menu, &QMenu::triggered, b, [b](QAction* a) { b->setDefaultAction(a); });
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Placeholders and aliases

void MainWindow::showNotAvailable(const QString& feature) {
  lastNotAvailable_ = feature;
  QString msg = tr("%1 is not available in Veraa yet.").arg(feature);
  setPrompt(msg);
  auto* box = new QMessageBox(QMessageBox::Information, tr("Not Available Yet"), msg, QMessageBox::Ok, this);
  box->setObjectName("NotAvailableMessage");
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->open();
}

QAction* MainWindow::placeholderAction(const QString& text) {
  auto* a = new QAction(text, this);
  QString feature = featureName(text);
  a->setProperty("notAvailable", true);
  a->setProperty("feature", feature);
  a->setToolTip(tr("%1 (not available yet)").arg(feature));
  connect(a, &QAction::triggered, this, [this, a] { showNotAvailable(a->property("feature").toString()); });
  return a;
}

QAction* MainWindow::aliasAction(const QString& id, const QString& text) {
  QString key = id + '\n' + text;
  if (QAction* a = aliases_.value(key)) return a;
  QAction* real = command(id);
  if (!real) return nullptr;
  auto* a = new QAction(real->icon(), text, this);
  a->setProperty("proxyId", id);
  a->setProperty("aliasOf", id);
  connect(a, &QAction::triggered, this, [this, id](bool checked) {
    QAction* r = command(id);
    if (!r || (r->isCheckable() && r->isChecked() == checked)) return;
    r->trigger();
  });
  bindProxy(a, real, text);
  aliases_.insert(key, a);
  return a;
}

void MainWindow::addToolBarAction(QToolBar* tb, QAction* a) {
  auto* menu = qobject_cast<QMenu*>(a->property("toolMenu").value<QObject*>());
  QString context = a->property("contextCommand").toString();
  if (!menu && context.isEmpty()) {
    tb->addAction(a);
    return;
  }
  auto* b = new QToolButton(tb);
  b->setObjectName(a->property("commandId").toString() + ".button");
  b->setDefaultAction(a);
  b->setAutoRaise(true);
  b->setIconSize(tb->iconSize());
  connect(tb, &QToolBar::iconSizeChanged, b, &QToolButton::setIconSize);
  if (menu) {
    b->setMenu(menu);
    b->setPopupMode(QToolButton::MenuButtonPopup);
  }
  if (!context.isEmpty()) {
    b->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(b, &QWidget::customContextMenuRequested, this, [this, context] {
      if (QAction* c = command(context)) c->trigger();
    });
  }
  tb->addWidget(b);
}

// ---------------------------------------------------------------------------------------------
// Commands the layout needs that no module provides.

void MainWindow::createLayoutCommands() {
  auto ensure = [this](const QString& id, const QString& text, const QKeySequence& key, const QIcon& icon,
                       const std::function<void()>& fn) -> QAction* {
    if (QAction* a = command(id)) return a;
    QAction* a = cmd(id, text, key, icon);
    connect(a, &QAction::triggered, this, fn);
    return a;
  };
  ensure("render.iterative", tr("Render Iterative"), QKeySequence(), icons::render(), [this] {
    render();
    setPrompt(tr("Iterative render: the image goes to the frame window only"));
  });
  // The rendering module's Environment and Effects window, opened at one of its tabs.
  auto environmentTab = [this](const QString& tab) {
    EnvironmentDialog* d = environmentDialog(*this);
    for (int i = 0; i < d->tabs()->count(); ++i)
      if (d->tabs()->tabText(i) == tab) d->tabs()->setCurrentIndex(i);
    d->show();
    d->raise();
    d->activateWindow();
  };
  if (command("rendering.environment")) {
    ensure("rendering.exposureControl", tr("Exposure Control..."), QKeySequence(), QIcon(),
           [environmentTab] { environmentTab(tr("Environment")); });
    ensure("rendering.effects", tr("Effects..."), QKeySequence(), QIcon(), [environmentTab] { environmentTab(tr("Effects")); });
  }
  QAction* axisSnap = ensure("snaps.useAxisConstraint", tr("Snaps Use Axis Constraint Toggle"), QKeySequence("Alt+D"),
                             icons::snapAxisConstraint(), [this] {
                               bool on = command("snaps.useAxisConstraint")->isChecked();
                               UiState::instance().setSnapUseAxisConstraint(on);
                               setPrompt(on ? tr("Snapped moves keep to the axis constraint")
                                            : tr("Snapped moves go straight to the snap point"));
                             });
  axisSnap->setCheckable(true);
  axisSnap->setToolTip(tr("Snaps Use Axis Constraint Toggle (Alt+D)"));
  ensure("content.assetLibrary", tr("Asset Library..."), QKeySequence(), QIcon(), [this] {
    auto* d = findChild<AssetLibraryDialog*>();
    if (!d) d = new AssetLibraryDialog(this);
    d->show();
    d->raise();
  });
  ensure("scripting.pythonHelp", tr("Python Help..."), QKeySequence(), QIcon(),
         [] { QDesktopServices::openUrl(QUrl("https://docs.python.org/3/")); });
}

// ---------------------------------------------------------------------------------------------
// Menus

void MainWindow::arrangeMenus() {
  MenuArranger m(*this, [this](const QString& id, const QString& text) { return aliasAction(id, text); });

  m.arrangeTop(menuAt("File"), {
      keep("New"), id("file.reset"), id("file.open"), keep("Open Recent"), id("file.viewImage"), sep(),
      id("file.save"), id("file.saveAs"), id("file.saveCopyAs"), id("file.saveSelected"), id("file.archive"), sep(),
      keep("Import"), keep("Export"), todo("Share View..."), subTodo("Send to"), keep("Reference"), keep("Project"),
      id("file.summaryInfo"), id("file.fileProperties"), text("Preferences..."), sep(),
      id("file.exit")});

  m.arrangeTop(menuAt("Edit"), {
      id("edit.undo"), id("edit.redo"), sep(),
      id("edit.hold"), id("edit.fetch"), sep(),
      id("edit.delete"), id("edit.clone"), sep(),
      id("edit.move"), id("edit.rotate"), id("edit.scale"), id("edit.placement"), id("edit.transformTypeIn"),
      id("edit.transformToolbox"), sep(),
      id("edit.selectAll"), id("edit.selectNone"), id("edit.selectInvert"), sep(),
      id("edit.selectSimilar"), id("edit.selectInstances"),
      sub("Select By", {id("edit.selectByColor"), id("edit.selectByName", "Name..."), id("edit.selectByLayer"),
                        id("edit.selectByMaterial")}),
      sep(),
      sub("Selection Region", {id("edit.regionRectangle"), id("edit.regionCircle"), id("edit.regionFence"),
                               id("edit.regionLasso"), id("edit.regionPaint"), sep(), id("edit.windowCrossing")}),
      sep(),
      id("edit.selectionSets"), sep(),
      id("edit.objectProperties"),
      // On the main toolbar (and the Hierarchy panel).
      drop("tools.selectAndLink"), drop("tools.unlink")});

  m.arrangeTop(menuAt("Tools"), {
      id("tools.sceneExplorer"), id("tools.layerExplorer"), todo("Crease Explorer..."),
      sub("All Global Explorers", {id("tools.sceneExplorer", "Scene Explorer"), id("tools.layerExplorer", "Layer Explorer")}),
      todo("Manage Local Explorers..."), subTodo("Local Scene Explorers"), sep(),
      subTodo("Containers"), sep(),
      id("tools.isolate"), id("tools.endIsolate"), sep(),
      id("tools.displayFloater"), sep(),
      id("tools.sceneStates"), todo("Manage Templates..."), sep(),
      id("tools.lightLister"), sep(),
      id("edit.mirror"), id("tools.array"),
      sub("Align", {id("tools.align"), id("tools.quickAlign"), id("tools.spacing"), todo("Clone and Align..."),
                    id("tools.alignToView"), id("tools.normalAlign")}),
      id("tools.snapshot"), sep(),
      id("tools.renameObjects"), sep(),
      todo("Assign Vertex Colors..."), id("tools.colorClipboard"), todo("Perspective Match..."), sep(),
      todo("Viewport Canvas..."), sep(),
      sub("Preview - Grab Viewport", {id("tools.makePreview"), id("tools.grabViewport"), drop("tools.previewGrab")}),
      sub("Grids and Snaps", {id("snaps.toggle"), id("snaps.angle"), id("snaps.percent"), id("snaps.spinner"),
                              id("snaps.useAxisConstraint"), sep(), id("snaps.settings"), sep(), id("views.grid")}),
      id("tools.measureDistance", "Measure Distance..."), sep(),
      todo("Channel Info..."), sub("Mesh Inspector", {id("tools.meshInspector")}),
      // Reached from the toolbar, the quad menu and the Modify panel instead.
      drop("tools.convertToPoly"), drop("convertToEditableSpline"), drop("tools.collapseStack"),
      drop("tools.bindSpaceWarp")});

  m.arrangeTop(menuAt("Group"), {
      id("group.group"), id("group.ungroup"), sep(),
      id("group.open"), id("group.openRecursively"), id("group.close"), sep(),
      id("group.attach"), id("group.detach"), sep(),
      id("group.explode"), sep(),
      sub("Assembly", {todo("Assemble..."), todo("Disassemble"), sep(), todo("Open"), todo("Close"), sep(),
                       todo("Attach"), todo("Detach"), sep(), todo("Explode")})});

  m.arrangeTop(menuAt("Animation"), {
      id("anim.loadAnimation"), id("anim.saveAnimation"), sep(),
      keep("IK Solvers"), sep(),
      keep("Constraints"), sep(),
      keep("Transform Controllers"), keep("Position Controllers"), keep("Rotation Controllers"),
      keep("Scale Controllers"), keep("Simulation - Rigid Bodies"), subTodo("Fluids"), sep(),
      subTodo("Offset Controllers"), id("anim.parameterEditor"), id("anim.parameterCollector"), sep(),
      sub("Wire Parameters", {id("anim.wireParameters"), id("anim.wireParameters", "Parameter Wiring Dialog...")}), sep(),
      id("anim.layers"), sep(),
      id("anim.reactionManager"), sep(),
      id("anim.boneTools"), sep(),
      id("anim.setSkinPose"), id("anim.assumeSkinPose"), todo("Skin Pose Mode"), sep(),
      todo("Toggle Limits"), sep(),
      id("anim.deleteSelectedAnimation"), sep(),
      subTodo("Populate"), id("anim.walkthrough"), subTodo("Presets")});

  m.arrangeTop(menuAt("Graph Editors"), {
      id("graph.curveEditor"), id("graph.dopeSheet"), sep(), id("graph.schematic")});

  m.arrangeTop(menuAt("Rendering"), {
      id("render.render"), todo("Render in the Cloud"), id("render.setup"), id("render.frameWindow"),
      id("render.animation"), sep(),
      todo("State Sets..."), todo("Camera Sequencer..."), sep(),
      todo("Scene Converter..."), sep(),
      id("rendering.exposureControl"), sep(),
      id("rendering.environment", "Environment..."), id("rendering.effects"), id("rendering.sunAndSky"), sep(),
      todo("Render to Texture..."), todo("Bake to Texture..."), todo("Render Surface Map..."), sep(),
      sub("Material Editor", {id("render.materialEditor", "Compact Material Editor..."), id("rendering.slateEditor")}),
      todo("Material/Map Browser..."), todo("Material Explorer..."), sep(),
      todo("Video Post..."), sep(),
      todo("Panorama Exporter..."), id("rendering.batchRender"), id("rendering.printSize"), id("rendering.exportProxy"),
      sep(),
      todo("Gamma/LUT Setup..."), todo("Render Message Window..."), todo("Compare Media in RAM Player...")});

  m.arrangeTop(menuAt("Scripting"), {
      todo("Install Creation Graph Package..."), sep(),
      todo("New Creation Graph"), todo("Open Creation Graph..."), todo("Creation Graph Editor"), sep(),
      todo("Create Graph Geometry..."), todo("Create and Apply Graph Modifier..."), todo("Create Compound From Mesh..."),
      sep(),
      id("scripting.new"), id("scripting.open"), id("scripting.run"), sep(),
      id("scripting.listener"), id("scripting.editor"), sep(),
      id("scripting.macroRecorder"), id("scripting.recordToEditor"), sep(),
      todo("Visual Script Editor..."), sep(),
      todo("Debugger Dialog..."), sep(),
      id("scripting.help", "Script Help..."), sep(),
      id("scripting.pythonHelp"), id("scripting.openFolder")});

  // Content takes the place of the reference's Content menu (between Scripting and Help).
  m.arrangeTop(menuAt("Content"), {id("content.assetLibrary")});
}

// ---------------------------------------------------------------------------------------------
// Toolbars

void MainWindow::arrangeMainToolBar() {
  QToolBar* tb = mainToolBar_;
  QList<QAction*> old = tb->actions();
  QHash<QString, QAction*> widgets;
  for (QAction* a : old)
    if (auto* wa = qobject_cast<QWidgetAction*>(a))
      if (QWidget* wd = wa->defaultWidget(); wd && !wd->objectName().isEmpty()) widgets.insert(wd->objectName(), a);
  tb->clear();

  // New flyouts.
  auto widgetAction = [tb](QWidget* w) {
    auto* wa = new QWidgetAction(tb);
    wa->setDefaultWidget(w);
    return wa;
  };
  QAction* placeRotate = placeholderAction(tr("Select and Rotate (Place)"));
  placeRotate->setIcon(icons::place());
  widgets.insert("PlaceFlyout", widgetAction(flyoutButton(tb, "PlaceFlyout", {command("edit.placement"), placeRotate})));
  QList<QAction*> materialEditors{command("render.materialEditor")};
  if (QAction* slate = command("rendering.slateEditor")) {
    if (slate->icon().isNull()) slate->setIcon(icons::slateEditor());
    materialEditors << slate;
  }
  widgets.insert("MaterialEditorFlyout", widgetAction(flyoutButton(tb, "MaterialEditorFlyout", materialEditors)));
  QAction* activeShade = placeholderAction(tr("ActiveShade"));
  activeShade->setIcon(icons::activeShade());
  if (QAction* it = command("render.iterative")) it->setIcon(icons::renderIterative());
  widgets.insert("RenderFlyout",
                 widgetAction(flyoutButton(tb, "RenderFlyout", {command("render.render"), command("render.iterative"), activeShade})));
  QAction* ribbon = placeholderAction(tr("Toggle Ribbon"));
  ribbon->setIcon(icons::ribbon());

  const char* const layout[] = {
      "edit.undo", "edit.redo", "|",
      "tools.selectAndLink", "tools.unlink", "tools.bindSpaceWarp", "|",
      "w:SelectionFilter", "edit.select", "edit.selectByName", "w:RegionFlyout", "edit.windowCrossing", "|",
      "edit.move", "edit.rotate", "w:ScaleFlyout", "w:PlaceFlyout", "w:RefCoordSystem", "w:PivotFlyout", "|",
      "edit.manipulate", "customize.shortcutOverride", "|",
      "snaps.toggle", "snaps.angle", "snaps.percent", "snaps.spinner", "|",
      "edit.editNamedSets", "w:NamedSelectionSets", "|",
      "edit.mirror", "w:AlignFlyout", "|",
      "ui.toggleSceneExplorer", "ui.toggleLayerExplorer", "|",
      "ribbon", "graph.curveEditor", "graph.schematic", "|",
      "w:MaterialEditorFlyout", "|",
      "render.setup", "render.frameWindow", "w:RenderFlyout"};
  QSet<QAction*> placed;
  for (const char* entry : layout) {
    QString e = QString::fromLatin1(entry);
    if (e == "|") {
      tb->addSeparator();
    } else if (e.startsWith("w:")) {
      if (QAction* wa = widgets.value(e.mid(2))) {
        tb->addAction(wa);
        placed.insert(wa);
        // A flyout's actions are on the toolbar through it.
        if (auto* b = qobject_cast<QToolButton*>(static_cast<QWidgetAction*>(wa)->defaultWidget()); b && b->menu())
          for (QAction* x : b->menu()->actions()) {
            placed.insert(x);
            if (QAction* real = command(x->property("proxyId").toString())) placed.insert(real);
          }
      }
    } else if (e == "ribbon") {
      tb->addAction(ribbon);
    } else if (QAction* a = command(e)) {
      addToolBarAction(tb, a);
      placed.insert(a);
    } else {
      tb->addAction(placeholderAction(e));
    }
  }
  // Keep toolbar buttons modules added that the layout does not name.
  bool first = true;
  for (QAction* a : old) {
    if (placed.contains(a)) continue;
    if (a->isSeparator() || qobject_cast<QWidgetAction*>(a)) continue;
    QString cid = a->property("commandId").toString();
    QString pid = a->property("proxyId").toString();
    if ((!cid.isEmpty() && placed.contains(command(cid))) || (!pid.isEmpty() && placed.contains(command(pid))) ||
        !a->property("proxyId").toString().isEmpty())
      continue;
    if (first) {
      tb->addSeparator();
      first = false;
    }
    addToolBarAction(tb, a);
    placed.insert(a);
  }
  for (QAction* a : old)
    if (a->isSeparator() && a->parent() == tb) a->deleteLater();
}

void MainWindow::arrangeAxisToolBar() {
  QToolBar* tb = axisToolBar_;
  QAction* plane = nullptr;
  for (QAction* a : tb->actions())
    if (auto* wa = qobject_cast<QWidgetAction*>(a))
      if (wa->defaultWidget() && wa->defaultWidget()->objectName() == "PlaneFlyout") plane = wa;
  QList<QAction*> old = tb->actions();
  tb->clear();
  for (QAction* a : {axisXAct_, axisYAct_, axisZAct_}) tb->addAction(a);
  if (plane) tb->addAction(plane);
  if (QAction* snap = command("snaps.useAxisConstraint")) tb->addAction(snap);
  for (QAction* a : old)
    if (a->isSeparator() && a->parent() == tb) a->deleteLater();
}

void MainWindow::arrangeInterface() {
  createLayoutCommands();
  arrangeMenus();
  arrangeMainToolBar();
  arrangeAxisToolBar();
}

}  // namespace veraa
