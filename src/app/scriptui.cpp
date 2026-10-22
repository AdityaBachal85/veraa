#include "scriptui.h"

#include <QAction>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include "icons_ui.h"
#include "mainwindow.h"
#include "project.h"
#include "scripting.h"

namespace veraa {

namespace {

QFont monoFont() {
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  f.setPointSizeF(std::max(9.0, f.pointSizeF()));
  return f;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Listener

ScriptListener::ScriptListener(MainWindow* w, Mode mode, QWidget* parent) : QWidget(parent), w_(w), mode_(mode) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(1);
  ScriptEngine& engine = ScriptEngine::instance();
  if (mode == Mini) {
    lastLine_ = new QLineEdit(this);
    lastLine_->setReadOnly(true);
    lastLine_->setObjectName("MiniListenerOutput");
    lastLine_->setStyleSheet("QLineEdit { background: #4a3a40; color: #f0e0e6; border: none; }");
    lastLine_->setPlaceholderText(tr("Python output"));
    lastLine_->setToolTip(tr("Last Python output or recorded macro line (right-click for the listener)"));
    input_ = new QLineEdit(this);
    input_->setObjectName("MiniListenerInput");
    input_->setStyleSheet("QLineEdit { background: #e8e8e8; color: #101010; border: none; }");
    input_->setPlaceholderText(tr("Python: type and press Enter"));
    v->addWidget(lastLine_);
    v->addWidget(input_);
    for (QWidget* x : {static_cast<QWidget*>(lastLine_), static_cast<QWidget*>(input_)}) {
      x->setContextMenuPolicy(Qt::CustomContextMenu);
      connect(x, &QWidget::customContextMenuRequested, this, [this](const QPoint&) {
        QMenu menu(this);
        menu.addAction(tr("Open Script Listener"), this, [this] { showListener(*w_); });
        menu.addAction(tr("Clear"), this, [this] {
          lastLine_->clear();
          input_->clear();
        });
        menu.exec(QCursor::pos());
      });
    }
    connect(&engine, &ScriptEngine::recordedLine, this, [this](const QString& line) { lastLine_->setText(line); });
  } else {
    auto* split = new QSplitter(Qt::Vertical, this);
    recorder_ = new QPlainTextEdit(split);
    recorder_->setObjectName("MacroRecorderPane");
    recorder_->setFont(monoFont());
    recorder_->setPlaceholderText(tr("Macro Recorder: turn it on from the Scripting menu; your actions appear here as Python."));
    recorder_->setStyleSheet("QPlainTextEdit { background: #3d3034; color: #f3dfe6; }");
    output_ = new QPlainTextEdit(split);
    output_->setObjectName("ListenerOutput");
    output_->setReadOnly(true);
    output_->setFont(monoFont());
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    v->addWidget(split, 1);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(">>>", this));
    input_ = new QLineEdit(this);
    input_->setObjectName("ListenerInput");
    input_->setFont(monoFont());
    row->addWidget(input_, 1);
    v->addLayout(row);
    for (const QString& line : engine.recorded()) recorder_->appendPlainText(line);
    connect(&engine, &ScriptEngine::recordedLine, this, [this](const QString& line) { recorder_->appendPlainText(line); });
    if (engine.available())
      append(tr("Python %1 - type Python, e.g.  b = veraa.create(\"Box\", width=40)  (help(veraa) for the API)\n")
                 .arg(engine.version().isEmpty() ? tr("scripting") : engine.version()));
    else
      append(engine.unavailableReason() + "\n");
  }
  input_->installEventFilter(this);
  connect(input_, &QLineEdit::returnPressed, this, [this] {
    QString code = input_->text();
    if (code.trimmed().isEmpty()) return;
    input_->clear();
    execute(code);
  });
}

QString ScriptListener::execute(const QString& code) {
  history_.push_back(code);
  historyPos_ = history_.size();
  if (output_) append(">>> " + code + "\n");
  QString out;
  ScriptEngine::instance().run(code, &out, true);
  if (output_) {
    append(out);
  } else if (lastLine_) {
    QString shown = out.trimmed();
    lastLine_->setText(shown.isEmpty() ? code : shown.section('\n', -1));
    lastLine_->setToolTip(out);
  }
  return out;
}

void ScriptListener::append(const QString& text) {
  if (!output_ || text.isEmpty()) return;
  QTextCursor c = output_->textCursor();
  c.movePosition(QTextCursor::End);
  c.insertText(text);
  output_->setTextCursor(c);
  output_->ensureCursorVisible();
}

bool ScriptListener::eventFilter(QObject* obj, QEvent* e) {
  if (obj == input_ && e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Up && !history_.isEmpty()) {
      historyPos_ = std::max(0, historyPos_ - 1);
      input_->setText(history_.value(historyPos_));
      return true;
    }
    if (k->key() == Qt::Key_Down && !history_.isEmpty()) {
      historyPos_ = std::min(static_cast<int>(history_.size()), historyPos_ + 1);
      input_->setText(history_.value(historyPos_));
      return true;
    }
  }
  return QWidget::eventFilter(obj, e);
}

