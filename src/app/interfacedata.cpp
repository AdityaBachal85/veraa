#include "interfacedata.h"

#include <QObject>

namespace veraa {

namespace {

const json* section(const Scene& s, const char* key) {
  auto it = s.extensions.find("interface");
  if (it == s.extensions.end() || !it->is_object()) return nullptr;
  auto jt = it->find(key);
  if (jt == it->end()) return nullptr;
  return &*jt;
}

json& section(Scene& s, const char* key, const json& init) {
  json& iface = s.extensions["interface"];
  if (!iface.is_object()) iface = json::object();
  json& sec = iface[key];
  if (sec.is_null()) sec = init;
  return sec;
}

std::string key(const QString& q) { return q.toStdString(); }

}  // namespace

// ---- Selection sets ----

QStringList selectionSetNames(const Scene& s) {
  QStringList out;
  if (const json* sets = section(s, "selectionSets"); sets && sets->is_object())
    for (auto it = sets->begin(); it != sets->end(); ++it) out << QString::fromStdString(it.key());
  out.sort(Qt::CaseInsensitive);
  return out;
}

QVector<uint32_t> selectionSetMembers(const Scene& s, const QString& name) {
  QVector<uint32_t> out;
  const json* sets = section(s, "selectionSets");
  if (!sets || !sets->is_object() || !sets->contains(key(name))) return out;
  for (const auto& v : (*sets)[key(name)])
    if (v.is_number_unsigned() || v.is_number_integer()) {
      uint32_t id = v.get<uint32_t>();
      if (s.findNode(id)) out.push_back(id);
    }
  return out;
}

void setSelectionSet(SceneDocument* doc, const QString& name, const QVector<uint32_t>& ids) {
  doc->beginEdit(QObject::tr("Selection Set"));
  json& sets = section(doc->scene(), "selectionSets", json::object());
  json arr = json::array();
  for (uint32_t id : ids) arr.push_back(id);
  sets[key(name)] = arr;
  doc->notifyStructureChanged();
  doc->endEdit();
}

void removeSelectionSet(SceneDocument* doc, const QString& name) {
  doc->beginEdit(QObject::tr("Delete Selection Set"));
  json& sets = section(doc->scene(), "selectionSets", json::object());
  sets.erase(key(name));
  doc->notifyStructureChanged();
  doc->endEdit();
}

void renameSelectionSet(SceneDocument* doc, const QString& from, const QString& to) {
  if (from == to || to.trimmed().isEmpty()) return;
  doc->beginEdit(QObject::tr("Rename Selection Set"));
  json& sets = section(doc->scene(), "selectionSets", json::object());
  if (sets.contains(key(from))) {
    json v = sets[key(from)];
    sets.erase(key(from));
    sets[key(to)] = v;
  }
  doc->notifyStructureChanged();
  doc->endEdit();
}

// ---- Scene states ----

QStringList sceneStateNames(const Scene& s) {
  QStringList out;
  if (const json* states = section(s, "sceneStates"); states && states->is_object())
    for (auto it = states->begin(); it != states->end(); ++it) out << QString::fromStdString(it.key());
  out.sort(Qt::CaseInsensitive);
  return out;
}

int sceneStateParts(const Scene& s, const QString& name) {
  const json* states = section(s, "sceneStates");
  if (!states || !states->contains(key(name))) return 0;
  return (*states)[key(name)].value("parts", 0);
}

void saveSceneState(SceneDocument* doc, const QString& name, int parts) {
  Scene& scene = doc->scene();
  json st;
  st["parts"] = parts;
  json nodes = json::object();
  for (const auto& n : scene.nodes) {
    json jn;
    if (parts & StateHidden) jn["hidden"] = n->hidden;
    if (parts & StateFrozen) jn["frozen"] = n->frozen;
    if (parts & StateTransforms) jn["transform"] = n->transform.toJson();
    if (parts & StateMaterials) jn["material"] = n->materialId;
    if (parts & StateLayers) jn["layer"] = n->layerId;
    if (parts & StateDisplay) {
      jn["wireColor"] = vec3ToJson(n->wireColor);
      jn["seeThrough"] = n->seeThrough;
      jn["displayAsBox"] = n->displayAsBox;
    }
    nodes[std::to_string(n->id)] = jn;
  }
  st["nodes"] = nodes;
  if (parts & StateLayers) {
    json layers = json::array();
    for (const Layer& l : scene.layers)
      layers.push_back({{"id", l.id}, {"hidden", l.hidden}, {"frozen", l.frozen}, {"renderable", l.renderable}});
    st["layers"] = layers;
  }
  doc->beginEdit(QObject::tr("Save Scene State"));
  section(scene, "sceneStates", json::object())[key(name)] = st;
  doc->endEdit();
}

bool restoreSceneState(SceneDocument* doc, const QString& name) {
  Scene& scene = doc->scene();
  const json* states = section(scene, "sceneStates");
  if (!states || !states->contains(key(name))) return false;
  json st = (*states)[key(name)];
  int parts = st.value("parts", 0);
  doc->beginEdit(QObject::tr("Restore Scene State"));
  QVector<uint32_t> ids;
  const json& nodes = st["nodes"];
  for (const auto& n : scene.nodes) {
    auto it = nodes.find(std::to_string(n->id));
    if (it == nodes.end()) continue;
    const json& jn = *it;
    if ((parts & StateHidden) && jn.contains("hidden")) n->hidden = jn["hidden"];
    if ((parts & StateFrozen) && jn.contains("frozen")) n->frozen = jn["frozen"];
    if ((parts & StateTransforms) && jn.contains("transform")) n->transform.fromJson(jn["transform"]);
    if ((parts & StateMaterials) && jn.contains("material")) {
      uint32_t m = jn["material"];
      n->materialId = m == 0 || scene.findMaterial(m) ? m : n->materialId;
    }
    if ((parts & StateLayers) && jn.contains("layer")) {
      uint32_t l = jn["layer"];
      if (scene.findLayer(l)) n->layerId = l;
    }
    if (parts & StateDisplay) {
      if (jn.contains("wireColor")) n->wireColor = vec3FromJson(jn["wireColor"], n->wireColor);
      n->seeThrough = jn.value("seeThrough", n->seeThrough);
      n->displayAsBox = jn.value("displayAsBox", n->displayAsBox);
    }
    ids.push_back(n->id);
  }
  if ((parts & StateLayers) && st.contains("layers"))
    for (const auto& jl : st["layers"])
      if (Layer* l = scene.findLayer(jl.value("id", 0u))) {
        l->hidden = jl.value("hidden", l->hidden);
        l->frozen = jl.value("frozen", l->frozen);
        l->renderable = jl.value("renderable", l->renderable);
      }
  doc->notifyNodesChanged(ids, ChangeAll);
  doc->notifyStructureChanged();
  doc->endEdit();
  return true;
}

void deleteSceneState(SceneDocument* doc, const QString& name) {
  doc->beginEdit(QObject::tr("Delete Scene State"));
  section(doc->scene(), "sceneStates", json::object()).erase(key(name));
  doc->endEdit();
}

// ---- File properties ----

json fileProperties(const Scene& s) {
  const json* p = section(s, "fileProperties");
  return p && p->is_object() ? *p : json::object();
}

void setFileProperties(SceneDocument* doc, const json& props) {
  doc->beginEdit(QObject::tr("File Properties"));
  section(doc->scene(), "fileProperties", json::object()) = props;
  doc->endEdit();
}

// ---- XRefs ----

QVector<XRefEntry> xrefScenes(const Scene& s) {
  QVector<XRefEntry> out;
  const json* list = section(s, "xrefScenes");
  if (!list || !list->is_array()) return out;
  for (const auto& j : *list) {
    XRefEntry e;
    e.path = QString::fromStdString(j.value("path", std::string()));
    e.enabled = j.value("enabled", true);
    if (j.contains("nodes"))
      for (const auto& id : j["nodes"]) e.nodeIds.push_back(id.get<uint32_t>());
    out.push_back(e);
  }
  return out;
}

void setXRefScenes(Scene& s, const QVector<XRefEntry>& list) {
  json arr = json::array();
  for (const auto& e : list) {
    json ids = json::array();
    for (uint32_t id : e.nodeIds) ids.push_back(id);
    arr.push_back({{"path", e.path.toStdString()}, {"enabled", e.enabled}, {"nodes", ids}});
  }
  section(s, "xrefScenes", json::array()) = arr;
}

// ---- Transform locks ----

int transformLocks(const Scene& s, uint32_t nodeId) {
  const json* locks = section(s, "locks");
  if (!locks || !locks->is_object()) return 0;
  auto it = locks->find(std::to_string(nodeId));
  return it == locks->end() ? 0 : it->get<int>();
}

void setTransformLocks(SceneDocument* doc, uint32_t nodeId, int mask) {
  doc->beginEdit(QObject::tr("Transform Locks"));
  json& locks = section(doc->scene(), "locks", json::object());
  if (mask) locks[std::to_string(nodeId)] = mask;
  else locks.erase(std::to_string(nodeId));
  doc->endEdit();
}

void applyTransformLocks(const Scene& s, uint32_t nodeId, const Transform& before, Transform& t) {
  int m = transformLocks(s, nodeId);
  if (!m) return;
  for (int a = 0; a < 3; ++a) {
    if (m & (1 << a)) t.position[a] = before.position[a];
    if (m & (1 << (a + 3))) t.rotation[a] = before.rotation[a];
    if (m & (1 << (a + 6))) t.scale[a] = before.scale[a];
  }
}

// ---- Working pivot ----

bool hasWorkingPivot(const Scene& s) {
  const json* w = section(s, "workingPivot");
  return w && w->is_object();
}

Transform workingPivotTransform(const Scene& s) {
  Transform t;
  if (const json* w = section(s, "workingPivot"); w && w->is_object()) t.fromJson(*w);
  return t;
}

void setWorkingPivot(SceneDocument* doc, const Transform* t) {
  doc->beginEdit(QObject::tr("Working Pivot"));
  json& iface = doc->scene().extensions["interface"];
  if (!iface.is_object()) iface = json::object();
  if (t) iface["workingPivot"] = t->toJson();
  else iface.erase("workingPivot");
  doc->notifyNodesChanged({}, ChangeTransform);
  doc->endEdit();
}

}  // namespace veraa
