// Scripting UI: the Python listener (full window and the two-line mini listener in the status
// bar), the script editor with syntax highlighting, and the Scripting menu.
#pragma once

#include <QPointer>
#include <QSyntaxHighlighter>
#include <QWidget>

class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QTextDocument;

namespace veraa {

class MainWindow;

class ScriptListener : public QWidget {
  Q_OBJECT
 public:
  enum Mode { Mini, Full };
  ScriptListener(MainWindow* w, Mode mode, QWidget* parent = nullptr);
  // Runs one entry as if typed (also used by tests). Returns the output text.
  QString execute(const QString& code);
  QLineEdit* input() const { return input_; }

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  void append(const QString& text);
  MainWindow* w_;
  Mode mode_;
  QLineEdit* input_ = nullptr;
  QLineEdit* lastLine_ = nullptr;      // mini: last output / recorded line
  QPlainTextEdit* output_ = nullptr;   // full: transcript
  QPlainTextEdit* recorder_ = nullptr; // full: macro recorder pane
  QStringList history_;
  int historyPos_ = 0;
};

class PythonHighlighter : public QSyntaxHighlighter {
  Q_OBJECT
 public:
  explicit PythonHighlighter(QTextDocument* doc);

 protected:
  void highlightBlock(const QString& text) override;
};

class ScriptEditor : public QWidget {
  Q_OBJECT
 public:
  explicit ScriptEditor(MainWindow* w, const QString& path = QString());
  bool load(const QString& path);
  bool save();
  bool saveAs();
  void evaluateAll();
  void evaluateSelection();
  QPlainTextEdit* editor() const { return edit_; }

 protected:
  void closeEvent(QCloseEvent* e) override;

 private:
  void updateTitle();
  MainWindow* w_;
  QString path_;
  QPlainTextEdit* edit_;
  QPlainTextEdit* output_;
};

// Fills the Scripting menu and registers scripting.* commands.
void installScriptingMenu(MainWindow& w, QMenu* menu);
// Opens (or raises) the full listener window.
void showListener(MainWindow& w);

}  // namespace veraa
