// Rendering module UI glue: menus, creation tools, viewport gizmos and helpers shared with
// the rendering self tests.
#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

namespace veraa {

class BatchRenderDialog;
class EnvironmentDialog;
class MainWindow;
class Node;
class PrintSizeDialog;
class SceneDocument;
class SlateEditor;

// Writes the selected geometry (combined, in the first node's space) to a .vxp proxy file and
// creates a VeraaProxy node in its place. With replace set the source nodes are deleted.
// One undo step. Returns the proxy node id, or 0 (error set) on failure.
uint32_t exportProxy(SceneDocument* doc, const QVector<uint32_t>& nodeIds, const QString& path, bool replace,
                     QString* error = nullptr);

// Creates a VeraaFur node growing on `source` (inside an open edit).
Node* createFurOn(SceneDocument* doc, const Node* source);

// The rendering module's windows (created on first use).
EnvironmentDialog* environmentDialog(MainWindow& w);
BatchRenderDialog* batchRenderDialog(MainWindow& w);
PrintSizeDialog* printSizeDialog(MainWindow& w);
SlateEditor* slateEditor(MainWindow& w);

// Path of a bundled asset (e.g. "ies/downlight_wide.ies") next to the executable or in a
// parent folder; empty when not found.
QString findBundledAsset(const QString& relative);

}  // namespace veraa
