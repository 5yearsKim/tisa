#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include "semantics.h"

namespace tisa {
namespace {

using Bindings = std::unordered_map<std::string, Integer>;

Integer EvaluateIndex(const IndexExpr& expression, const Bindings& bindings) {
  Integer sum = 0;
  for (const auto& term : expression) {
    if (const auto* value = std::get_if<Integer>(&term)) {
      sum += *value;
    } else {
      sum += bindings.at(std::get<std::string>(term));
    }
  }
  return sum;
}

struct Span {
  std::size_t begin;
  std::size_t end;
};

struct AccessFault {
  std::string bank;
};

struct Update {
  std::vector<std::int32_t>& destination;
  std::size_t offset;
  std::vector<std::int32_t> values;
};

class Evaluation {
 public:
  Evaluation(const Program& program, const Bindings& bindings,
             const State& state)
      : bindings_(bindings), state_(state) {
    for (const auto& bank : program.banks) {
      banks_.emplace(bank.name, &bank);
    }
  }

  const Bank& BankOf(const Location& location) const {
    return *banks_.at(location.bank);
  }

  Span Resolve(const Location& location) const {
    const auto& bank = BankOf(location);
    const Integer begin = EvaluateIndex(location.start, bindings_);
    const Integer end =
        location.end ? EvaluateIndex(*location.end, bindings_) : begin + 1;
    if (begin < 0 || begin > end || end > bank.size) {
      throw AccessFault{location.bank};
    }
    const auto stride = bank.ElementsPerSlot();
    return {begin.convert_to<std::size_t>() * stride,
            end.convert_to<std::size_t>() * stride};
  }

  std::vector<Integer> Evaluate(const Expression& expression) const {
    if (const auto* location = std::get_if<Location>(&expression.node)) {
      const auto span = Resolve(*location);
      const auto& values = state_.at(location->bank);
      return {values.begin() + span.begin, values.begin() + span.end};
    }
    if (const auto* literal = std::get_if<TileLiteral>(&expression.node)) {
      std::vector<Integer> values;
      for (const auto& row : literal->rows) {
        values.insert(values.end(), row.begin(), row.end());
      }
      return values;
    }

    const auto& operation = std::get<Operation>(expression.node);
    auto lhs = Evaluate(operation.args.front());
    switch (operation.kind) {
      case Operation::Kind::kAdd:
        for (std::size_t arg = 1; arg < operation.args.size(); ++arg) {
          const auto rhs = Evaluate(operation.args[arg]);
          for (std::size_t i = 0; i < lhs.size(); ++i) {
            lhs[i] += rhs[i];
          }
        }
        return lhs;
      case Operation::Kind::kMatmul:
        return Matmul(lhs, *operation.args[0].type->tile,
                      Evaluate(operation.args[1]),
                      *operation.args[1].type->tile);
      case Operation::Kind::kWrapI32: {
        const Integer modulus = Integer(1) << 32;
        const Integer sign = Integer(1) << 31;
        for (auto& value : lhs) {
          value %= modulus;
          if (value < 0) {
            value += modulus;
          }
          if (value >= sign) {
            value -= modulus;
          }
        }
        return lhs;
      }
    }
    throw std::logic_error("unknown operation in validated expression");
  }

 private:
  static std::vector<Integer> Matmul(const std::vector<Integer>& lhs,
                                     TileShape left_shape,
                                     const std::vector<Integer>& rhs,
                                     TileShape right_shape) {
    std::vector<Integer> result(left_shape.rows * right_shape.cols);
    for (std::size_t row = 0; row < left_shape.rows; ++row) {
      for (std::size_t col = 0; col < right_shape.cols; ++col) {
        Integer& sum = result[row * right_shape.cols + col];
        for (std::size_t k = 0; k < left_shape.cols; ++k) {
          sum +=
              lhs[row * left_shape.cols + k] * rhs[k * right_shape.cols + col];
        }
      }
    }
    return result;
  }

