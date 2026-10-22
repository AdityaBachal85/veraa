// Installs the animation module: controller, commands and shortcuts, and the bar below the
// viewports (time slider, track bar, controls).
#include <QAction>
#include <QVBoxLayout>

#include "animbar.h"
#include "animcontroller.h"
#include "animicons.h"
#include "extension.h"
#include "mainwindow.h"

namespace veraa {
namespace {

QAction* command(MainWindow& w, const QString& id, const QString& text, const QKeySequence& key, const QIcon& icon = QIcon(),
                 bool checkable = false) {
  auto* a = new QAction(icon, text, &w);
  if (!key.isEmpty()) {
    a->setShortcut(key);
    a->setShortcutContext(Qt::WindowShortcut);
    a->setToolTip(QStringLiteral("%1 (%2)").arg(text, key.toString(QKeySequence::NativeText)));
  }
  a->setCheckable(checkable);
  w.addAction(a);
  w.registerCommand(id, a);
  return a;
}

void installAnimation(MainWindow& w) {
  auto* c = new AnimController(w);

  QAction* autoKey = command(w, "anim.autoKey", QObject::tr("Auto Key"), QKeySequence("N"), QIcon(), true);
  QObject::connect(autoKey, &QAction::toggled, c, [c](bool on) { c->setAutoKey(on); });
  QObject::connect(c, &AnimController::autoKeyChanged, autoKey, [autoKey](bool on) { autoKey->setChecked(on); });

  QAction* setKeyMode = command(w, "anim.setKeyMode", QObject::tr("Set Key"), QKeySequence("'"), QIcon(), true);
  QObject::connect(setKeyMode, &QAction::toggled, c, [c](bool on) { c->setSetKeyMode(on); });
  QObject::connect(c, &AnimController::setKeyModeChanged, setKeyMode, [setKeyMode](bool on) { setKeyMode->setChecked(on); });

  QAction* setKeys = command(w, "anim.setKey", QObject::tr("Set Keys"), QKeySequence("K"), animicons::key());
  QObject::connect(setKeys, &QAction::triggered, c, [c] { c->setKeys(); });

  QAction* play = command(w, "anim.play", QObject::tr("Play Animation"), QKeySequence("/"), animicons::play());
  QObject::connect(play, &QAction::triggered, c, [c] { c->togglePlay(); });
  QObject::connect(c, &AnimController::playingChanged, play, [play](bool on) {
    play->setIcon(on ? animicons::stop() : animicons::play());
    play->setText(on ? QObject::tr("Stop Animation") : QObject::tr("Play Animation"));
  });

  QAction* prev = command(w, "anim.prevFrame", QObject::tr("Previous Frame"), QKeySequence(","), animicons::prevFrame());
  QObject::connect(prev, &QAction::triggered, c, [c] { c->stepFrame(-1); });
  QAction* next = command(w, "anim.nextFrame", QObject::tr("Next Frame"), QKeySequence("."), animicons::nextFrame());
  QObject::connect(next, &QAction::triggered, c, [c] { c->stepFrame(1); });
  QAction* start = command(w, "anim.goStart", QObject::tr("Go to Start"), QKeySequence(Qt::Key_Home), animicons::goStart());
  QObject::connect(start, &QAction::triggered, c, [c] { c->goStart(); });
  QAction* end = command(w, "anim.goEnd", QObject::tr("Go to End"), QKeySequence(Qt::Key_End), animicons::goEnd());
  QObject::connect(end, &QAction::triggered, c, [c] { c->goEnd(); });
  QAction* keyMode = command(w, "anim.keyMode", QObject::tr("Key Mode Toggle"), QKeySequence(), animicons::keyMode(), true);
  QObject::connect(keyMode, &QAction::toggled, c, [c](bool on) { c->setKeyStepMode(on); });
  QAction* nextKey = command(w, "anim.nextKey", QObject::tr("Next Key"), QKeySequence("Alt+."));
  QObject::connect(nextKey, &QAction::triggered, c, [c] { c->nextKey(); });
  QAction* prevKey = command(w, "anim.prevKey", QObject::tr("Previous Key"), QKeySequence("Alt+,"));
  QObject::connect(prevKey, &QAction::triggered, c, [c] { c->prevKey(); });
  QAction* timeCfg = command(w, "anim.timeConfig", QObject::tr("Time Configuration..."), QKeySequence(), animicons::timeConfig());
  QObject::connect(timeCfg, &QAction::triggered, c, [c, &w] {
    TimeConfigDialog d(c, &w);
    d.exec();
  });

  auto* bar = new QWidget;
  bar->setObjectName("animationBar");
  auto* v = new QVBoxLayout(bar);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  v->addWidget(new TimeSlider(c, bar));
  v->addWidget(new TrackBar(c, bar));
  v->addWidget(new AnimControlBar(c, bar));
  w.addBelowViewports(bar);
}

}  // namespace

VERAA_APP_EXTENSION(animation, 300, installAnimation);

}  // namespace veraa
