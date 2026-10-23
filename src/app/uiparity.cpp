// Interface structure checks: menu titles and item order, main toolbar, command panel tabs and
// Create panel buttons ("Veraa --script uiParity", also part of "--script selftest").
// With VERAA_UI_DUMP=<file>, "Veraa --script uiDump" writes the whole menu / toolbar / command
// panel structure as text (used to compare the interface with docs/UI_PARITY.md).
#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QPainter>
#include <QMenu>
#include <QMenuBar>
#include <QAbstractItemView>
#include <QTabWidget>
#include <QTextStream>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>

#include "commandpanel.h"
#include "mainwindow.h"
#include "selftest.h"

namespace veraa {

namespace {

using testing::check;
using testing::pump;

QString plain(QString t) {
  t.remove('&');
  int tab = t.indexOf('\t');
  return tab >= 0 ? t.left(tab) : t;
}

void dumpMenu(QTextStream& out, QMenu* m, int depth) {
  Q_EMIT m->aboutToShow();
  for (QAction* a : m->actions()) {
    QString indent(depth * 2, ' ');
    if (a->isSeparator()) {
      out << indent << "----\n";
      continue;
    }
    QString key = a->shortcut().toString(QKeySequence::PortableText);
    QString id = a->property("commandId").toString();
    if (id.isEmpty()) id = a->property("proxyId").toString();
    out << indent << plain(a->text()) << (id.isEmpty() ? QString() : "  {" + id + "}") << (key.isEmpty() ? QString() : "  [" + key + "]")
        << (a->isEnabled() ? QString() : QString("  (disabled)")) << (a->menu() ? " >" : "") << "\n";
    if (a->menu() && depth < 3) dumpMenu(out, a->menu(), depth + 1);
  }
}

void uiDump(MainWindow& w) {
  QString path = qEnvironmentVariable("VERAA_UI_DUMP");
  if (path.isEmpty()) {
    std::printf("      VERAA_UI_DUMP not set; skipping the interface dump\n");
    return;
  }
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    check(false, "open VERAA_UI_DUMP file");
    return;
  }
  QTextStream out(&f);
  out << "== Menu bar\n";
  for (QAction* a : w.menuBar()->actions())
    if (a->menu()) out << plain(a->text()) << "\n";
  for (QAction* a : w.menuBar()->actions()) {
    if (!a->menu()) continue;
    out << "\n== Menu " << plain(a->text()) << "\n";
    dumpMenu(out, a->menu(), 1);
  }
  out << "\n== Main toolbar\n";
  for (QAction* a : w.mainToolBar()->actions()) {
    if (a->isSeparator()) {
      out << "  |\n";
      continue;
    }
    if (auto* wa = qobject_cast<QWidgetAction*>(a)) {
      QWidget* wd = w.mainToolBar()->widgetForAction(wa);
      if (auto* tb = qobject_cast<QToolButton*>(wd)) {
        out << "  [flyout] " << (tb->defaultAction() ? plain(tb->defaultAction()->toolTip()) : QString()) << ":";
        if (tb->menu())
          for (QAction* x : tb->menu()->actions()) out << " " << plain(x->text()) << ";";
        out << "\n";
      } else if (auto* c = qobject_cast<QComboBox*>(wd)) {
        out << "  [combo] " << c->toolTip() << ":";
        for (int i = 0; i < c->count(); ++i) out << " " << c->itemText(i) << ";";
        out << "\n";
      } else {
        out << "  [widget] " << (wd ? wd->metaObject()->className() : "?") << "\n";
      }
      continue;
    }
    out << "  " << plain(a->toolTip().isEmpty() ? a->text() : a->toolTip()) << (a->isEnabled() ? "" : "  (disabled)")
        << "\n";
  }
  out << "\n== Commands\n";
  QStringList ids = w.commands().keys();
  ids.sort();
  for (const QString& id : ids) {
    QAction* a = w.command(id);
    out << "  " << id << " = " << plain(a->text()) << "  [" << a->shortcut().toString(QKeySequence::PortableText) << "]\n";
  }
  out << "\n== Command panel\n";
  QTabWidget* cp = w.commandPanel();
  for (int i = 0; i < cp->count(); ++i) out << "  tab " << cp->tabToolTip(i) << "\n";
  out.flush();
  check(true, "interface dump written");
}

// Screenshots matching the reference set (VERAA_UI_SCREENS=<folder>, "--script uiParityScreens"):
// the empty main window at 1920x1050, every top menu, every command panel tab and every Create
// category with its subcategory list.
void uiParityScreens(MainWindow& w) {
  QString dir = qEnvironmentVariable("VERAA_UI_SCREENS");
  if (dir.isEmpty()) {
    std::printf("      VERAA_UI_SCREENS not set; skipping the parity screenshots\n");
    return;
  }
  QDir().mkpath(dir);
  w.resize(1920, 1050);
  pump(400);
  check(testing::saveGrab(&w, QDir(dir).filePath("p01_main_window.png")), "main window screenshot");
  int n = 0;
  for (QAction* a : w.menuBar()->actions()) {
    if (!a->menu()) continue;
    QMenu* m = a->menu();
    Q_EMIT m->aboutToShow();
    m->popup(w.mapToGlobal(w.menuBar()->actionGeometry(a).bottomLeft()));
    pump(250);
    QString file = QString("p1%1_menu_%2.png").arg(n++, 2, 10, QChar('0')).arg(plain(a->text()).toLower().replace(' ', '_'));
    check(m->grab().save(QDir(dir).filePath(file)), qPrintable("menu screenshot " + file));
    m->hide();
  }
  QTabWidget* cp = w.commandPanel();
  QWidget* dock = cp->parentWidget();
  while (dock && !qobject_cast<QDockWidget*>(dock)) dock = dock->parentWidget();
  for (int i = 0; i < cp->count(); ++i) {
    cp->setCurrentIndex(i);
    pump(150);
    QString file = QString("p2%1_panel_%2.png").arg(i).arg(cp->tabToolTip(i).toLower());
    check((dock ? dock : cp)->grab().save(QDir(dir).filePath(file)), qPrintable("panel screenshot " + file));
  }
  cp->setCurrentIndex(0);
  pump(100);
  QWidget* create = cp->widget(0);
  int c = 0;
  for (QToolButton* b : create->findChildren<QToolButton*>()) {
    if (b->toolTip().isEmpty() || !b->isCheckable()) continue;
    b->click();
    pump(120);
    QComboBox* sub = nullptr;
    for (QComboBox* box : create->findChildren<QComboBox*>())
      if (box->isVisible()) sub = box;
    QString base = QString("p3%1_create_%2").arg(c++).arg(b->toolTip().toLower().replace(' ', '_'));
    check((dock ? dock : cp)->grab().save(QDir(dir).filePath(base + ".png")), qPrintable("create screenshot " + base));
    if (sub) {
      sub->showPopup();
      pump(200);
      QPixmap pm = (dock ? dock : cp)->grab();
      if (QWidget* view = sub->view()) {
        QPixmap list = view->window()->grab();
        QPainter p(&pm);
        p.drawPixmap(sub->mapTo(dock ? dock : cp, QPoint(0, sub->height())), list);
      }
      check(pm.save(QDir(dir).filePath(base + "_list.png")), qPrintable("create list screenshot " + base));
      sub->hidePopup();
    }
  }
  if (!create->findChildren<QToolButton*>().isEmpty()) create->findChildren<QToolButton*>().front()->click();
}

}  // namespace

VERAA_SELF_TEST(uiDump, uiDump);
VERAA_SELF_TEST(uiParityScreens, uiParityScreens);

}  // namespace veraa
