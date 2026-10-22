// Customize menu dialogs: Hotkey Editor (Customize User Interface), UI schemes, Custom Defaults
// Switcher, project and user paths, Units Setup, Plug-in Manager and Preferences.
#pragma once

#include <QDialog>
#include <QHash>

class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QTableWidget;

namespace veraa {

class MainWindow;

class HotkeyEditorDialog : public QDialog {
  Q_OBJECT
 public:
  explicit HotkeyEditorDialog(MainWindow* w);
  // Assigns `key` to command `id`, removing it from any other command. Returns the id that
  // previously used the key (empty if none).
  QString assign(const QString& id, const QString& key);
  const QHash<QString, QString>& keys() const { return keys_; }
  // Saves/loads a hotkey file (.vkeys, JSON: {"hotkeys": {id: key}}).
  static bool saveKeys(const QString& path, const QHash<QString, QString>& keys);
  static bool loadKeys(const QString& path, QHash<QString, QString>* keys);

 private:
  void rebuild();
  QString currentId() const;
  void updateConflict();
  MainWindow* w_;
  QHash<QString, QString> keys_;
  QLineEdit* filter_;
  QTableWidget* table_;
  QKeySequenceEdit* edit_;
  QLabel* conflict_;
};

class CustomDefaultsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit CustomDefaultsDialog(MainWindow* w);
  static QStringList presetNames();
  static void applyPreset(MainWindow* w, const QString& name);
};

class PathsDialog : public QDialog {
  Q_OBJECT
 public:
  PathsDialog(MainWindow* w, bool userPaths);
};

class UnitsSetupDialog : public QDialog {
  Q_OBJECT
 public:
  explicit UnitsSetupDialog(QWidget* parent);
};

class PluginManagerDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PluginManagerDialog(QWidget* parent);
};

class PreferencesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(MainWindow* w);
};

// Customize > Load / Save Custom UI Scheme (.vui: window layout, viewport layout, hotkeys,
// units and display settings).
bool saveUiScheme(MainWindow* w, const QString& path = QString());
bool loadUiScheme(MainWindow* w, const QString& path = QString());

}  // namespace veraa
