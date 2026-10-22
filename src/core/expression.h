// Small, safe math expression language used by Expression controllers, wired parameters and
// reaction mappings. Expressions are pure: numbers, named variables, + - * / ^ %, comparisons,
// ?: and functions such as sin, cos, sqrt, min, max, clamp, lerp, noise. No side effects.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace veraa {

class Expression {
 public:
  Expression() = default;
  // Parses the text. On failure returns false and sets error (with the column).
  bool parse(const std::string& text, std::string* error = nullptr);
  bool valid() const { return root_ != nullptr; }
  const std::string& text() const { return text_; }
  // Names of variables used (e.g. "F", "T", "angle"), for binding.
  std::vector<std::string> variables() const;
  // Evaluates with a variable resolver; unknown variables evaluate to 0.
  double evaluate(const std::function<double(const std::string&)>& variable) const;

  struct Node;

 private:
  std::string text_;
  std::shared_ptr<const Node> root_;
};

}  // namespace veraa
