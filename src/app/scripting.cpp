// Python must be included before any standard or Qt header.
#ifdef VERAA_WITH_PYTHON
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif

#include "scripting.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include "mainwindow.h"
#include "modifier.h"
#include "project.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

QString pyString(const QString& s) {
  QString out = "\"";
  for (QChar c : s) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

namespace {

QString pyLiteral(const json& j) {
  if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
  if (j.is_number_integer()) return QString::number(j.get<long long>());
  if (j.is_number()) return QString::number(j.get<double>(), 'g', 7);
  if (j.is_string()) return pyString(QString::fromStdString(j.get<std::string>()));
  if (j.is_array()) {
    QStringList parts;
    for (const auto& v : j) parts << pyLiteral(v);
    return "(" + parts.join(", ") + (j.size() == 1 ? ",)" : ")");
  }
  return "None";
}

QString vecLiteral(const Vec3& v) {
  return QString("(%1, %2, %3)").arg(v.x, 0, 'g', 7).arg(v.y, 0, 'g', 7).arg(v.z, 0, 'g', 7);
}

SceneDocument* doc() {
  MainWindow* w = ScriptEngine::instance().window();
  return w ? w->document() : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Python bindings

#ifdef VERAA_WITH_PYTHON
namespace {

PyObject* vecToPy(const Vec3& v) { return Py_BuildValue("(ddd)", double(v.x), double(v.y), double(v.z)); }

bool pyToVec(PyObject* o, Vec3& v) {
  double x, y, z;
  PyObject* seq = PySequence_Fast(o, "expected a sequence of three numbers");
  if (!seq) return false;
  bool ok = PySequence_Fast_GET_SIZE(seq) == 3;
  if (ok) {
    x = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, 0));
    y = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, 1));
    z = PyFloat_AsDouble(PySequence_Fast_GET_ITEM(seq, 2));
    ok = !PyErr_Occurred();
    if (ok) v = Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
  } else {
    PyErr_SetString(PyExc_ValueError, "expected a sequence of three numbers");
  }
  Py_DECREF(seq);
  return ok;
}

Node* nodeArg(unsigned int id) {
  SceneDocument* d = doc();
  Node* n = d ? d->scene().findNode(id) : nullptr;
  if (!n) PyErr_Format(PyExc_ValueError, "no object with id %u", id);
  return n;
}

PyObject* paramToPy(const ParamBlock& p, const std::string& key) {
  const ParamDef* d = p.def(key);
  if (!d) {
    PyErr_Format(PyExc_KeyError, "no parameter '%s'", key.c_str());
    return nullptr;
  }
  switch (d->type) {
    case ParamType::Float: return PyFloat_FromDouble(p.getFloat(key));
    case ParamType::Int:
    case ParamType::Enum: return PyLong_FromLong(p.getInt(key));
    case ParamType::Bool: return PyBool_FromLong(p.getBool(key));
    case ParamType::Color: return vecToPy(p.getColor(key));
    case ParamType::String:
    case ParamType::File: return PyUnicode_FromString(p.getString(key).c_str());
  }
  Py_RETURN_NONE;
}

bool pyToParam(ParamBlock& p, const std::string& key, PyObject* v) {
  const ParamDef* d = p.def(key);
  if (!d) {
    PyErr_Format(PyExc_KeyError, "no parameter '%s'", key.c_str());
    return false;
  }
  switch (d->type) {
    case ParamType::Color: {
      Vec3 c;
      if (!pyToVec(v, c)) return false;
      p.setColor(key, c);
      return true;
    }
    case ParamType::String:
    case ParamType::File: {
      const char* s = PyUnicode_AsUTF8(v);
      if (!s) return false;
      p.setString(key, s);
      return true;
    }
    case ParamType::Enum:
      if (PyUnicode_Check(v)) {
        std::string s = PyUnicode_AsUTF8(v);
        for (size_t i = 0; i < d->items.size(); ++i)
          if (d->items[i] == s) {
            p.setNumber(key, static_cast<double>(i));
            return true;
          }
        PyErr_Format(PyExc_ValueError, "'%s' is not a choice of %s", s.c_str(), key.c_str());
        return false;
      }
      [[fallthrough]];
    default: {
      double x = PyFloat_AsDouble(v);
      if (PyErr_Occurred()) return false;
      p.setNumber(key, x);
      return true;
    }
  }
}

PyObject* py_write(PyObject*, PyObject* args) {
  const char* s;
  if (!PyArg_ParseTuple(args, "s", &s)) return nullptr;
  ScriptEngine::instance().write(QString::fromUtf8(s));
  Py_RETURN_NONE;
}

