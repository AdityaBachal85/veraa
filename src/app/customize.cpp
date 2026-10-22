#include "customize.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "extension.h"
#include "mainwindow.h"
#include "modifier.h"
#include "object.h"
#include "project.h"
#include "uistate.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

using json = nlohmann::json;

namespace {
QSettings appSettings() { return QSettings("Veraa", "Veraa"); }
void noHelp(QDialog* d) { d->setWindowFlag(Qt::WindowContextHelpButtonHint, false); }

QString groupOf(const QString& id) {
  QString g = id.section('.', 0, 0);
  if (g.isEmpty()) return QObject::tr("Other");
  g[0] = g[0].toUpper();
  return g;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Hotkey Editor

HotkeyEditorDialog::HotkeyEditorDialog(MainWindow* w) : QDialog(w), w_(w), keys_(w->hotkeys()) {
  noHelp(this);
  setWindowTitle(tr("Hotkey Editor"));
  resize(720, 560);
  auto* v = new QVBoxLayout(this);
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Search actions..."));
  filter_->setClearButtonEnabled(true);
  v->addWidget(filter_);
  table_ = new QTableWidget(0, 3, this);
  table_->setHorizontalHeaderLabels({tr("Action"), tr("Group"), tr("Hotkey")});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->verticalHeader()->hide();
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSortingEnabled(true);
  v->addWidget(table_, 1);
  auto* row = new QHBoxLayout;
  row->addWidget(new QLabel(tr("Hotkey:"), this));
  edit_ = new QKeySequenceEdit(this);
  row->addWidget(edit_, 1);
  auto* assignBtn = new QPushButton(tr("Assign"), this);
  auto* removeBtn = new QPushButton(tr("Remove"), this);
  row->addWidget(assignBtn);
  row->addWidget(removeBtn);
  v->addLayout(row);
  conflict_ = new QLabel(this);
  conflict_->setStyleSheet("color: #f0a040");
  v->addWidget(conflict_);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  QPushButton* save = box->addButton(tr("Save..."), QDialogButtonBox::ActionRole);
  QPushButton* load = box->addButton(tr("Load..."), QDialogButtonBox::ActionRole);
  QPushButton* reset = box->addButton(tr("Reset"), QDialogButtonBox::ResetRole);
  v->addWidget(box);

  connect(filter_, &QLineEdit::textChanged, this, &HotkeyEditorDialog::rebuild);
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] {
    QString id = currentId();
    edit_->setKeySequence(QKeySequence(keys_.value(id), QKeySequence::PortableText));
    updateConflict();
  });
  connect(edit_, &QKeySequenceEdit::keySequenceChanged, this, &HotkeyEditorDialog::updateConflict);
  connect(assignBtn, &QPushButton::clicked, this, [this] {
    QString id = currentId();
    if (id.isEmpty()) return;
    QString key = edit_->keySequence().toString(QKeySequence::PortableText);
    QString other = assign(id, key);
    if (!other.isEmpty())
      conflict_->setText(tr("Reassigned %1 from \"%2\"").arg(key, w_->command(other) ? w_->command(other)->text().remove('&') : other));
    rebuild();
  });
  connect(removeBtn, &QPushButton::clicked, this, [this] {
    QString id = currentId();
    if (id.isEmpty()) return;
    keys_[id] = QString();
    edit_->clear();
    rebuild();
  });
  connect(save, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getSaveFileName(this, tr("Save Hotkeys"), QDir(projectSubfolder("scripts")).filePath("hotkeys.vkeys"),
                                             tr("Hotkey files (*.vkeys)"));
    if (!f.isEmpty() && !saveKeys(f, keys_)) QMessageBox::warning(this, windowTitle(), tr("Could not write %1").arg(f));
  });
  connect(load, &QPushButton::clicked, this, [this] {
    QString f = QFileDialog::getOpenFileName(this, tr("Load Hotkeys"), projectSubfolder("scripts"), tr("Hotkey files (*.vkeys)"));
    if (f.isEmpty()) return;
    QHash<QString, QString> k;
    if (!loadKeys(f, &k)) {
      QMessageBox::warning(this, windowTitle(), tr("Could not read %1").arg(f));
      return;
    }
    for (auto it = k.begin(); it != k.end(); ++it)
      if (keys_.contains(it.key())) keys_[it.key()] = it.value();
    rebuild();
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    keys_ = w_->defaultHotkeys();
    rebuild();
  });
  connect(box, &QDialogButtonBox::accepted, this, [this] {
    w_->applyHotkeys(keys_, true);
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  rebuild();
}

QString HotkeyEditorDialog::assign(const QString& id, const QString& key) {
  QString previous;
  if (!key.isEmpty())
    for (auto it = keys_.begin(); it != keys_.end(); ++it)
      if (it.key() != id && it.value() == key) {
        previous = it.key();
        it.value() = QString();
      }
  keys_[id] = key;
  return previous;
}

QString HotkeyEditorDialog::currentId() const {
  int r = table_->currentRow();
  if (r < 0 || !table_->item(r, 0)) return QString();
  return table_->item(r, 0)->data(Qt::UserRole).toString();
}

void HotkeyEditorDialog::updateConflict() {
  QString key = edit_->keySequence().toString(QKeySequence::PortableText);
  QString id = currentId();
  conflict_->clear();
  if (key.isEmpty()) return;
  for (auto it = keys_.begin(); it != keys_.end(); ++it)
    if (it.key() != id && it.value() == key) {
      QAction* a = w_->command(it.key());
      conflict_->setText(tr("Conflict: %1 is used by \"%2\" (Assign moves it)")
                             .arg(edit_->keySequence().toString(QKeySequence::NativeText),
                                  a ? a->text().remove('&').section('\t', 0, 0) : it.key()));
    }
}

void HotkeyEditorDialog::rebuild() {
  QString cur = currentId();
  table_->setSortingEnabled(false);
  table_->setRowCount(0);
  QString f = filter_->text().trimmed();
  for (auto it = keys_.begin(); it != keys_.end(); ++it) {
    QAction* a = w_->command(it.key());
    if (!a) continue;
    QString text = a->text().remove('&').section('\t', 0, 0);
    if (!f.isEmpty() && !text.contains(f, Qt::CaseInsensitive) && !it.key().contains(f, Qt::CaseInsensitive) &&
        !it.value().contains(f, Qt::CaseInsensitive))
      continue;
    int row = table_->rowCount();
    table_->insertRow(row);
    auto* item = new QTableWidgetItem(a->icon(), text);
    item->setData(Qt::UserRole, it.key());
    item->setToolTip(it.key());
    table_->setItem(row, 0, item);
    table_->setItem(row, 1, new QTableWidgetItem(groupOf(it.key())));
    table_->setItem(row, 2, new QTableWidgetItem(QKeySequence(it.value(), QKeySequence::PortableText).toString(QKeySequence::NativeText)));
  }
  table_->setSortingEnabled(true);
  table_->sortByColumn(0, Qt::AscendingOrder);
  for (int r = 0; r < table_->rowCount(); ++r)
    if (table_->item(r, 0)->data(Qt::UserRole).toString() == cur) table_->selectRow(r);
}

bool HotkeyEditorDialog::saveKeys(const QString& path, const QHash<QString, QString>& keys) {
  json j;
  json k = json::object();
  for (auto it = keys.begin(); it != keys.end(); ++it) k[it.key().toStdString()] = it.value().toStdString();
  j["hotkeys"] = k;
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(QByteArray::fromStdString(j.dump(2)));
  return true;
}

bool HotkeyEditorDialog::loadKeys(const QString& path, QHash<QString, QString>* keys) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return false;
  json j = json::parse(f.readAll().toStdString(), nullptr, false);
  if (j.is_discarded() || !j.contains("hotkeys") || !j["hotkeys"].is_object()) return false;
  for (auto it = j["hotkeys"].begin(); it != j["hotkeys"].end(); ++it)
    if (it->is_string()) keys->insert(QString::fromStdString(it.key()), QString::fromStdString(it->get<std::string>()));
  return true;
}

