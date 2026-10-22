#include "theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace veraa {

void applyDarkTheme(QApplication& app) {
  app.setStyle(QStyleFactory::create("Fusion"));
  QPalette p;
  QColor window(0x44, 0x44, 0x44), base(0x2f, 0x2f, 0x2f), alt(0x3a, 0x3a, 0x3a), text(0xdc, 0xdc, 0xdc);
  QColor button(0x52, 0x52, 0x52), highlight(0x4f, 0x8f, 0xcf);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, alt);
  p.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2a, 0x2a));
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, button);
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::BrightText, Qt::white);
  p.setColor(QPalette::Highlight, highlight);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::Link, QColor(0x6f, 0xb0, 0xf0));
  p.setColor(QPalette::Light, QColor(0x60, 0x60, 0x60));
  p.setColor(QPalette::Midlight, QColor(0x58, 0x58, 0x58));
  p.setColor(QPalette::Mid, QColor(0x38, 0x38, 0x38));
  p.setColor(QPalette::Dark, QColor(0x26, 0x26, 0x26));
  p.setColor(QPalette::Shadow, QColor(0x18, 0x18, 0x18));
  QColor disabled(0x80, 0x80, 0x80);
  p.setColor(QPalette::Disabled, QPalette::Text, disabled);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
  p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
  app.setPalette(p);

  app.setStyleSheet(R"(
    QToolTip { color: #dcdcdc; background-color: #2a2a2a; border: 1px solid #555; padding: 3px; }
    QMenuBar { background: #3a3a3a; border-bottom: 1px solid #2a2a2a; }
    QMenuBar::item { padding: 4px 9px; background: transparent; }
    QMenuBar::item:selected { background: #555; }
    QMenu { background: #3c3c3c; border: 1px solid #222; }
    QMenu::item { padding: 4px 26px 4px 22px; }
    QMenu::item:selected { background: #4f8fcf; color: white; }
    QMenu::separator { height: 1px; background: #555; margin: 3px 6px; }
    QToolBar { background: #3d3d3d; border: none; spacing: 2px; padding: 2px; }
    QToolBar::separator { background: #555; width: 1px; margin: 4px 4px; }
    QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 2px; }
    QToolButton:hover { background: #575757; border: 1px solid #666; }
    QToolButton:checked { background: #4f8fcf; border: 1px solid #6fa8e0; }
    QToolButton:pressed { background: #3a6ea3; }
    QDockWidget { titlebar-close-icon: none; }
    QDockWidget::title { background: #3a3a3a; padding: 4px 6px; border-bottom: 1px solid #2a2a2a; }
    QTabWidget::pane { border: 1px solid #2a2a2a; top: -1px; }
    QTabBar::tab { background: #3a3a3a; padding: 5px 12px; border: 1px solid #2a2a2a; border-bottom: none;
                   min-width: 48px; }
    QTabBar::tab:selected { background: #4a4a4a; color: white; }
    QTabBar::tab:!selected { color: #b0b0b0; }
    QPushButton { background: #555; border: 1px solid #2c2c2c; border-radius: 2px; padding: 4px 10px; }
    QPushButton:hover { background: #626262; }
    QPushButton:pressed { background: #3f3f3f; }
    QPushButton:checked { background: #4f8fcf; color: white; border: 1px solid #6fa8e0; }
    QPushButton:disabled { color: #808080; background: #4a4a4a; }
    QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: #2c2c2c; border: 1px solid #222;
                   border-radius: 2px; padding: 1px 3px; selection-background-color: #4f8fcf; }
    QSpinBox, QDoubleSpinBox { padding-right: 14px; }
    QComboBox QAbstractItemView { background: #333; border: 1px solid #222; selection-background-color: #4f8fcf; }
    QListView, QTreeView, QListWidget, QTreeWidget { background: #2f2f2f; border: 1px solid #222; }
    QHeaderView::section { background: #3c3c3c; border: none; border-right: 1px solid #2a2a2a;
                           border-bottom: 1px solid #2a2a2a; padding: 3px 5px; }
    QScrollBar:vertical { background: #333; width: 11px; margin: 0; }
    QScrollBar::handle:vertical { background: #5a5a5a; min-height: 24px; border-radius: 4px; margin: 2px; }
    QScrollBar:horizontal { background: #333; height: 11px; margin: 0; }
    QScrollBar::handle:horizontal { background: #5a5a5a; min-width: 24px; border-radius: 4px; margin: 2px; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QStatusBar { background: #3a3a3a; }
    QSplitter::handle { background: #262626; }
    QGroupBox { border: 1px solid #2a2a2a; margin-top: 8px; padding-top: 6px; }
    QGroupBox::title { subcontrol-origin: margin; left: 6px; padding: 0 3px; }
    QToolButton#RolloutHeader { background: #505050; border: 1px solid #2c2c2c; border-radius: 0;
                                text-align: left; padding: 3px 4px; font-weight: bold; color: #e6e6e6; }
    QToolButton#RolloutHeader:hover { background: #5a5a5a; }
    QFrame#RolloutBody { background: #444; border: 1px solid #2c2c2c; border-top: none; }
    QLabel#Prompt { color: #c8c8c8; }
  )");
}

}  // namespace veraa
