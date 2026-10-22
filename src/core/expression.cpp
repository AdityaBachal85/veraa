#include "expression.h"

#include <cctype>
#include <cmath>
#include <set>

#include "modifier.h"  // perlinNoise
#include "vmath.h"

namespace veraa {

struct Expression::Node {
  enum class Kind { Number, Variable, Unary, Binary, Call, Conditional } kind = Kind::Number;
  double number = 0.0;
  std::string name;  // variable or function name
  char op = 0;       // unary/binary operator: + - * / ^ % < > l(<=) g(>=) =(==) !(!=) &(&&) |(||)
  std::vector<std::shared_ptr<const Node>> args;
};

namespace {

using NodePtr = std::shared_ptr<const Expression::Node>;

class Parser {
 public:
  explicit Parser(const std::string& s) : s_(s) {}

  NodePtr parse(std::string* error) {
    NodePtr n = conditional();
    skipSpace();
    if (n && pos_ != s_.size()) fail("unexpected '" + std::string(1, s_[pos_]) + "'");
    if (!error_.empty()) {
      if (error) *error = error_;
      return nullptr;
    }
    return n;
  }

 private:
  void fail(const std::string& what) {
    if (error_.empty()) error_ = what + " at column " + std::to_string(pos_ + 1);
  }
  void skipSpace() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
  }
  bool accept(const char* tok) {
    skipSpace();
    size_t n = std::char_traits<char>::length(tok);
    if (s_.compare(pos_, n, tok) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }
  static NodePtr makeBinary(char op, NodePtr a, NodePtr b) {
    auto n = std::make_shared<Expression::Node>();
    n->kind = Expression::Node::Kind::Binary;
    n->op = op;
    n->args = {std::move(a), std::move(b)};
    return n;
  }

  NodePtr conditional() {
    NodePtr c = logicOr();
    if (!c) return nullptr;
    if (accept("?")) {
      NodePtr a = conditional();
      if (!accept(":")) {
        fail("expected ':'");
        return nullptr;
      }
      NodePtr b = conditional();
      if (!a || !b) return nullptr;
      auto n = std::make_shared<Expression::Node>();
      n->kind = Expression::Node::Kind::Conditional;
      n->args = {c, a, b};
      return n;
    }
    return c;
  }
  NodePtr logicOr() {
    NodePtr a = logicAnd();
    while (a && accept("||")) a = makeBinary('|', a, logicAnd());
    return a;
  }
  NodePtr logicAnd() {
    NodePtr a = comparison();
    while (a && accept("&&")) a = makeBinary('&', a, comparison());
    return a;
  }
  NodePtr comparison() {
    NodePtr a = additive();
    while (a) {
      if (accept("<=")) a = makeBinary('l', a, additive());
      else if (accept(">=")) a = makeBinary('g', a, additive());
      else if (accept("==")) a = makeBinary('=', a, additive());
      else if (accept("!=")) a = makeBinary('!', a, additive());
      else if (accept("<")) a = makeBinary('<', a, additive());
      else if (accept(">")) a = makeBinary('>', a, additive());
      else break;
    }
    return a;
  }
  NodePtr additive() {
    NodePtr a = multiplicative();
    while (a) {
      if (accept("+")) a = makeBinary('+', a, multiplicative());
      else if (accept("-")) a = makeBinary('-', a, multiplicative());
      else break;
    }
    return a;
  }
  NodePtr multiplicative() {
    NodePtr a = unary();
    while (a) {
      if (accept("*")) a = makeBinary('*', a, unary());
      else if (accept("/")) a = makeBinary('/', a, unary());
      else if (accept("%")) a = makeBinary('%', a, unary());
      else break;
    }
    return a;
  }
  NodePtr unary() {
    if (accept("-")) {
      NodePtr x = unary();
      if (!x) return nullptr;
      auto n = std::make_shared<Expression::Node>();
      n->kind = Expression::Node::Kind::Unary;
      n->op = '-';
      n->args = {x};
      return n;
    }
    if (accept("+")) return unary();
    if (accept("!")) {
      NodePtr x = unary();
      if (!x) return nullptr;
      auto n = std::make_shared<Expression::Node>();
      n->kind = Expression::Node::Kind::Unary;
      n->op = '!';
      n->args = {x};
      return n;
    }
    return power();
  }
  NodePtr power() {
    NodePtr base = primary();
    if (base && accept("^")) {
      NodePtr e = unary();  // right-associative, binds tighter than unary minus on the left
      if (!e) return nullptr;
      return makeBinary('^', base, e);
    }
    return base;
  }
  NodePtr primary() {
    skipSpace();
    if (pos_ >= s_.size()) {
      fail("unexpected end");
      return nullptr;
    }
    char c = s_[pos_];
    if (c == '(') {
      ++pos_;
      NodePtr n = conditional();
      if (!accept(")")) {
        fail("expected ')'");
        return nullptr;
      }
      return n;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
      size_t start = pos_;
      while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.')) ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
        size_t save = pos_++;
        if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
        if (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
          while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
        } else {
          pos_ = save;
        }
      }
      auto n = std::make_shared<Expression::Node>();
      try {
        n->number = std::stod(s_.substr(start, pos_ - start));
      } catch (...) {
        fail("bad number");
        return nullptr;
      }
      return n;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t start = pos_;
      while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
      std::string name = s_.substr(start, pos_ - start);
      if (accept("(")) {
        auto n = std::make_shared<Expression::Node>();
        n->kind = Expression::Node::Kind::Call;
        n->name = name;
        if (!accept(")")) {
          do {
            NodePtr a = conditional();
            if (!a) return nullptr;
            n->args.push_back(a);
          } while (accept(","));
          if (!accept(")")) {
            fail("expected ')' after arguments");
            return nullptr;
          }
        }
        static const std::set<std::string> known = {
            "sin",  "cos",  "tan",   "asin", "acos",  "atan", "atan2", "sqrt",  "abs",     "min",
            "max",  "pow",  "exp",   "log",  "log10", "floor", "ceil", "round", "clamp",   "lerp",
            "noise", "sign", "mod",  "degToRad", "radToDeg", "smoothstep", "if"};
        if (!known.count(name)) {
          fail("unknown function '" + name + "'");
          return nullptr;
        }
        return n;
      }
      auto n = std::make_shared<Expression::Node>();
      if (name == "pi" || name == "PI") {
        n->number = kPi;
      } else {
        n->kind = Expression::Node::Kind::Variable;
        n->name = name;
      }
      return n;
    }
    fail("unexpected '" + std::string(1, c) + "'");
    return nullptr;
  }