PyObject* py_create(PyObject*, PyObject* args) {
  const char* type;
  const char* name = "";
  if (!PyArg_ParseTuple(args, "s|s", &type, &name)) return nullptr;
  SceneDocument* d = doc();
  if (!d) return nullptr;
  if (!findObjectType(type)) {
    PyErr_Format(PyExc_ValueError, "unknown object type '%s' (see veraa.object_types())", type);
    return nullptr;
  }
  d->beginEdit(QObject::tr("Create %1").arg(QString::fromUtf8(type)));
  Node* n = d->createNode(QString::fromUtf8(type));
  if (!n) {
    d->cancelEdit();
    PyErr_Format(PyExc_RuntimeError, "could not create '%s'", type);
    return nullptr;
  }
  if (*name) n->name = d->scene().findNodeByName(name) ? d->scene().uniqueNodeName(name) : std::string(name);
  d->notifyStructureChanged();
  d->endEdit();
  return PyLong_FromUnsignedLong(n->id);
}

PyObject* py_delete(PyObject*, PyObject* args) {
  unsigned int id;
  if (!PyArg_ParseTuple(args, "I", &id)) return nullptr;
  if (!nodeArg(id)) return nullptr;
  SceneDocument* d = doc();
  d->beginEdit(QObject::tr("Delete"));
  d->deselect(id);
  d->scene().removeNode(id);
  d->notifyStructureChanged();
  d->endEdit();
  Py_RETURN_NONE;
}

PyObject* py_exists(PyObject*, PyObject* args) {
  unsigned int id;
  if (!PyArg_ParseTuple(args, "I", &id)) return nullptr;
  SceneDocument* d = doc();
  return PyBool_FromLong(d && d->scene().findNode(id));
}

PyObject* py_nodes(PyObject*, PyObject*) {
  SceneDocument* d = doc();
  PyObject* list = PyList_New(0);
  if (d)
    for (const auto& n : d->scene().nodes) {
      PyObject* v = PyLong_FromUnsignedLong(n->id);
      PyList_Append(list, v);
      Py_DECREF(v);
    }
  return list;
}

PyObject* py_find(PyObject*, PyObject* args) {
  const char* name;
  if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
  SceneDocument* d = doc();
  Node* n = d ? d->scene().findNodeByName(name) : nullptr;
  if (!n) Py_RETURN_NONE;
  return PyLong_FromUnsignedLong(n->id);
}

PyObject* py_selection(PyObject*, PyObject*) {
  SceneDocument* d = doc();
  PyObject* list = PyList_New(0);
  if (d)
    for (uint32_t id : d->selection()) {
      PyObject* v = PyLong_FromUnsignedLong(id);
      PyList_Append(list, v);
      Py_DECREF(v);
    }
  return list;
}

PyObject* py_select(PyObject*, PyObject* args) {
  PyObject* seq;
  int add = 0;
  if (!PyArg_ParseTuple(args, "O|p", &seq, &add)) return nullptr;
  SceneDocument* d = doc();
  PyObject* fast = PySequence_Fast(seq, "expected a list of ids");
  if (!fast || !d) return nullptr;
  QVector<uint32_t> ids = add ? d->selection() : QVector<uint32_t>();
  for (Py_ssize_t i = 0; i < PySequence_Fast_GET_SIZE(fast); ++i) {
    unsigned long id = PyLong_AsUnsignedLong(PySequence_Fast_GET_ITEM(fast, i));
    if (PyErr_Occurred()) {
      Py_DECREF(fast);
      return nullptr;
    }
    if (d->scene().findNode(static_cast<uint32_t>(id)) && !ids.contains(static_cast<uint32_t>(id))) ids.push_back(static_cast<uint32_t>(id));
  }
  Py_DECREF(fast);
  d->setSelection(ids);
  Py_RETURN_NONE;
}