// ---------------------------------------------------------------------------------------------
// Highlighter

PythonHighlighter::PythonHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc) {}

void PythonHighlighter::highlightBlock(const QString& text) {
  static const QRegularExpression keywords(
      R"(\b(and|as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield|True|False|None)\b)");
  static const QRegularExpression builtins(R"(\b(print|len|range|list|dict|set|tuple|int|float|str|enumerate|zip|min|max|abs|round|sorted|isinstance|veraa)\b)");
  static const QRegularExpression numbers(R"(\b\d+(\.\d+)?([eE][-+]?\d+)?\b)");
  static const QRegularExpression strings(R"("([^"\\]|\\.)*"|'([^'\\]|\\.)*')");
  static const QRegularExpression comment(R"(#[^\n]*)");
  QTextCharFormat kw, bi, num, str, com;
  kw.setForeground(QColor(0xc5, 0x86, 0xc0));
  kw.setFontWeight(QFont::Bold);
  bi.setForeground(QColor(0x4e, 0xc9, 0xb0));
  num.setForeground(QColor(0xb5, 0xce, 0xa8));
  str.setForeground(QColor(0xce, 0x91, 0x78));
  com.setForeground(QColor(0x6a, 0x99, 0x55));
  auto apply = [&](const QRegularExpression& re, const QTextCharFormat& f) {
    for (auto it = re.globalMatch(text); it.hasNext();) {
      auto m = it.next();
      setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), f);
    }
  };
  apply(keywords, kw);
  apply(builtins, bi);
  apply(numbers, num);
  apply(strings, str);
  // Comments last, but not inside strings.
  for (auto it = comment.globalMatch(text); it.hasNext();) {
    auto m = it.next();
    if (format(static_cast<int>(m.capturedStart())) != str) {
      setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(text.size() - m.capturedStart()), com);
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Script editor

ScriptEditor::ScriptEditor(MainWindow* w, const QString& path) : QWidget(w, Qt::Window), w_(w) {
  setAttribute(Qt::WA_DeleteOnClose);
  resize(780, 620);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  auto* bar = new QMenuBar(this);
  v->setMenuBar(bar);
  QMenu* file = bar->addMenu(tr("&File"));
  file->addAction(tr("&New"), QKeySequence::New, this, [w] { (new ScriptEditor(w))->show(); });
  file->addAction(tr("&Open..."), QKeySequence::Open, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, tr("Open Script"), projectSubfolder("scripts"), tr("Python scripts (*.py)"));
    if (!f.isEmpty()) load(f);
  });
  file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(); });
  file->addAction(tr("Save &As..."), QKeySequence("Ctrl+Shift+S"), this, [this] { saveAs(); });
  file->addSeparator();
  file->addAction(tr("&Close"), QKeySequence::Close, this, &QWidget::close);
  QMenu* tools = bar->addMenu(tr("&Tools"));
  tools->addAction(tr("&Evaluate All"), QKeySequence("Ctrl+E"), this, &ScriptEditor::evaluateAll);
  tools->addAction(tr("Evaluate &Selection"), QKeySequence("Shift+Return"), this, &ScriptEditor::evaluateSelection);
  tools->addSeparator();
  tools->addAction(tr("Clear Output"), this, [this] { output_->clear(); });
  auto* split = new QSplitter(Qt::Vertical, this);
  edit_ = new QPlainTextEdit(split);
  edit_->setObjectName("ScriptEditorText");
  edit_->setFont(monoFont());
  edit_->setTabStopDistance(QFontMetricsF(edit_->font()).horizontalAdvance(' ') * 4);
  edit_->setLineWrapMode(QPlainTextEdit::NoWrap);
  new PythonHighlighter(edit_->document());
  output_ = new QPlainTextEdit(split);
  output_->setReadOnly(true);
  output_->setFont(monoFont());
  output_->setMaximumHeight(220);
  split->setStretchFactor(0, 4);
  split->setStretchFactor(1, 1);
  v->addWidget(split);
  connect(edit_->document(), &QTextDocument::modificationChanged, this, &ScriptEditor::updateTitle);
  if (!path.isEmpty()) load(path);
  else edit_->setPlainText("import veraa\n\n");
  edit_->document()->setModified(false);
  updateTitle();
}

