#ifndef TISA_AST_H_
#define TISA_AST_H_

#include <boost/multiprecision/cpp_int.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace tisa {

using Integer = boost::multiprecision::cpp_int;

enum class IntegerKind { kI8, kI32, kExact };

struct TileShape {
  std::size_t rows;
  std::size_t cols;

  bool operator==(const TileShape& other) const {
    return rows == other.rows && cols == other.cols;
  }
  bool operator!=(const TileShape& other) const { return !(*this == other); }
};

struct ValueType {
  IntegerKind element;
  // With no tile shape, the value is a one-dimensional buffer slice.
  std::optional<TileShape> tile;
};

struct Bank {
  std::string name;
  ValueType type;
  // Number of tiles in a tile bank, or elements in a flat buffer.
  std::size_t size;

  std::size_t ElementsPerSlot() const {
    return type.tile ? type.tile->rows * type.tile->cols : 1;
  }
};

using IndexTerm = std::variant<Integer, std::string>;
using IndexExpr = std::vector<IndexTerm>;

struct Location {
  std::string bank;
  IndexExpr start;
  std::optional<IndexExpr> end;
};

struct Comparison {
  IndexExpr lhs;
  IndexExpr rhs;
  enum class Op { kLess, kLessEqual } op;
};

struct TileLiteral {
  std::vector<std::vector<Integer>> rows;
};

struct Expression;
struct Operation {
  enum class Kind { kAdd, kMatmul, kWrapI32 } kind;
  std::vector<Expression> args;
};

struct Expression {
  std::variant<Location, TileLiteral, Operation> node;
  // Filled by validation. A slice's length is derived from its location.
  std::optional<ValueType> type;
};

struct Equation {
  Location target;
  Expression rhs;
};

struct Operand {
  std::string name;  // All operands in this version have type u32.
};

struct Instruction {
  std::string name;
  std::vector<Operand> operands;
  std::vector<Comparison> preconditions;
  std::vector<Location> reads;
  std::vector<Location> writes;
  std::vector<Equation> equations;
};

struct Program {
  std::vector<Bank> banks;
  std::vector<Instruction> instructions;
};

}  // namespace tisa

#endif  // TISA_AST_H_
