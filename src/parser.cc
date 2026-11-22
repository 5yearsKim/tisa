#include "parser.h"

#include <limits>
#include <stdexcept>
#include <utility>

#include "antlr4-runtime.h"
#include "src/TisaLexer.h"
#include "src/TisaParser.h"

namespace tisa {
namespace {

class SyntaxErrors : public antlr4::BaseErrorListener {
 public:
  void syntaxError(antlr4::Recognizer*, antlr4::Token*, std::size_t line,
                   std::size_t column, const std::string& message,
                   std::exception_ptr) override {
    throw std::invalid_argument("line " + std::to_string(line) + ":" +
                                std::to_string(column + 1) + ": " + message);
  }
};

Integer Decimal(std::string_view digits) {
  Integer value = 0;
  for (char digit : digits) {
    value *= 10;
    value += digit - '0';
  }
  return value;
}

std::size_t Dimension(antlr4::tree::TerminalNode* token) {
  const Integer value = Decimal(token->getText());
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("state dimension is too large");
  }
  return value.convert_to<std::size_t>();
}

Bank Translate(TisaParser::BankContext* bank) {
  ValueType type{
      bank->element()->getText() == "i8" ? IntegerKind::kI8 : IntegerKind::kI32,
      std::nullopt};
  const auto dimensions = bank->UINT();
  if (dimensions.size() == 3) {
    type.tile = TileShape{Dimension(dimensions[0]), Dimension(dimensions[1])};
  }
  return {bank->ID()->getText(), type, Dimension(dimensions.back())};
}

IndexExpr Translate(TisaParser::IndexExprContext* context) {
  IndexExpr terms;
  for (auto* term : context->indexTerm()) {
    if (term->ID()) {
      terms.emplace_back(term->ID()->getText());
    } else {
      terms.emplace_back(Decimal(term->UINT()->getText()));
    }
  }
  return terms;
}

Location Translate(antlr4::tree::TerminalNode* bank,
                   const std::vector<TisaParser::IndexExprContext*>& indices) {
  Location location{bank->getText(), Translate(indices[0]), std::nullopt};
  if (indices.size() == 2) {
    location.end = Translate(indices[1]);
  }
  return location;
}

std::vector<Location> Translate(TisaParser::LocationsContext* context) {
  std::vector<Location> locations;
  if (context) {
    for (auto* location : context->location()) {
      locations.push_back(Translate(location->ID(), location->indexExpr()));
    }
  }
  return locations;
}

Expression Translate(TisaParser::ExpressionContext* context);

Expression Translate(TisaParser::PrimaryContext* primary) {
  if (auto* location = primary->location()) {
    return {Translate(location->ID(), location->indexExpr()), std::nullopt};
  }

  if (auto* literal = primary->tileLiteral()) {
    TileLiteral tile;
    for (auto* row : literal->row()) {
      auto& values = tile.rows.emplace_back();
      for (auto* number : row->integer()) {
        Integer value = Decimal(number->UINT()->getText());
        if (number->getStart()->getText() == "-") {
          value = -value;
        }
        values.push_back(std::move(value));
      }
    }
    return {std::move(tile), std::nullopt};
  }

  Operation operation{primary->op->getText() == "matmul_int"
                          ? Operation::Kind::kMatmul
                          : Operation::Kind::kWrapI32,
                      {}};
  for (auto* arg : primary->expression()) {
    operation.args.push_back(Translate(arg));
  }
  return {std::move(operation), std::nullopt};
}

Expression Translate(TisaParser::ExpressionContext* context) {
  const auto primaries = context->primary();
  if (primaries.size() == 1) {
    return Translate(primaries.front());
  }

  Operation addition{Operation::Kind::kAdd, {}};
  for (auto* primary : primaries) {
    addition.args.push_back(Translate(primary));
  }
  return {std::move(addition), std::nullopt};
}

Instruction Translate(TisaParser::InstructionContext* context) {
  Instruction instruction;
  instruction.name = context->ID()->getText();
  for (auto* operand : context->operand()) {
    instruction.operands.push_back({operand->ID()->getText()});
  }
  for (auto* comparison : context->comparison()) {
    const auto op = comparison->op->getText() == "<"
                        ? Comparison::Op::kLess
                        : Comparison::Op::kLessEqual;
    instruction.preconditions.push_back({Translate(comparison->indexExpr(0)),
                                         Translate(comparison->indexExpr(1)),
                                         op});
  }
  instruction.reads = Translate(context->readList);
  instruction.writes = Translate(context->writeList);
  for (auto* equation : context->equation()) {
    instruction.equations.push_back(
        {Translate(equation->ID(), equation->indexExpr()),
         Translate(equation->expression())});
  }
  return instruction;
}

}  // namespace

Program Parse(std::string_view source) {
  SyntaxErrors errors;
  antlr4::ANTLRInputStream input{std::string(source)};
  TisaLexer lexer(&input);
  lexer.removeErrorListeners();
  lexer.addErrorListener(&errors);

  antlr4::CommonTokenStream tokens(&lexer);
  TisaParser parser(&tokens);
  parser.removeErrorListeners();
  parser.addErrorListener(&errors);

  auto* tree = parser.program();
  Program program;
  for (auto* bank : tree->bank()) {
    program.banks.push_back(Translate(bank));
  }
  for (auto* instruction : tree->instruction()) {
    program.instructions.push_back(Translate(instruction));
  }
  return program;
}

}  // namespace tisa
