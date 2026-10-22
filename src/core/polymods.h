// Sub-object modifiers: Edit Poly (records editing operations and replays them on its input),
// Poly Select / Mesh Select (store a sub-object selection), Vol. Select (selects by volume)
// and Turn to Poly (polygon clean-up). Selections are passed up the stack in
// GeomState::selWeights so deformers above affect only the selected part.
#pragma once

#include <memory>
#include <string>

#include "modifier.h"
#include "polyops.h"

namespace veraa {

// Modifiers whose sub-objects the Modify panel tools pick and edit. The last evaluation is
// cached so the tools can show and pick sub-objects of the mesh at this point of the stack.
class SubObjectModifier : public Modifier {
 public:
  mutable PolyMesh cachedMesh;      // result of the last evaluation (object space)
  mutable PolySelection cachedSel;  // selection on cachedMesh
  mutable bool cacheValid = false;

  // Current selection (valid after the node has been evaluated).
  virtual PolySelection selection() const = 0;
  // Replaces the selection (bumps the revision so the node re-evaluates).
  virtual void setSelection(const PolySelection& s) = 0;
  // Edit Poly can run editing operations; the select modifiers only select.
  virtual bool canEdit() const { return false; }
};

class EditPolyModifier : public SubObjectModifier {
 public:
  std::string type() const override { return "EditPoly"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<EditPolyModifier>(*this); }
  void applyState(GeomState& st) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  PolySelection selection() const override { return cachedSel; }
  void setSelection(const PolySelection& s) override;
  bool canEdit() const override { return true; }
  // Appends an operation (applied with the current selection).
  void record(const json& op);
  // A transient operation shown on top of the recorded ones (interactive previews).
  void setPreview(const json& op);
  void clearPreview();
  bool hasPreview() const { return !preview_.is_null(); }

  json ops = json::array();  // recorded operations, replayed in order

 private:
  json preview_;
};

class PolySelectModifier : public SubObjectModifier {
 public:
  explicit PolySelectModifier(std::string typeName = "PolySelect") : type_(std::move(typeName)) {}
  std::string type() const override { return type_; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<PolySelectModifier>(*this); }
  void applyState(GeomState& st) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  PolySelection selection() const override { return sel; }
  void setSelection(const PolySelection& s) override;

  PolySelection sel;

 private:
  std::string type_;
};

void registerPolyModifierTypes();  // EditPoly, PolySelect, MeshSelect, VolSelect, TurnToPoly

}  // namespace veraa