  const std::string& s_;
  size_t pos_ = 0;
  std::string error_;
};

double callFunction(const std::string& f, const std::vector<double>& a) {
  auto arg = [&](size_t i) { return i < a.size() ? a[i] : 0.0; };
  if (f == "sin") return std::sin(arg(0));
  if (f == "cos") return std::cos(arg(0));
  if (f == "tan") return std::tan(arg(0));
  if (f == "asin") return std::asin(std::clamp(arg(0), -1.0, 1.0));
  if (f == "acos") return std::acos(std::clamp(arg(0), -1.0, 1.0));
  if (f == "atan") return std::atan(arg(0));
  if (f == "atan2") return std::atan2(arg(0), arg(1));
  if (f == "sqrt") return std::sqrt(std::max(0.0, arg(0)));
  if (f == "abs") return std::fabs(arg(0));
  if (f == "min") {
    double m = arg(0);
    for (double v : a) m = std::min(m, v);
    return m;
  }
  if (f == "max") {
    double m = arg(0);
    for (double v : a) m = std::max(m, v);
    return m;
  }
  if (f == "pow") return std::pow(arg(0), arg(1));
  if (f == "exp") return std::exp(arg(0));
  if (f == "log") return arg(0) > 0 ? std::log(arg(0)) : 0.0;
  if (f == "log10") return arg(0) > 0 ? std::log10(arg(0)) : 0.0;
  if (f == "floor") return std::floor(arg(0));
  if (f == "ceil") return std::ceil(arg(0));
  if (f == "round") return std::round(arg(0));
  if (f == "clamp") return std::clamp(arg(0), std::min(arg(1), arg(2)), std::max(arg(1), arg(2)));
  if (f == "lerp") return arg(0) + (arg(1) - arg(0)) * arg(2);
  if (f == "noise") return perlinNoise(static_cast<float>(arg(0)), static_cast<float>(arg(1)), static_cast<float>(arg(2)));
  if (f == "sign") return arg(0) > 0 ? 1.0 : (arg(0) < 0 ? -1.0 : 0.0);
  if (f == "mod") return arg(1) != 0 ? std::fmod(arg(0), arg(1)) : 0.0;
  if (f == "degToRad") return arg(0) * kPi / 180.0;
  if (f == "radToDeg") return arg(0) * 180.0 / kPi;
  if (f == "smoothstep") {
    double e0 = arg(0), e1 = arg(1);
    double t = e1 != e0 ? std::clamp((arg(2) - e0) / (e1 - e0), 0.0, 1.0) : 0.0;
    return t * t * (3 - 2 * t);
  }
  if (f == "if") return arg(0) != 0.0 ? arg(1) : arg(2);
  return 0.0;
}

double eval(const Expression::Node& n, const std::function<double(const std::string&)>& var) {
  using K = Expression::Node::Kind;
  switch (n.kind) {
    case K::Number: return n.number;
    case K::Variable: return var ? var(n.name) : 0.0;
    case K::Unary: {
      double x = eval(*n.args[0], var);
      return n.op == '-' ? -x : (x == 0.0 ? 1.0 : 0.0);
    }
    case K::Conditional: return eval(*n.args[0], var) != 0.0 ? eval(*n.args[1], var) : eval(*n.args[2], var);
    case K::Call: {
      std::vector<double> a;
      a.reserve(n.args.size());
      for (const auto& x : n.args) a.push_back(eval(*x, var));
      return callFunction(n.name, a);
    }
    case K::Binary: {
      double a = eval(*n.args[0], var), b = eval(*n.args[1], var);
      switch (n.op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0.0 ? a / b : 0.0;
        case '%': return b != 0.0 ? std::fmod(a, b) : 0.0;
        case '^': return std::pow(a, b);
        case '<': return a < b;
        case '>': return a > b;
        case 'l': return a <= b;
        case 'g': return a >= b;
        case '=': return a == b;
        case '!': return a != b;
        case '&': return (a != 0.0 && b != 0.0) ? 1.0 : 0.0;
        case '|': return (a != 0.0 || b != 0.0) ? 1.0 : 0.0;
      }
      return 0.0;
    }
  }
  return 0.0;
}

void collect(const Expression::Node& n, std::set<std::string>& out) {
  if (n.kind == Expression::Node::Kind::Variable) out.insert(n.name);
  for (const auto& a : n.args) collect(*a, out);
}

}  // namespace

bool Expression::parse(const std::string& text, std::string* error) {
  text_ = text;
  root_.reset();
  Parser p(text);
  NodePtr n = p.parse(error);
  if (!n) return false;
  root_ = n;
  return true;
}

std::vector<std::string> Expression::variables() const {
  std::set<std::string> names;
  if (root_) collect(*root_, names);
  return std::vector<std::string>(names.begin(), names.end());
}

double Expression::evaluate(const std::function<double(const std::string&)>& variable) const {
  if (!root_) return 0.0;
  double v = eval(*root_, variable);
  return std::isfinite(v) ? v : 0.0;
}

}  // namespace veraa
