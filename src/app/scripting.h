// Embedded Python (CPython) with the `veraa` module, the macro recorder and script running.
// Built when CMake finds Python development files (option VERAA_WITH_PYTHON); otherwise the
// engine reports that scripting is unavailable.
//
// Python API overview (see Scripting > Python API Help):
//   import veraa
//   b = veraa.create("Box", width=50, length=50, height=20, position=(0, 0, 0))
//   b.position = (100, 0, 0); b.rotation = (0, 0, 45); b.name = "Crate"
//   b["height"] = 35                    # object parameters
//   b.add_modifier("Bend")
//   for n in veraa.nodes(): print(n.name, n.type)
//   veraa.select([b]); veraa.command("render.render")
#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>

#include "scene.h"

namespace veraa {

class MainWindow;

class ScriptEngine : public QObject {
  Q_OBJECT
 public:
  static ScriptEngine& instance();

  // Binds the engine to the window whose scene scripts edit. Call once.
  void attach(MainWindow* w);
  MainWindow* window() const { return window_; }
  bool available() const;
  QString version() const;
  QString unavailableReason() const;

  // Runs code. Interactive mode echoes expression values like the Python prompt. Output (stdout
  // and stderr) goes to *output and to outputWritten. Returns false on errors.
  bool run(const QString& code, QString* output = nullptr, bool interactive = false);
  bool runFile(const QString& path, QString* output = nullptr);
  // Runs every .py file in the startup scripts and plug-in folders.
  void runStartupScripts();

  // ---- Macro recorder: records edits as Python lines ----
  bool recording() const { return recording_; }
  void setRecording(bool on);
  const QStringList& recorded() const { return recorded_; }
  void clearRecorded() { recorded_.clear(); }
  // Adds a line (also used by commands to record themselves).
  void record(const QString& line);

  // Called from the Python bindings.
  void write(const QString& text);

 Q_SIGNALS:
  void outputWritten(const QString& text);
  void recordedLine(const QString& line);
  void recordingChanged(bool on);

 private:
  ScriptEngine() = default;
  bool ensureInitialized();
  void snapshot();
  void recordChanges();

  MainWindow* window_ = nullptr;
  bool initialized_ = false;
  bool failed_ = false;
  QString error_;
  QString* sink_ = nullptr;
  bool recording_ = false;
  QStringList recorded_;
  bool running_ = false;
  struct NodeState {
    std::string type, name;
    Transform xf;
    json params;
    uint32_t parent = 0;
    int modifiers = 0;
  };
  QHash<uint32_t, NodeState> before_;
};

// Python-literal of a string (for recorded macros).
QString pyString(const QString& s);

}  // namespace veraa
