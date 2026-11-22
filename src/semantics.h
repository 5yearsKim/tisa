#ifndef TISA_SEMANTICS_H_
#define TISA_SEMANTICS_H_

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"

namespace tisa {

class ValidatedProgram {
 public:
  const Program& program() const { return program_; }

 private:
  friend ValidatedProgram Validate(Program program);
  explicit ValidatedProgram(Program program) : program_(std::move(program)) {}

  Program program_;
};

// Throws std::invalid_argument if the specification is invalid.
ValidatedProgram Validate(Program program);

// Each tile bank is flattened by tile, then by row within each tile.
using State = std::map<std::string, std::vector<std::int32_t>>;

enum class Fault {
  kNone,
  kUnknownInstruction,
  kInvalidOperands,
  kInvalidState,
  kPrecondition,
  kOutOfBounds,
  kUnrepresentable,
};

struct ExecutionResult {
  Fault fault;
  std::string message;
  explicit operator bool() const { return fault == Fault::kNone; }
};

// Applies all updates on success. Any reported fault leaves state unchanged.
ExecutionResult Execute(const ValidatedProgram& program,
                        const std::string& instruction,
                        const std::vector<Integer>& operands, State& state);

}  // namespace tisa

#endif  // TISA_SEMANTICS_H_