  const Bindings& bindings_;
  const State& state_;
  std::unordered_map<std::string, const Bank*> banks_;
};

bool Fits(const Integer& value, IntegerKind kind) {
  if (kind == IntegerKind::kI8) {
    return value >= -128 && value <= 127;
  }
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

ExecutionResult CheckState(const Program& program, const State& state) {
  for (const auto& bank : program.banks) {
    const auto entry = state.find(bank.name);
    if (entry == state.end()) {
      return {Fault::kInvalidState, "missing state bank '" + bank.name + "'"};
    }
    const auto& values = entry->second;
    if (values.size() != bank.size * bank.ElementsPerSlot()) {
      return {Fault::kInvalidState,
              "wrong element count in '" + bank.name + "'"};
    }
    if (bank.type.element == IntegerKind::kI8 &&
        std::any_of(values.begin(), values.end(), [](std::int32_t value) {
          return value < -128 || value > 127;
        })) {
      return {Fault::kInvalidState,
              "element outside i8 range in '" + bank.name + "'"};
    }
  }
  if (state.size() != program.banks.size()) {
    return {Fault::kInvalidState, "initial state contains unknown banks"};
  }
  return {Fault::kNone, {}};
}

}  // namespace

ExecutionResult Execute(const ValidatedProgram& validated,
                        const std::string& name,
                        const std::vector<Integer>& operands, State& state) {
  const auto& program = validated.program();
  const auto instruction = std::find_if(
      program.instructions.begin(), program.instructions.end(),
      [&](const Instruction& candidate) { return candidate.name == name; });
  if (instruction == program.instructions.end()) {
    return {Fault::kUnknownInstruction, "unknown instruction '" + name + "'"};
  }
  if (instruction->operands.size() != operands.size()) {
    return {Fault::kInvalidOperands,
            "expected " + std::to_string(instruction->operands.size()) +
                " operands"};
  }

  Bindings bindings;
  for (std::size_t i = 0; i < operands.size(); ++i) {
    const auto& param = instruction->operands[i];
    if (operands[i] < 0 ||
        operands[i] > std::numeric_limits<std::uint32_t>::max()) {
      return {Fault::kInvalidOperands,
              "operand '" + param.name + "' is outside u32 range"};
    }
    bindings.emplace(param.name, operands[i]);
  }
  for (const auto& condition : instruction->preconditions) {
    const auto lhs = EvaluateIndex(condition.lhs, bindings);
    const auto rhs = EvaluateIndex(condition.rhs, bindings);
    const bool strict = condition.op == Comparison::Op::kLess;
    if (strict ? lhs >= rhs : lhs > rhs) {
      return {Fault::kPrecondition, "precondition failed for '" + name +
                                        "': " + lhs.str() +
                                        (strict ? " < " : " <= ") + rhs.str()};
    }
  }
  if (const auto result = CheckState(program, state); !result) {
    return result;
  }

  const Evaluation evaluation(program, bindings, state);
  std::vector<Update> updates;
  try {
    for (const auto& equation : instruction->equations) {
      const auto span = evaluation.Resolve(equation.target);
      const auto& bank = evaluation.BankOf(equation.target);
      auto& update =
          updates.emplace_back(Update{state.at(bank.name), span.begin, {}});
      update.values.reserve(span.end - span.begin);
      for (const auto& value : evaluation.Evaluate(equation.rhs)) {
        if (!Fits(value, bank.type.element)) {
          return {Fault::kUnrepresentable,
                  "result is outside element range in '" + bank.name + "'"};
        }
        update.values.push_back(value.convert_to<std::int32_t>());
      }
    }
  } catch (const AccessFault& fault) {
    return {Fault::kOutOfBounds,
            "out-of-bounds access to '" + fault.bank + "'"};
  }

  for (const auto& update : updates) {
    std::copy(update.values.begin(), update.values.end(),
              update.destination.begin() + update.offset);
  }
  return {Fault::kNone, {}};
}

}  // namespace tisa