PyObject* py_get(PyObject*, PyObject* args) {
  unsigned int id;
  const char* what;
  if (!PyArg_ParseTuple(args, "Is", &id, &what)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  std::string w = what;
  if (w == "name") return PyUnicode_FromString(n->name.c_str());
  if (w == "type") return PyUnicode_FromString(n->object ? n->object->type().c_str() : "");
  if (w == "position") return vecToPy(n->worldPosition());
  if (w == "local_position") return vecToPy(n->transform.position);
  if (w == "rotation") return vecToPy(n->transform.rotation);
  if (w == "scale") return vecToPy(n->transform.scale);
  if (w == "wirecolor") return vecToPy(n->wireColor);
  if (w == "parent") return PyLong_FromUnsignedLong(n->parentId);
  if (w == "hidden") return PyBool_FromLong(n->hidden);
  if (w == "frozen") return PyBool_FromLong(n->frozen);
  if (w == "renderable") return PyBool_FromLong(n->renderable);
  if (w == "layer") {
    const Layer* l = doc()->scene().findLayer(n->layerId);
    return PyUnicode_FromString(l ? l->name.c_str() : "0");
  }
  if (w == "material") {
    const Material* m = doc()->scene().findMaterial(n->materialId);
    if (!m) Py_RETURN_NONE;
    return PyUnicode_FromString(m->name.c_str());
  }
  if (w == "children") {
    PyObject* list = PyList_New(0);
    for (Node* c : doc()->scene().children(n->id)) {
      PyObject* v = PyLong_FromUnsignedLong(c->id);
      PyList_Append(list, v);
      Py_DECREF(v);
    }
    return list;
  }
  if (w == "bounds") {
    Aabb b = n->worldBounds();
    return Py_BuildValue("(NN)", vecToPy(b.lo), vecToPy(b.hi));
  }
  if (w == "vertex_count") return PyLong_FromSize_t(n->mesh().positions.size());
  if (w == "face_count") return PyLong_FromSize_t(n->mesh().faces.size());
  PyErr_Format(PyExc_KeyError, "unknown property '%s'", what);
  return nullptr;
}

PyObject* py_set(PyObject*, PyObject* args) {
  unsigned int id;
  const char* what;
  PyObject* value;
  if (!PyArg_ParseTuple(args, "IsO", &id, &what, &value)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  SceneDocument* d = doc();
  std::string w = what;
  Vec3 v;
  int flags = ChangeTransform;
  d->beginEdit(QObject::tr("Script: %1").arg(QString::fromUtf8(what)));
  bool ok = true;
  if (w == "name") {
    const char* s = PyUnicode_AsUTF8(value);
    ok = s != nullptr;
    if (ok) n->name = s;
    flags = ChangeName;
  } else if (w == "position" || w == "local_position" || w == "rotation" || w == "scale" || w == "wirecolor") {
    ok = pyToVec(value, v);
    if (ok) {
      if (w == "position") n->setWorldPosition(v);
      else if (w == "local_position") n->transform.position = v;
      else if (w == "rotation") n->transform.rotation = v;
      else if (w == "scale") n->transform.scale = v;
      else {
        n->wireColor = v;
        flags = ChangeDisplay;
      }
    }
  } else if (w == "parent") {
    unsigned long p = PyLong_AsUnsignedLong(value);
    ok = !PyErr_Occurred() && d->scene().setParent(n->id, static_cast<uint32_t>(p));
    if (!ok && !PyErr_Occurred()) PyErr_SetString(PyExc_ValueError, "cannot link: the parent is missing or a descendant");
    d->notifyStructureChanged();
  } else if (w == "hidden" || w == "frozen" || w == "renderable") {
    int b = PyObject_IsTrue(value);
    ok = b >= 0;
    if (ok) (w == "hidden" ? n->hidden : w == "frozen" ? n->frozen : n->renderable) = b != 0;
    flags = ChangeDisplay;
  } else if (w == "layer") {
    const char* s = PyUnicode_AsUTF8(value);
    ok = s != nullptr;
    if (ok) {
      Layer* found = nullptr;
      for (Layer& l : d->scene().layers)
        if (l.name == s) found = &l;
      if (!found) found = d->scene().createLayer(s);
      n->layerId = found->id;
      flags = ChangeDisplay;
      d->notifyStructureChanged();
    }
  } else if (w == "material") {
    const char* s = PyUnicode_AsUTF8(value);
    ok = s != nullptr;
    if (ok) {
      uint32_t mid = 0;
      for (const auto& m : d->scene().materials)
        if (m->name == s) mid = m->id;
      if (!mid) {
        Material* m = d->scene().createMaterial(s);
        mid = m->id;
        d->notifyMaterialsChanged();
      }
      n->materialId = mid;
      flags = ChangeMaterial;
    }
  } else {
    PyErr_Format(PyExc_KeyError, "unknown property '%s'", what);
    ok = false;
  }
  if (!ok) {
    d->cancelEdit();
    return nullptr;
  }
  d->notifyNodeChanged(n, flags);
  d->endEdit();
  Py_RETURN_NONE;
}

PyObject* py_get_param(PyObject*, PyObject* args) {
  unsigned int id;
  const char* key;
  if (!PyArg_ParseTuple(args, "Is", &id, &key)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  return paramToPy(n->object->params, key);
}

PyObject* py_set_param(PyObject*, PyObject* args) {
  unsigned int id;
  const char* key;
  PyObject* value;
  if (!PyArg_ParseTuple(args, "IsO", &id, &key, &value)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  SceneDocument* d = doc();
  d->beginEdit(QObject::tr("Script: %1").arg(QString::fromUtf8(key)));
  if (!pyToParam(n->object->params, key, value)) {
    d->cancelEdit();
    return nullptr;
  }
  n->object->touch();
  n->invalidate();
  d->notifyNodeChanged(n, ChangeGeometry);
  d->endEdit();
  Py_RETURN_NONE;
}

PyObject* py_params(PyObject*, PyObject* args) {
  unsigned int id;
  int mod = -1;
  if (!PyArg_ParseTuple(args, "I|i", &id, &mod)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  const ParamBlock* p = &n->object->params;
  if (mod >= 0) {
    if (mod >= static_cast<int>(n->modifiers.size())) {
      PyErr_SetString(PyExc_IndexError, "modifier index out of range");
      return nullptr;
    }
    p = &n->modifiers[mod]->params;
  }
  PyObject* list = PyList_New(0);
  for (const ParamDef& def : p->defs()) {
    PyObject* t = Py_BuildValue("(ss)", def.key.c_str(), def.label.c_str());
    PyList_Append(list, t);
    Py_DECREF(t);
  }
  return list;
}

PyObject* py_add_modifier(PyObject*, PyObject* args) {
  unsigned int id;
  const char* type;
  if (!PyArg_ParseTuple(args, "Is", &id, &type)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  auto mod = createModifier(type);
  if (!mod) {
    PyErr_Format(PyExc_ValueError, "unknown modifier '%s' (see veraa.modifier_types())", type);
    return nullptr;
  }
  SceneDocument* d = doc();
  d->beginEdit(QObject::tr("Add Modifier"));
  n->modifiers.push_back(std::move(mod));
  n->invalidate();
  d->notifyNodeChanged(n, ChangeGeometry);
  d->notifyStructureChanged();
  d->endEdit();
  return PyLong_FromSize_t(n->modifiers.size() - 1);
}

PyObject* py_modifiers(PyObject*, PyObject* args) {
  unsigned int id;
  if (!PyArg_ParseTuple(args, "I", &id)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  PyObject* list = PyList_New(0);
  for (const auto& m : n->modifiers) {
    PyObject* s = PyUnicode_FromString(m ? m->type().c_str() : "");
    PyList_Append(list, s);
    Py_DECREF(s);
  }
  return list;
}

PyObject* py_mod_param(PyObject*, PyObject* args) {
  unsigned int id;
  int index;
  const char* key;
  PyObject* value = nullptr;
  if (!PyArg_ParseTuple(args, "Iis|O", &id, &index, &key, &value)) return nullptr;
  Node* n = nodeArg(id);
  if (!n) return nullptr;
  if (index < 0 || index >= static_cast<int>(n->modifiers.size()) || !n->modifiers[index]) {
    PyErr_SetString(PyExc_IndexError, "modifier index out of range");
    return nullptr;
  }
  ParamBlock& p = n->modifiers[index]->params;
  if (!value) return paramToPy(p, key);
  SceneDocument* d = doc();
  d->beginEdit(QObject::tr("Script: modifier %1").arg(QString::fromUtf8(key)));
  if (!pyToParam(p, key, value)) {
    d->cancelEdit();
    return nullptr;
  }
  n->modifiers[index]->touch();
  n->invalidate();
  d->notifyNodeChanged(n, ChangeGeometry);
  d->endEdit();
  Py_RETURN_NONE;
}

PyObject* py_object_types(PyObject*, PyObject*) {
  PyObject* list = PyList_New(0);
  for (const ObjectTypeInfo& t : objectTypes()) {
    PyObject* s = PyUnicode_FromString(t.type.c_str());
    PyList_Append(list, s);
    Py_DECREF(s);
  }
  return list;
}

PyObject* py_modifier_types(PyObject*, PyObject*) {
  PyObject* list = PyList_New(0);
  for (const ModifierTypeInfo& t : modifierTypeInfos()) {
    PyObject* s = PyUnicode_FromString(t.type.c_str());
    PyList_Append(list, s);
    Py_DECREF(s);
  }
  return list;
}

PyObject* py_command(PyObject*, PyObject* args) {
  const char* id;
  if (!PyArg_ParseTuple(args, "s", &id)) return nullptr;
  MainWindow* w = ScriptEngine::instance().window();
  QAction* a = w ? w->command(QString::fromUtf8(id)) : nullptr;
  if (!a) {
    PyErr_Format(PyExc_ValueError, "unknown command '%s' (see veraa.commands())", id);
    return nullptr;
  }
  if (!a->isEnabled()) Py_RETURN_FALSE;
  a->trigger();
  Py_RETURN_TRUE;
}

PyObject* py_commands(PyObject*, PyObject*) {
  MainWindow* w = ScriptEngine::instance().window();
  PyObject* list = PyList_New(0);
  if (w) {
    QStringList ids = w->commands().keys();
    ids.sort();
    for (const QString& id : ids) {
      PyObject* s = PyUnicode_FromString(id.toUtf8().constData());
      PyList_Append(list, s);
      Py_DECREF(s);
    }
  }
  return list;
}

PyObject* py_undo(PyObject*, PyObject*) {
  if (SceneDocument* d = doc()) {
    d->commitSoftEdit();
    d->undoStack()->undo();
  }
  Py_RETURN_NONE;
}

PyObject* py_redo(PyObject*, PyObject*) {
  if (SceneDocument* d = doc()) d->undoStack()->redo();
  Py_RETURN_NONE;
}

PyObject* py_new_scene(PyObject*, PyObject*) {
  if (SceneDocument* d = doc()) d->newScene();
  Py_RETURN_NONE;
}

PyObject* py_open(PyObject*, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
  MainWindow* w = ScriptEngine::instance().window();
  return PyBool_FromLong(w && w->openFile(QString::fromUtf8(path)));
}

PyObject* py_save(PyObject*, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
  QString err;
  SceneDocument* d = doc();
  if (!d || !d->save(QString::fromUtf8(path), &err)) {
    PyErr_SetString(PyExc_IOError, err.toUtf8().constData());
    return nullptr;
  }
  Py_RETURN_TRUE;
}

PyObject* py_import(PyObject*, PyObject* args) {
  const char* path;
  if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
  MainWindow* w = ScriptEngine::instance().window();
  QString err;
  QVector<uint32_t> ids;
  if (!w || !w->importPath(QString::fromUtf8(path), &ids, &err)) {
    PyErr_SetString(PyExc_IOError, err.toUtf8().constData());
    return nullptr;
  }
  PyObject* list = PyList_New(0);
  for (uint32_t id : ids) {
    PyObject* v = PyLong_FromUnsignedLong(id);
    PyList_Append(list, v);
    Py_DECREF(v);
  }
  return list;
}

PyObject* py_export(PyObject*, PyObject* args) {
  const char* path;
  int selected = 0;
  if (!PyArg_ParseTuple(args, "s|p", &path, &selected)) return nullptr;
  MainWindow* w = ScriptEngine::instance().window();
  QString err;
  if (!w || !w->exportPath(QString::fromUtf8(path), selected != 0, &err)) {
    PyErr_SetString(PyExc_IOError, err.toUtf8().constData());
    return nullptr;
  }
  Py_RETURN_TRUE;
}

PyObject* py_redraw(PyObject*, PyObject*) {
  if (MainWindow* w = ScriptEngine::instance().window()) w->viewports()->updateAll();
  QApplication::processEvents();
  Py_RETURN_NONE;
}

PyObject* py_prompt(PyObject*, PyObject* args) {
  const char* s;
  if (!PyArg_ParseTuple(args, "s", &s)) return nullptr;
  if (MainWindow* w = ScriptEngine::instance().window()) w->setPrompt(QString::fromUtf8(s));
  Py_RETURN_NONE;
}

PyObject* py_units(PyObject*, PyObject*) {
  return PyUnicode_FromString(Units::unitName(Units::instance().display()).toUtf8().constData());
}

PyMethodDef kMethods[] = {
    {"write", py_write, METH_VARARGS, "Write text to the listener."},
    {"create", py_create, METH_VARARGS, "create(type, name='') -> id"},
    {"delete", py_delete, METH_VARARGS, "delete(id)"},
    {"exists", py_exists, METH_VARARGS, "exists(id) -> bool"},
    {"nodes", py_nodes, METH_NOARGS, "nodes() -> [id]"},
    {"find", py_find, METH_VARARGS, "find(name) -> id or None"},
    {"selection", py_selection, METH_NOARGS, "selection() -> [id]"},
    {"select", py_select, METH_VARARGS, "select([id], add=False)"},
    {"get", py_get, METH_VARARGS, "get(id, property)"},
    {"set", py_set, METH_VARARGS, "set(id, property, value)"},
    {"get_param", py_get_param, METH_VARARGS, "get_param(id, key)"},
    {"set_param", py_set_param, METH_VARARGS, "set_param(id, key, value)"},
    {"params", py_params, METH_VARARGS, "params(id, modifier=-1) -> [(key, label)]"},
    {"add_modifier", py_add_modifier, METH_VARARGS, "add_modifier(id, type) -> index"},
    {"modifiers", py_modifiers, METH_VARARGS, "modifiers(id) -> [type]"},
    {"mod_param", py_mod_param, METH_VARARGS, "mod_param(id, index, key[, value])"},
    {"object_types", py_object_types, METH_NOARGS, "object_types() -> [type]"},
    {"modifier_types", py_modifier_types, METH_NOARGS, "modifier_types() -> [type]"},
    {"command", py_command, METH_VARARGS, "command(id) -> bool: runs a menu/toolbar command"},
    {"commands", py_commands, METH_NOARGS, "commands() -> [id]"},
    {"undo", py_undo, METH_NOARGS, "undo()"},
    {"redo", py_redo, METH_NOARGS, "redo()"},
    {"new_scene", py_new_scene, METH_NOARGS, "new_scene()"},
    {"open", py_open, METH_VARARGS, "open(path) -> bool"},
    {"save", py_save, METH_VARARGS, "save(path)"},
    {"import_file", py_import, METH_VARARGS, "import_file(path) -> [id]"},
    {"export_file", py_export, METH_VARARGS, "export_file(path, selected=False)"},
    {"redraw", py_redraw, METH_NOARGS, "redraw()"},
    {"prompt", py_prompt, METH_VARARGS, "prompt(text): show text in the status bar"},
    {"units", py_units, METH_NOARGS, "units() -> display unit name"},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef kModule = {PyModuleDef_HEAD_INIT, "_veraa", "Veraa scene access (low level).", -1, kMethods,
                       nullptr, nullptr, nullptr, nullptr};

PyObject* initVeraaModule() { return PyModule_Create(&kModule); }

const char* kPrelude = R"PY(
"""Veraa scripting.

    import veraa
    b = veraa.create("Box", width=50, length=50, height=20, position=(0, 0, 0))
    b.position = (100, 0, 0); b.rotation = (0, 0, 45); b.name = "Crate"
    b["height"] = 35                  # object parameters (see b.params())
    b.add_modifier("Bend", angle=45)  # modifier parameters by keyword
    for n in veraa.nodes(): print(n.name, n.type)
    veraa.select([b]); veraa.command("views.zoomExtentsSelected")

Functions: create, nodes, node, selection, select, clear_selection, delete, object_types,
modifier_types, command, commands, undo, redo, new_scene, open_scene, save_scene, import_file,
export_file, redraw, prompt, units.
"""
import sys
import _veraa

class _Out:
    def write(self, s):
        _veraa.write(str(s))
        return len(s)
    def flush(self):
        pass

sys.stdout = _Out()
sys.stderr = _Out()

class Node:
    """A scene object. Properties: name, type, position (world), local_position, rotation
    (Euler XYZ degrees), scale, wirecolor, parent, children, hidden, frozen, renderable, layer,
    material, exists. Parameters: node["radius"] = 10."""
    __slots__ = ("id",)
    def __init__(self, id):
        object.__setattr__(self, "id", int(id))
    def __repr__(self):
        if not _veraa.exists(self.id):
            return "<Node %d (deleted)>" % self.id
        return "<Node %d %r %s>" % (self.id, self.name, self.type)
    def __eq__(self, other):
        return isinstance(other, Node) and other.id == self.id
    def __hash__(self):
        return hash(self.id)
    def _prop(name):
        return property(lambda s: _veraa.get(s.id, name), lambda s, v: _veraa.set(s.id, name, v))
    name = _prop("name")
    position = _prop("position")
    local_position = _prop("local_position")
    rotation = _prop("rotation")
    scale = _prop("scale")
    wirecolor = _prop("wirecolor")
    hidden = _prop("hidden")
    frozen = _prop("frozen")
    renderable = _prop("renderable")
    layer = _prop("layer")
    material = _prop("material")
    del _prop
    type = property(lambda s: _veraa.get(s.id, "type"))
    exists = property(lambda s: _veraa.exists(s.id))
    bounds = property(lambda s: _veraa.get(s.id, "bounds"))
    vertex_count = property(lambda s: _veraa.get(s.id, "vertex_count"))
    face_count = property(lambda s: _veraa.get(s.id, "face_count"))
    @property
    def parent(self):
        p = _veraa.get(self.id, "parent")
        return Node(p) if p else None
    @parent.setter
    def parent(self, value):
        _veraa.set(self.id, "parent", value.id if value else 0)
    @property
    def children(self):
        return [Node(i) for i in _veraa.get(self.id, "children")]
    def __getitem__(self, key):
        return _veraa.get_param(self.id, key)
    def __setitem__(self, key, value):
        _veraa.set_param(self.id, key, value)
    def params(self):
        return _veraa.params(self.id)
    def add_modifier(self, type, **params):
        i = _veraa.add_modifier(self.id, type)
        for k, v in params.items():
            _veraa.mod_param(self.id, i, k, v)
        return i
    def modifiers(self):
        return _veraa.modifiers(self.id)
    def modifier_param(self, index, key, *value):
        return _veraa.mod_param(self.id, index, key, *value)
    def select(self, add=False):
        _veraa.select([self.id], add)
    def delete(self):
        _veraa.delete(self.id)

def _ids(items):
    if isinstance(items, (Node, int)):
        items = [items]
    return [n.id if isinstance(n, Node) else int(n) for n in items]

def create(type, name=None, position=None, rotation=None, scale=None, **params):
    """Creates an object: create("Sphere", radius=25, position=(0, 0, 25))."""
    n = Node(_veraa.create(type, name or ""))
    for k, v in params.items():
        n[k] = v
    if rotation is not None:
        n.rotation = rotation
    if scale is not None:
        n.scale = scale
    if position is not None:
        n.position = position
    return n

def nodes(type=None):
    return [Node(i) for i in _veraa.nodes() if type is None or _veraa.get(i, "type") == type]

def node(name):
    i = _veraa.find(name)
    return Node(i) if i else None

def selection():
    return [Node(i) for i in _veraa.selection()]

def select(items, add=False):
    _veraa.select(_ids(items), add)

def clear_selection():
    _veraa.select([], False)

def delete(items):
    for i in _ids(items):
        if _veraa.exists(i):
            _veraa.delete(i)

def import_file(path):
    return [Node(i) for i in _veraa.import_file(path)]

object_types = _veraa.object_types
modifier_types = _veraa.modifier_types
command = _veraa.command
commands = _veraa.commands
undo = _veraa.undo
redo = _veraa.redo
new_scene = _veraa.new_scene
open_scene = _veraa.open
save_scene = _veraa.save
export_file = _veraa.export_file
redraw = _veraa.redraw
prompt = _veraa.prompt
units = _veraa.units

def _run(src, interactive, filename):
    import traceback, __main__
    g = __main__.__dict__
    try:
        if interactive:
            try:
                code = compile(src, filename, "single")
            except SyntaxError:
                code = compile(src, filename, "exec")
        else:
            code = compile(src, filename, "exec")
        exec(code, g)
        return True
    except SystemExit:
        return True
    except BaseException:
        traceback.print_exc()
        return False
)PY";

}  // namespace
#endif  // VERAA_WITH_PYTHON

// ---------------------------------------------------------------------------------------------
// Engine

ScriptEngine& ScriptEngine::instance() {
  static ScriptEngine* e = new ScriptEngine();
  return *e;
}

void ScriptEngine::attach(MainWindow* w) {
  window_ = w;
  SceneDocument* d = w->document();
  // Macro recorder: every completed edit is compared with the previous scene state.
  connect(d->undoStack(), &QUndoStack::indexChanged, this, [this] {
    if (!recording_ || running_) return;
    recordChanges();
    snapshot();
    // Commands that edited the scene were recorded as their changes; skip their command line.
    window_->setProperty("veraaMacroDiff", true);
    QTimer::singleShot(0, this, [this] { window_->setProperty("veraaMacroDiff", false); });
  });
  for (auto it = w->commands().begin(); it != w->commands().end(); ++it) {
    QString id = it.key();
    if (id.startsWith("edit.undo") || id.startsWith("edit.redo") || id.startsWith("scripting.")) continue;
    connect(it.value(), &QAction::triggered, this, [this, id] {
      if (!recording_ || running_ || window_->property("veraaMacroDiff").toBool()) return;
      record(QString("veraa.command(%1)").arg(pyString(id)));
    });
  }
}

bool ScriptEngine::available() const {
#ifdef VERAA_WITH_PYTHON
  return !failed_;
#else
  return false;
#endif
}

QString ScriptEngine::unavailableReason() const {
#ifdef VERAA_WITH_PYTHON
  return error_;
#else
  return tr("This build of Veraa was made without Python (install Python development files and rebuild).");
#endif
}

QString ScriptEngine::version() const {
#ifdef VERAA_WITH_PYTHON
  return QString::fromUtf8(Py_GetVersion()).section(' ', 0, 0);
#else
  return QString();
#endif
}

bool ScriptEngine::ensureInitialized() {
#ifdef VERAA_WITH_PYTHON
  if (initialized_) return true;
  if (failed_) return false;
  PyImport_AppendInittab("_veraa", &initVeraaModule);
  PyConfig config;
  // Isolated: environment variables (PYTHONHOME, PYTHONPATH) of other Python installs are ignored.
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 0;
  config.parse_argv = 0;
  config.user_site_directory = 0;
  // A packaged Windows build carries the embeddable standard library next to the executable.
  QDir appDir(QApplication::applicationDirPath());
  QString zip = appDir.filePath(QString("python%1%2.zip").arg(PY_MAJOR_VERSION).arg(PY_MINOR_VERSION));
  if (QFileInfo::exists(zip)) {
    std::wstring home = QDir::toNativeSeparators(appDir.absolutePath()).toStdWString();
    PyConfig_SetString(&config, &config.home, home.c_str());
  }
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) {
    failed_ = true;
    error_ = tr("Python could not start: %1").arg(QString::fromUtf8(status.err_msg ? status.err_msg : "unknown error"));
    return false;
  }
  PyObject* mod = PyImport_AddModule("veraa");  // borrowed
  PyObject* dict = PyModule_GetDict(mod);
  PyDict_SetItemString(dict, "__builtins__", PyEval_GetBuiltins());
  PyObject* r = PyRun_String(kPrelude, Py_file_input, dict, dict);
  if (!r) {
    PyErr_Print();
    failed_ = true;
    error_ = tr("The veraa Python module failed to load");
    return false;
  }
  Py_DECREF(r);
  PyRun_SimpleString("import veraa\n");
  initialized_ = true;
  return true;
#else
  return false;
#endif
}

void ScriptEngine::write(const QString& text) {
  if (sink_) *sink_ += text;
  Q_EMIT outputWritten(text);
}

bool ScriptEngine::run(const QString& code, QString* output, bool interactive) {
#ifdef VERAA_WITH_PYTHON
  if (!ensureInitialized()) {
    QString msg = unavailableReason() + "\n";
    if (output) *output += msg;
    Q_EMIT outputWritten(msg);
    return false;
  }
  QString local;
  QString* prevSink = sink_;
  sink_ = output ? output : &local;
  bool prevRunning = running_;
  running_ = true;
  bool ok = false;
  PyObject* mod = PyImport_AddModule("veraa");
  PyObject* fn = PyObject_GetAttrString(mod, "_run");
  if (fn) {
    PyObject* res = PyObject_CallFunction(fn, "sis", code.toUtf8().constData(), interactive ? 1 : 0,
                                          interactive ? "<listener>" : "<script>");
    if (res) {
      ok = PyObject_IsTrue(res) == 1;
      Py_DECREF(res);
    } else {
      PyErr_Print();
    }
    Py_DECREF(fn);
  }
  running_ = prevRunning;
  sink_ = prevSink;
  if (recording_ && !running_) snapshot();  // script edits are not recorded as user actions
  return ok;
#else
  QString msg = unavailableReason() + "\n";
  if (output) *output += msg;
  Q_EMIT outputWritten(msg);
  Q_UNUSED(code);
  Q_UNUSED(interactive);
  return false;
#endif
}

bool ScriptEngine::runFile(const QString& path, QString* output) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QString msg = tr("Cannot open %1\n").arg(path);
    if (output) *output += msg;
    Q_EMIT outputWritten(msg);
    return false;
  }
  QString code = QString::fromUtf8(f.readAll());
  // Make __file__ available like a normal script run.
  QString prefix = QString("__file__ = %1\n").arg(pyString(QFileInfo(path).absoluteFilePath()));
  return run(prefix + code, output, false);
}

void ScriptEngine::runStartupScripts() {
  QStringList disabled = QSettings("Veraa", "Veraa").value("plugins/disabledScripts").toStringList();
  for (const QString& dirPath : {userPath("startupScripts"), userPath("plugins")}) {
    QDir dir(dirPath);
    for (const QString& f : dir.entryList({"*.py"}, QDir::Files, QDir::Name)) {
      if (disabled.contains(f)) continue;
      QString out;
      if (!runFile(dir.filePath(f), &out)) qWarning("Startup script %s failed:\n%s", qPrintable(f), qPrintable(out));
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Macro recorder

void ScriptEngine::setRecording(bool on) {
  if (on == recording_) return;
  recording_ = on;
  if (on) snapshot();
  Q_EMIT recordingChanged(on);
}

void ScriptEngine::record(const QString& line) {
  recorded_ << line;
  Q_EMIT recordedLine(line);
}

void ScriptEngine::snapshot() {
  before_.clear();
  if (!window_) return;
  for (const auto& n : window_->document()->scene().nodes) {
    NodeState s;
    s.type = n->object ? n->object->type() : std::string();
    s.name = n->name;
    s.xf = n->transform;
    s.params = n->object ? n->object->params.toJson() : json();
    s.parent = n->parentId;
    s.modifiers = static_cast<int>(n->modifiers.size());
    before_.insert(n->id, s);
  }
}

void ScriptEngine::recordChanges() {
  if (!window_) return;
  const Scene& scene = window_->document()->scene();
  auto ref = [](const std::string& name) { return QString("veraa.node(%1)").arg(pyString(QString::fromStdString(name))); };
  QSet<uint32_t> now;
  for (const auto& n : scene.nodes) {
    now.insert(n->id);
    auto it = before_.find(n->id);
    if (it == before_.end()) {
      // Created.
      QStringList args;
      args << pyString(QString::fromStdString(n->object ? n->object->type() : std::string()));
      args << QString("name=%1").arg(pyString(QString::fromStdString(n->name)));
      if (n->object) {
        const json p = n->object->params.toJson();
        for (auto pi = p.begin(); pi != p.end(); ++pi)
          if (pi->is_number() || pi->is_boolean()) args << QString("%1=%2").arg(QString::fromStdString(pi.key()), pyLiteral(*pi));
      }
      if (!(n->transform.rotation == Vec3())) args << QString("rotation=%1").arg(vecLiteral(n->transform.rotation));
      if (!(n->transform.scale == Vec3(1.0f))) args << QString("scale=%1").arg(vecLiteral(n->transform.scale));
      args << QString("position=%1").arg(vecLiteral(n->worldPosition()));
      record(QString("veraa.create(%1)").arg(args.join(", ")));
      continue;
    }
    const NodeState& b = it.value();
    if (b.name != n->name) record(QString("%1.name = %2").arg(ref(b.name), pyString(QString::fromStdString(n->name))));
    QString r = ref(n->name);
    if (!(b.xf.position == n->transform.position)) record(QString("%1.position = %2").arg(r, vecLiteral(n->worldPosition())));
    if (!(b.xf.rotation == n->transform.rotation)) record(QString("%1.rotation = %2").arg(r, vecLiteral(n->transform.rotation)));
    if (!(b.xf.scale == n->transform.scale)) record(QString("%1.scale = %2").arg(r, vecLiteral(n->transform.scale)));
    if (n->object && b.type == n->object->type()) {
      const json p = n->object->params.toJson();
      for (auto pi = p.begin(); pi != p.end(); ++pi)
        if (!b.params.contains(pi.key()) || b.params[pi.key()] != *pi)
          record(QString("%1[%2] = %3").arg(r, pyString(QString::fromStdString(pi.key())), pyLiteral(*pi)));
    }
    if (b.parent != n->parentId) {
      Node* p = scene.findNode(n->parentId);
      record(QString("%1.parent = %2").arg(r, p ? ref(p->name) : QString("None")));
    }
    for (int m = b.modifiers; m < static_cast<int>(n->modifiers.size()); ++m)
      if (n->modifiers[m]) record(QString("%1.add_modifier(%2)").arg(r, pyString(QString::fromStdString(n->modifiers[m]->type()))));
  }
  for (auto it = before_.begin(); it != before_.end(); ++it)
    if (!now.contains(it.key())) record(QString("veraa.delete(%1)").arg(ref(it.value().name)));
}

}  // namespace veraa