// ---------------------------------------------------------------------------------------------
// UI schemes

bool saveUiScheme(MainWindow* w, const QString& pathIn) {
  QString path = pathIn;
  if (path.isEmpty()) {
    path = QFileDialog::getSaveFileName(w, QObject::tr("Save Custom UI Scheme"), QDir(projectSubfolder("scripts")).filePath("custom.vui"),
                                        QObject::tr("UI schemes (*.vui)"));
    if (path.isEmpty()) return false;
  }
  const UiState& ui = UiState::instance();
  json j;
  j["windowState"] = w->saveState().toBase64().toStdString();
  j["viewLayout"] = w->viewports()->viewLayout();
  json k = json::object();
  QHash<QString, QString> keys = w->hotkeys();
  for (auto it = keys.begin(); it != keys.end(); ++it) k[it.key().toStdString()] = it.value().toStdString();
  j["hotkeys"] = k;
  j["units"] = static_cast<int>(Units::instance().display());
  j["display"] = {{"viewCube", ui.showViewCube()},           {"gizmoSize", ui.gizmoSize()},
                  {"brackets", ui.showSelectionBrackets()}, {"frozenGray", ui.showFrozenInGray()},
                  {"wireObject", ui.wireUsesObjectColor()}, {"shadedObject", ui.shadedUsesObjectColor()}};
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(w, QObject::tr("Save Custom UI Scheme"), QObject::tr("Could not write %1").arg(path));
    return false;
  }
  f.write(QByteArray::fromStdString(j.dump(2)));
  w->setPrompt(QObject::tr("Saved UI scheme %1").arg(QFileInfo(path).fileName()));
  return true;
}

