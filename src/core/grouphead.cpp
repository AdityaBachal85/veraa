// The helper object at the head of a group (Group > Group). Members are linked to it and
// flagged as group members; it has no geometry of its own.
#include "object.h"
#include "registry.h"

namespace veraa {

namespace {

class GroupHeadObject : public HelperObject {
 public:
  std::string type() const override { return "GroupHead"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<GroupHeadObject>(*this); }
};

}  // namespace

void registerGroupObjectTypes() {
  // No category: groups are made from the Group menu, not the Create panel.
  registerObjectType({"GroupHead", "Group", "", "", [] { return std::make_unique<GroupHeadObject>(); }});
}

}  // namespace veraa
