#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "semantics.h"
#include "z3++.h"

namespace tisa {
namespace {

using Banks = std::unordered_map<std::string, const Bank*>;

struct Region {
  const Bank& bank;
  z3::expr begin;
  z3::expr end;

  z3::expr Contains(const z3::expr& slot) const {
    return begin <= slot && slot < end;
  }
};

struct ExpressionType {
  ValueType value;
  std::optional<z3::expr> slice_length;
};

class InstructionValidator {
 public:
  InstructionValidator(const Banks& banks, Instruction& instruction)
      : banks_(banks),
        instruction_(instruction),
        solver_(context_, "QF_LIA"),
        slot_(context_.int_const("slot")) {}

  void Run() {
    for (const auto& operand : instruction_.operands) {
      // Internal symbols use a separate namespace from source identifiers.
      const auto symbol =
          context_.int_const(("operand." + operand.name).c_str());
      if (!operands_.emplace(operand.name, symbol).second) {
        Fail("duplicate operand '" + operand.name + "'");
      }
      solver_.add(symbol >= 0 &&
                  symbol <= Number(std::numeric_limits<std::uint32_t>::max()));
    }
    for (const auto& comparison : instruction_.preconditions) {
      const auto lhs = Index(comparison.lhs);
      const auto rhs = Index(comparison.rhs);
      solver_.add(comparison.op == Comparison::Op::kLess ? lhs < rhs
                                                         : lhs <= rhs);
    }
    for (const auto& read : instruction_.reads) {
      reads_.push_back(Resolve(read));
    }
    std::vector<Region> writes;
    for (const auto& write : instruction_.writes) {
      writes.push_back(Resolve(write));
    }

    std::vector<Region> targets;
    for (auto& equation : instruction_.equations) {
      auto target = Resolve(equation.target);
      const auto rhs = TypeOf(equation.rhs);
      if (target.bank.type.tile != rhs.value.tile) {
        Fail("assignment shape mismatch for '" + target.bank.name + "'");
      }
      if (rhs.slice_length) {
        Prove(
            target.end - target.begin == *rhs.slice_length,
            "assignment slice length mismatch for '" + target.bank.name + "'");
      }

      RequireCovered(target, writes,
                     "equation target is outside declared writes");
      for (const auto& previous : targets) {
        if (&target.bank == &previous.bank) {
          Prove(!(target.Contains(slot_) && previous.Contains(slot_)),
                "overlapping equation targets in '" + target.bank.name + "'");
        }
      }
      targets.push_back(std::move(target));
    }
    for (const auto& write : writes) {
      RequireCovered(write, targets,
                     "declared writes are missing equation targets");
    }
  }

 private:
  [[noreturn]] void Fail(const std::string& message) const {
    throw std::invalid_argument("instruction '" + instruction_.name +
                                "': " + message);
  }

  z3::expr Number(const Integer& value) {
    return context_.int_val(value.str().c_str());
  }

  z3::expr Index(const IndexExpr& index) {
    if (index.empty()) {
      Fail("empty index expression");
    }
    z3::expr sum = context_.int_val(0);
    for (const auto& term : index) {
      if (const auto* value = std::get_if<Integer>(&term)) {
        sum = sum + Number(*value);
      } else {
        const auto& name = std::get<std::string>(term);
        const auto operand = operands_.find(name);
        if (operand == operands_.end()) {
          Fail("unknown operand '" + name + "'");
        }
        sum = sum + operand->second;
      }
    }
    return sum;
  }

  // Search for an operand assignment (and, for coverage, a slot) that violates
  // the property while satisfying the instruction's preconditions.
  void Prove(const z3::expr& property, const std::string& message) {
    solver_.push();
    solver_.add(!property);
    const auto result = solver_.check();
    solver_.pop();
    if (result == z3::sat) {
      Fail(message);
    }
    if (result == z3::unknown) {
      Fail("could not verify that " + message + " never occurs");
    }
  }

  Region Resolve(const Location& location) {
    const auto found = banks_.find(location.bank);
    if (found == banks_.end()) {
      Fail("unknown state bank '" + location.bank + "'");
    }
    const auto& bank = *found->second;
    if (bank.type.tile.has_value() == location.end.has_value()) {
      Fail("expected " +
           std::string(bank.type.tile ? "a tile index" : "a slice") + " for '" +
           bank.name + "'");
    }

    auto begin = Index(location.start);
    auto end = location.end ? Index(*location.end) : begin + 1;
    Prove(begin >= 0 && begin <= end && end <= Number(bank.size),
          "out-of-bounds access to '" + bank.name + "'");
    return {bank, std::move(begin), std::move(end)};
  }