bool loadUiScheme(MainWindow* w, const QString& pathIn) {
  QString path = pathIn;
  if (path.isEmpty()) {
    path = QFileDialog::getOpenFileName(w, QObject::tr("Load Custom UI Scheme"), projectSubfolder("scripts"),
                                        QObject::tr("UI schemes (*.vui)"));
    if (path.isEmpty()) return false;
  }
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return false;
  json j = json::parse(f.readAll().toStdString(), nullptr, false);
  if (j.is_discarded()) {
    QMessageBox::warning(w, QObject::tr("Load Custom UI Scheme"), QObject::tr("%1 is not a UI scheme").arg(path));
    return false;
  }
  if (j.contains("windowState")) w->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(j["windowState"].get<std::string>())));
  if (j.contains("viewLayout")) w->viewports()->setViewLayout(j["viewLayout"].get<int>());
  if (j.contains("hotkeys") && j["hotkeys"].is_object()) {
    QHash<QString, QString> keys = w->hotkeys();
    for (auto it = j["hotkeys"].begin(); it != j["hotkeys"].end(); ++it)
      if (it->is_string() && keys.contains(QString::fromStdString(it.key())))
        keys[QString::fromStdString(it.key())] = QString::fromStdString(it->get<std::string>());
    w->applyHotkeys(keys, true);
  }
  if (j.contains("units")) Units::instance().setDisplay(static_cast<DisplayUnits>(j["units"].get<int>()));
  if (j.contains("display")) {
    UiState& ui = UiState::instance();
    const json& d = j["display"];
    ui.setShowViewCube(d.value("viewCube", true));
    ui.setGizmoSize(d.value("gizmoSize", 85.0f));
    ui.setShowSelectionBrackets(d.value("brackets", true));
    ui.setShowFrozenInGray(d.value("frozenGray", true));
    ui.setDisplayColors(d.value("wireObject", true), d.value("shadedObject", false));
  }
  w->setPrompt(QObject::tr("Loaded UI scheme %1").arg(QFileInfo(path).fileName()));
  return true;
}

// ---------------------------------------------------------------------------------------------
// Custom Defaults Switcher

