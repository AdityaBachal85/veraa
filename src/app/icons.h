// Toolbar and panel icons drawn in code (no image assets needed).
#pragma once

#include <QIcon>

namespace veraa::icons {

QIcon app();
QIcon undo();
QIcon redo();
QIcon select();
QIcon move();
QIcon rotate();
QIcon scale();
QIcon axis(const QString& label);
QIcon materialEditor();
QIcon renderSetup();
QIcon render();
QIcon newFile();
QIcon open();
QIcon save();
QIcon import();
QIcon geometry();
QIcon lights();
QIcon cameras();
QIcon create();
QIcon modify();
QIcon trash();
QIcon arrowUp();
QIcon arrowDown();
QIcon light();
QIcon camera();
QIcon object();
QIcon eye(bool open);
QIcon stop();
QIcon zoomExtents();
QIcon maximize();

}  // namespace veraa::icons