void ScriptEditor::updateTitle() {
  QString name = path_.isEmpty() ? tr("Untitled") : QFileInfo(path_).fileName();
  setWindowTitle(tr("%1%2 - Script Editor").arg(name, edit_->document()->isModified() ? " *" : ""));
}

bool ScriptEditor::load(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, tr("Open Script"), tr("Cannot open %1").arg(path));
    return false;
  }
  edit_->setPlainText(QString::fromUtf8(f.readAll()));
  path_ = path;
  edit_->document()->setModified(false);
  updateTitle();
  return true;
}

bool ScriptEditor::save() {
  if (path_.isEmpty()) return saveAs();
  QFile f(path_);
  if (!f.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(this, tr("Save Script"), tr("Cannot write %1").arg(path_));
    return false;
  }
  f.write(edit_->toPlainText().toUtf8());
  edit_->document()->setModified(false);
  updateTitle();
  return true;
}

bool ScriptEditor::saveAs() {
  QString f = QFileDialog::getSaveFileName(this, tr("Save Script As"),
                                           path_.isEmpty() ? QDir(projectSubfolder("scripts")).filePath("script.py") : path_,
                                           tr("Python scripts (*.py)"));
  if (f.isEmpty()) return false;
  if (QFileInfo(f).suffix().isEmpty()) f += ".py";
  path_ = f;
  return save();
}

void ScriptEditor::evaluateAll() {
  QString out;
  bool ok = ScriptEngine::instance().run(edit_->toPlainText(), &out, false);
  output_->appendPlainText(out.isEmpty() ? (ok ? tr("OK") : tr("Failed")) : out.trimmed());
}

void ScriptEditor::evaluateSelection() {
  QString code = edit_->textCursor().selectedText().replace(QChar(0x2029), '\n');
  if (code.isEmpty()) code = edit_->textCursor().block().text();
  QString out;
  ScriptEngine::instance().run(code, &out, true);
  if (!out.isEmpty()) output_->appendPlainText(out.trimmed());
}

void ScriptEditor::closeEvent(QCloseEvent* e) {
  if (edit_->document()->isModified()) {
    auto r = QMessageBox::question(this, windowTitle(), tr("Save changes to the script?"),
                                   QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel || (r == QMessageBox::Save && !save())) {
      e->ignore();
      return;
    }
  }
  e->accept();
}

// ---------------------------------------------------------------------------------------------
// Menu

void showListener(MainWindow& w) {
  auto* dlg = w.findChild<QDialog*>("ScriptListenerWindow");
  if (!dlg) {
    dlg = new QDialog(&w);
    dlg->setObjectName("ScriptListenerWindow");
    dlg->setWindowTitle(QObject::tr("Script Listener"));
    dlg->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dlg->resize(720, 480);
    auto* v = new QVBoxLayout(dlg);
    v->setContentsMargins(2, 2, 2, 2);
    auto* listener = new ScriptListener(&w, ScriptListener::Full, dlg);
    v->addWidget(listener);
  }
  dlg->show();
  dlg->raise();
  dlg->activateWindow();
  if (auto* l = dlg->findChild<ScriptListener*>()) l->input()->setFocus();
}