QStringList CustomDefaultsDialog::presetNames() {
  return {QObject::tr("Default (centimeters)"), QObject::tr("Design - Metric (meters)"),
          QObject::tr("Architecture - Millimeters"), QObject::tr("US Standard (feet and inches)"),
          QObject::tr("Games and Assets (generic units)")};
}

void CustomDefaultsDialog::applyPreset(MainWindow* w, const QString& name) {
  int i = presetNames().indexOf(name);
  Units& u = Units::instance();
  UiState& ui = UiState::instance();
  QSettings s = appSettings();
  switch (i) {
    case 1:
      u.setDisplay(DisplayUnits::Meters);
      s.setValue("grid/spacing", 100.0);
      break;
    case 2:
      u.setDisplay(DisplayUnits::Millimeters);
      s.setValue("grid/spacing", 1.0);
      break;
    case 3:
      u.setDisplay(DisplayUnits::FeetInches);
      s.setValue("grid/spacing", 30.48);
      break;
    case 4:
      u.setDisplay(DisplayUnits::Generic);
      s.setValue("grid/spacing", 10.0);
      break;
    default:
      u.setDisplay(DisplayUnits::Centimeters);
      s.setValue("grid/spacing", 10.0);
      break;
  }
  ui.setShowViewCube(true);
  for (int k = 0; k < w->viewports()->count(); ++k) {
    Viewport* vp = w->viewports()->viewport(k);
    vp->setLighting(i == 1 || i == 2 ? ViewportLighting::Scene : ViewportLighting::Default);
  }
  s.setValue("defaults/preset", name);
  w->applyPreferences();
}

