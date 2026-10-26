// Create panel and Create menu layout in 3ds Max 2022 order (see docs/UI_PARITY.md): the
// subcategory lists of each Create category and the object buttons of each list. Registered
// object types the table does not mention are appended, so new types always show up; entries
// Veraa does not have yet are kept as "not available" buttons or lists.
#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace veraa {

struct CreateButton {
  QString label;  // button text
  QString type;   // object type it creates; empty = not available yet
};

struct CreateList {
  QString name;                       // entry of the subcategory list
  std::vector<CreateButton> buttons;  // panel order (two columns, row by row)
  bool available() const;             // at least one button creates something
};

// Create panel categories in tab order: Geometry, Shapes, Lights, Cameras, Helpers, Space Warps,
// Systems.
const QStringList& createCategories();
// The lists of a category ("Geometry", ...).
std::vector<CreateList> createLists(const QString& category);

}  // namespace veraa