void installScriptingMenu(MainWindow& w, QMenu* menu) {
  auto add = [&](const QString& id, const QString& text, const QKeySequence& key, const std::function<void()>& fn) {
    auto* a = new QAction(text, &w);
    if (!key.isEmpty()) a->setShortcut(key);
    QObject::connect(a, &QAction::triggered, &w, fn);
    w.registerCommand(id, a);
    menu->addAction(a);
    return a;
  };
  MainWindow* win = &w;
  add("scripting.new", QObject::tr("&New Script"), QKeySequence(), [win] { (new ScriptEditor(win))->show(); });
  add("scripting.open", QObject::tr("&Open Script..."), QKeySequence(), [win] {
    QString f = QFileDialog::getOpenFileName(win, QObject::tr("Open Script"), projectSubfolder("scripts"),
                                             QObject::tr("Python scripts (*.py)"));
    if (!f.isEmpty()) (new ScriptEditor(win, f))->show();
  });
  add("scripting.run", QObject::tr("&Run Script..."), QKeySequence(), [win] {
    QString f = QFileDialog::getOpenFileName(win, QObject::tr("Run Script"), projectSubfolder("scripts"),
                                             QObject::tr("Python scripts (*.py)"));
    if (f.isEmpty()) return;
    QString out;
    bool ok = ScriptEngine::instance().runFile(f, &out);
    win->setPrompt(ok ? QObject::tr("Ran %1").arg(QFileInfo(f).fileName()) : QObject::tr("Script failed: see the listener"));
    if (!ok) {
      showListener(*win);
      if (auto* l = win->findChild<QDialog*>("ScriptListenerWindow")->findChild<QPlainTextEdit*>("ListenerOutput"))
        l->appendPlainText(out);
    }
  });
  menu->addSeparator();
  add("scripting.listener", QObject::tr("Script &Listener..."), QKeySequence("F11"), [win] { showListener(*win); });
  add("scripting.editor", QObject::tr("Script &Editor..."), QKeySequence(), [win] { (new ScriptEditor(win))->show(); });
  QAction* rec = add("scripting.macroRecorder", QObject::tr("&Macro Recorder"), QKeySequence(), [] {});
  rec->setCheckable(true);
  QObject::connect(rec, &QAction::toggled, &w, [](bool on) { ScriptEngine::instance().setRecording(on); });
  QObject::connect(&ScriptEngine::instance(), &ScriptEngine::recordingChanged, rec, [rec](bool on) {
    QSignalBlocker b(rec);
    rec->setChecked(on);
  });
  add("scripting.recordToEditor", QObject::tr("Recorded Macro to Script Editor"), QKeySequence(), [win] {
    auto* ed = new ScriptEditor(win);
    ed->editor()->setPlainText("import veraa\n\n" + ScriptEngine::instance().recorded().join('\n') + '\n');
    ed->show();
  });
  menu->addSeparator();
  add("scripting.help", QObject::tr("Python API &Help"), QKeySequence(), [win] {
    QDialog dlg(win);
    dlg.setWindowTitle(QObject::tr("Python API Help"));
    dlg.resize(640, 560);
    auto* v = new QVBoxLayout(&dlg);
    auto* text = new QTextBrowser(&dlg);
    QString out;
    ScriptEngine::instance().run(
        "import veraa\nprint(veraa.__doc__)\nprint('Node:', veraa.Node.__doc__)\n"
        "print('Object types:', ', '.join(veraa.object_types()))\nprint('Modifiers:', ', '.join(veraa.modifier_types()))\n",
        &out, false);
    text->setPlainText(out);
    text->setFont(monoFont());
    v->addWidget(text);
    dlg.exec();
  });
  add("scripting.openFolder", QObject::tr("Open Scripts &Folder"), QKeySequence(), [] {
    QString dir = userPath("scripts");
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
  });
}

}  // namespace veraa