  void RequireCovered(const Region& region, const std::vector<Region>& covering,
                      const std::string& message) {
    auto covered = context_.bool_val(false);
    for (const auto& candidate : covering) {
      if (&region.bank == &candidate.bank) {
        covered = covered || candidate.Contains(slot_);
      }
    }
    Prove(z3::implies(region.Contains(slot_), covered),
          message + " in '" + region.bank.name + "'");
  }

  ExpressionType TypeOf(Expression& expression) {
    if (const auto* location = std::get_if<Location>(&expression.node)) {
      const auto region = Resolve(*location);
      RequireCovered(region, reads_, "equation read is outside declared reads");
      expression.type = region.bank.type;
      std::optional<z3::expr> length;
      if (!region.bank.type.tile) {
        length = region.end - region.begin;
      }
      return {*expression.type, std::move(length)};
    }

    if (const auto* literal = std::get_if<TileLiteral>(&expression.node)) {
      if (literal->rows.empty() || literal->rows.front().empty()) {
        Fail("tile literals must have at least one row and one column");
      }
      const auto cols = literal->rows.front().size();
      for (const auto& row : literal->rows) {
        if (row.size() != cols) {
          Fail("tile literal rows must have the same length");
        }
      }
      expression.type =
          ValueType{IntegerKind::kExact, TileShape{literal->rows.size(), cols}};
    } else {
      expression.type = TypeOf(std::get<Operation>(expression.node));
    }
    return {*expression.type, std::nullopt};
  }

  ValueType TypeOf(Operation& op) {
    if ((op.kind == Operation::Kind::kAdd && op.args.size() < 2) ||
        (op.kind == Operation::Kind::kMatmul && op.args.size() != 2) ||
        (op.kind == Operation::Kind::kWrapI32 && op.args.size() != 1)) {
      Fail("invalid operation arity");
    }

    std::vector<TileShape> shapes;
    for (auto& arg : op.args) {
      const auto type = TypeOf(arg).value;
      if (!type.tile) {
        Fail("arithmetic requires tile operands");
      }
      shapes.push_back(*type.tile);
    }
    switch (op.kind) {
      case Operation::Kind::kAdd:
        for (const auto shape : shapes) {
          if (shape != shapes.front()) {
            Fail("tile addition requires equal shapes");
          }
        }
        return {IntegerKind::kExact, shapes.front()};
      case Operation::Kind::kMatmul:
        if (shapes[0].cols != shapes[1].rows) {
          Fail("matmul_int requires matching inner dimensions");
        }
        if (shapes[0].rows >
            std::vector<Integer>().max_size() / shapes[1].cols) {
          Fail("matmul_int result shape is too large");
        }
        return {IntegerKind::kExact, TileShape{shapes[0].rows, shapes[1].cols}};
      case Operation::Kind::kWrapI32:
        return {IntegerKind::kI32, shapes.front()};
    }
    Fail("unknown operation");
  }

  const Banks& banks_;
  Instruction& instruction_;
  z3::context context_;
  z3::solver solver_;
  z3::expr slot_;
  std::unordered_map<std::string, z3::expr> operands_;
  std::vector<Region> reads_;
};

}  // namespace

ValidatedProgram Validate(Program program) {
  Banks banks;
  for (const auto& bank : program.banks) {
    if (!banks.emplace(bank.name, &bank).second) {
      throw std::invalid_argument("duplicate state bank '" + bank.name + "'");
    }
    if (bank.type.element == IntegerKind::kExact) {
      throw std::invalid_argument("state bank '" + bank.name +
                                  "' requires a fixed-width element type");
    }

    const auto max_elements = std::vector<std::int32_t>().max_size();
    if (bank.type.tile) {
      const auto [rows, cols] = *bank.type.tile;
      if (rows == 0 || cols == 0 ||
          rows > std::vector<Integer>().max_size() / cols) {
        throw std::invalid_argument("invalid tile shape for '" + bank.name +
                                    "'");
      }
    }
    if (bank.size > max_elements / bank.ElementsPerSlot()) {
      throw std::invalid_argument("state bank '" + bank.name +
                                  "' is too large");
    }
  }

  std::unordered_set<std::string> names;
  for (auto& instruction : program.instructions) {
    if (!names.insert(instruction.name).second) {
      throw std::invalid_argument("duplicate instruction '" + instruction.name +
                                  "'");
    }
    InstructionValidator(banks, instruction).Run();
  }
  return ValidatedProgram(std::move(program));
}

}  // namespace tisa