CustomDefaultsDialog::CustomDefaultsDialog(MainWindow* w) : QDialog(w) {
  noHelp(this);
  setWindowTitle(tr("Custom Defaults Switcher"));
  auto* v = new QVBoxLayout(this);
  v->addWidget(new QLabel(tr("Choose the initial settings for tool options and units:"), this));
  auto* list = new QComboBox(this);
  list->addItems(presetNames());
  int cur = presetNames().indexOf(appSettings().value("defaults/preset").toString());
  list->setCurrentIndex(cur < 0 ? 0 : cur);
  v->addWidget(list);
  auto* info = new QLabel(this);
  info->setWordWrap(true);
  v->addWidget(info);
  auto describe = [info](int i) {
    const char* text[] = {"Centimeter units, 10 cm home grid, default viewport lights.",
                          "Meter units, 1 m home grid, scene lights in the viewports.",
                          "Millimeter units, 1 cm home grid, scene lights in the viewports.",
                          "Feet with fractional inches, 1 ft home grid.",
                          "Generic units, 10 unit home grid."};
    info->setText(tr(text[std::clamp(i, 0, 4)]));
  };
  connect(list, QOverload<int>::of(&QComboBox::currentIndexChanged), this, describe);
  describe(list->currentIndex());
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [this, w, list] {
    applyPreset(w, list->currentText());
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------------------------
// Paths

PathsDialog::PathsDialog(MainWindow* w, bool userPaths) : QDialog(w) {
  noHelp(this);
  setWindowTitle(userPaths ? tr("Configure User and System Paths") : tr("Configure Project Paths"));
  resize(620, 360);
  auto* v = new QVBoxLayout(this);
  auto* form = new QFormLayout;
  QList<QPair<QString, QLineEdit*>> edits;
  auto addRow = [&](const QString& key, const QString& label, const QString& value, bool folderPicker) {
    auto* row = new QHBoxLayout;
    auto* e = new QLineEdit(value, this);
    row->addWidget(e, 1);
    if (folderPicker) {
      auto* b = new QPushButton(tr("Browse..."), this);
      connect(b, &QPushButton::clicked, this, [this, e] {
        QString d = QFileDialog::getExistingDirectory(this, tr("Choose Folder"), e->text());
        if (!d.isEmpty()) e->setText(d);
      });
      row->addWidget(b);
    }
    form->addRow(label, row);
    edits.push_back({key, e});
  };
  if (userPaths) {
    addRow("scripts", tr("User Scripts:"), userPath("scripts"), true);
    addRow("startupScripts", tr("Startup Scripts:"), userPath("startupScripts"), true);
    addRow("plugins", tr("Python Plug-ins:"), userPath("plugins"), true);
    addRow("temp", tr("Temporary Files:"), userPath("temp"), true);
  } else {
    addRow("__root", tr("Project Folder:"), projectFolder(), true);
    for (const QString& k : projectSubfolderKeys()) addRow(k, projectSubfolderLabel(k) + ":", projectSubfolderName(k), false);
  }
  v->addLayout(form);
  if (!userPaths) v->addWidget(new QLabel(tr("Subfolder names are relative to the project folder."), this));
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [this, edits, userPaths] {
    for (const auto& [key, e] : edits) {
      if (userPaths) {
        setUserPath(key, e->text());
        QDir().mkpath(e->text());
      } else if (key == "__root") {
        veraa::setProjectFolder(e->text());
      } else {
        setProjectSubfolderName(key, e->text());
      }
    }
    if (!userPaths) createProjectStructure(projectFolder());
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------------------------
// Units Setup

UnitsSetupDialog::UnitsSetupDialog(QWidget* parent) : QDialog(parent) {
  noHelp(this);
  setWindowTitle(tr("Units Setup"));
  Units& u = Units::instance();
  auto* v = new QVBoxLayout(this);
  auto* sys = new QPushButton(tr("System Unit Setup..."), this);
  v->addWidget(sys);
  auto* scale = new QGroupBox(tr("Display Unit Scale"), this);
  auto* g = new QGridLayout(scale);
  auto* metric = new QRadioButton(tr("Metric"), scale);
  auto* metricCombo = new QComboBox(scale);
  for (DisplayUnits d : {DisplayUnits::Millimeters, DisplayUnits::Centimeters, DisplayUnits::Meters, DisplayUnits::Kilometers})
    metricCombo->addItem(Units::unitName(d), static_cast<int>(d));
  auto* us = new QRadioButton(tr("US Standard"), scale);
  auto* usCombo = new QComboBox(scale);
  for (DisplayUnits d : {DisplayUnits::FeetInches, DisplayUnits::Inches, DisplayUnits::Feet})
    usCombo->addItem(Units::unitName(d), static_cast<int>(d));
  auto* generic = new QRadioButton(tr("Generic Units"), scale);
  g->addWidget(metric, 0, 0);
  g->addWidget(metricCombo, 1, 0);
  g->addWidget(us, 2, 0);
  g->addWidget(usCombo, 3, 0);
  g->addWidget(generic, 4, 0);
  v->addWidget(scale);
  DisplayUnits cur = u.display();
  int mi = metricCombo->findData(static_cast<int>(cur)), ui = usCombo->findData(static_cast<int>(cur));
  if (mi >= 0) {
    metric->setChecked(true);
    metricCombo->setCurrentIndex(mi);
  } else if (ui >= 0) {
    us->setChecked(true);
    usCombo->setCurrentIndex(ui);
  } else {
    generic->setChecked(true);
    metricCombo->setCurrentIndex(1);
  }
  auto* preview = new QLabel(this);
  v->addWidget(preview);
  auto* systemCm = new QDoubleSpinBox(this);
  systemCm->setRange(0.0001, 1e6);
  systemCm->setDecimals(4);
  systemCm->setValue(u.systemUnitCm());
  systemCm->setSuffix(tr(" cm"));
  systemCm->setVisible(false);
  connect(sys, &QPushButton::clicked, this, [this, systemCm] {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("System Unit Setup"));
    auto* f = new QFormLayout(&dlg);
    auto* sb = new QDoubleSpinBox(&dlg);
    sb->setRange(0.0001, 1e6);
    sb->setDecimals(4);
    sb->setValue(systemCm->value());
    sb->setSuffix(tr(" cm"));
    f->addRow(tr("1 Unit ="), sb);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    f->addRow(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() == QDialog::Accepted) systemCm->setValue(sb->value());
  });
  auto choice = [=] {
    if (metric->isChecked()) return static_cast<DisplayUnits>(metricCombo->currentData().toInt());
    if (us->isChecked()) return static_cast<DisplayUnits>(usCombo->currentData().toInt());
    return DisplayUnits::Generic;
  };
  auto update = [=] { preview->setText(tr("Display: %1").arg(Units::unitName(choice()))); };
  for (QRadioButton* r : {metric, us, generic}) connect(r, &QRadioButton::toggled, this, update);
  connect(metricCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
  connect(usCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, update);
  update();
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [this, choice, systemCm] {
    Units::instance().setSystemUnitCm(systemCm->value());
    Units::instance().setDisplay(choice());
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------------------------
// Plug-in Manager

PluginManagerDialog::PluginManagerDialog(QWidget* parent) : QDialog(parent) {
  noHelp(this);
  setWindowTitle(tr("Plug-in Manager"));
  resize(560, 520);
  auto* v = new QVBoxLayout(this);
  auto* tree = new QTreeWidget(this);
  tree->setHeaderLabels({tr("Plug-in"), tr("Kind"), tr("Status")});
  tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  auto* mods = new QTreeWidgetItem(tree, {tr("Interface modules"), QString(), QString()});
  for (const QString& n : appExtensionNames()) new QTreeWidgetItem(mods, {n, tr("Module"), tr("Loaded")});
  auto* objs = new QTreeWidgetItem(tree, {tr("Object types"), QString(), QString()});
  std::map<std::string, QTreeWidgetItem*> cats;
  for (const ObjectTypeInfo& t : objectTypes()) {
    QTreeWidgetItem*& c = cats[t.category];
    if (!c) c = new QTreeWidgetItem(objs, {QString::fromStdString(t.category), QString(), QString()});
    new QTreeWidgetItem(c, {QString::fromStdString(t.displayName), QString::fromStdString(t.subcategory), tr("Loaded")});
  }
  auto* modTypes = new QTreeWidgetItem(tree, {tr("Modifiers"), QString(), QString()});
  for (const ModifierTypeInfo& m : modifierTypeInfos())
    new QTreeWidgetItem(modTypes, {QString::fromStdString(m.displayName), QString::fromStdString(m.category), tr("Loaded")});
  auto* py = new QTreeWidgetItem(tree, {tr("Python plug-ins"), QString(), QString()});
  QStringList disabled = appSettings().value("plugins/disabledScripts").toStringList();
  QDir pluginDir(userPath("plugins"));
  for (const QString& f : pluginDir.entryList({"*.py"}, QDir::Files)) {
    auto* it = new QTreeWidgetItem(py, {f, tr("Python"), QString()});
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(2, disabled.contains(f) ? Qt::Unchecked : Qt::Checked);
    it->setText(2, disabled.contains(f) ? tr("Deferred") : tr("Loads at startup"));
  }
  mods->setExpanded(true);
  py->setExpanded(true);
  v->addWidget(tree);
  v->addWidget(new QLabel(tr("Python plug-ins are .py files in %1, run at startup.").arg(pluginDir.absolutePath()), this));
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [this, py] {
    QStringList off;
    for (int i = 0; i < py->childCount(); ++i)
      if (py->child(i)->checkState(2) == Qt::Unchecked) off << py->child(i)->text(0);
    QSettings s = appSettings();
    s.setValue("plugins/disabledScripts", off);
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------------------------
// Preferences

PreferencesDialog::PreferencesDialog(MainWindow* w) : QDialog(w) {
  noHelp(this);
  setWindowTitle(tr("Preference Settings"));
  resize(520, 420);
  QSettings s = appSettings();
  auto* v = new QVBoxLayout(this);
  auto* tabs = new QTabWidget(this);
  v->addWidget(tabs);

  auto* general = new QWidget(tabs);
  auto* gf = new QFormLayout(general);
  auto* undo = new QSpinBox(general);
  undo->setRange(0, 1000);
  undo->setValue(s.value("prefs/undoLevels", 0).toInt());
  undo->setSpecialValueText(tr("Unlimited"));
  gf->addRow(tr("Scene undo levels:"), undo);
  auto* spinnerSnap = new QDoubleSpinBox(general);
  spinnerSnap->setRange(0.001, 1000);
  spinnerSnap->setDecimals(3);
  spinnerSnap->setValue(s.value("prefs/spinnerSnap", 1.0).toDouble());
  gf->addRow(tr("Spinner snap:"), spinnerSnap);
  auto* recent = new QSpinBox(general);
  recent->setRange(1, 30);
  recent->setValue(s.value("prefs/recentCount", 10).toInt());
  gf->addRow(tr("Recent files in File menu:"), recent);
  tabs->addTab(general, tr("General"));

  auto* files = new QWidget(tabs);
  auto* ff = new QFormLayout(files);
  auto* backup = new QCheckBox(tr("Enable auto backup"), files);
  backup->setChecked(s.value("prefs/autoBackup", true).toBool());
  ff->addRow(backup);
  auto* minutes = new QSpinBox(files);
  minutes->setRange(1, 240);
  minutes->setValue(s.value("prefs/autoBackupMinutes", 5).toInt());
  minutes->setSuffix(tr(" min"));
  ff->addRow(tr("Backup interval:"), minutes);
  auto* count = new QSpinBox(files);
  count->setRange(1, 99);
  count->setValue(s.value("prefs/autoBackupFiles", 3).toInt());
  ff->addRow(tr("Number of autoback files:"), count);
  auto* increment = new QCheckBox(tr("Increment on save (scene01, scene02, ...)"), files);
  increment->setChecked(s.value("prefs/incrementOnSave", false).toBool());
  ff->addRow(increment);
  ff->addRow(new QLabel(tr("Backups go to the project's autoback folder."), files));
  tabs->addTab(files, tr("Files"));

  auto* viewports = new QWidget(tabs);
  auto* vf = new QFormLayout(viewports);
  auto* grid = new UnitSpinBox(viewports);
  grid->setRange(0.001, 1e6);
  grid->setValue(s.value("grid/spacing", 10.0).toDouble());
  vf->addRow(tr("Home grid spacing:"), grid);
  UiState& ui = UiState::instance();
  auto* gizmo = new QSpinBox(viewports);
  gizmo->setRange(30, 250);
  gizmo->setValue(static_cast<int>(ui.gizmoSize()));
  vf->addRow(tr("Transform gizmo size:"), gizmo);
  auto* cube = new QCheckBox(tr("Show the ViewCube"), viewports);
  cube->setChecked(ui.showViewCube());
  vf->addRow(cube);
  auto* adaptive = new QCheckBox(tr("Adaptive degradation"), viewports);
  adaptive->setChecked(ui.adaptiveDegradation());
  vf->addRow(adaptive);
  tabs->addTab(viewports, tr("Viewports"));

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [=] {
    QSettings st = appSettings();
    st.setValue("prefs/undoLevels", undo->value());
    st.setValue("prefs/spinnerSnap", spinnerSnap->value());
    st.setValue("prefs/recentCount", recent->value());
    st.setValue("prefs/autoBackup", backup->isChecked());
    st.setValue("prefs/autoBackupMinutes", minutes->value());
    st.setValue("prefs/autoBackupFiles", count->value());
    st.setValue("prefs/incrementOnSave", increment->isChecked());
    st.setValue("grid/spacing", grid->value());
    UiState& u = UiState::instance();
    u.setGizmoSize(static_cast<float>(gizmo->value()));
    u.setShowViewCube(cube->isChecked());
    u.setAdaptiveDegradation(adaptive->isChecked());
    w->applyPreferences();
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

}  // namespace veraa
