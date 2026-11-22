#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/parser.h"
#include "src/semantics.h"

namespace {

using tisa::Execute;
using tisa::Fault;
using tisa::Parse;
using tisa::State;
using tisa::Validate;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

tisa::ValidatedProgram Load(const char* path) {
  std::ifstream file(path);
  Check(static_cast<bool>(file), path);
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  return Validate(Parse(source));
}

void TestZero(const char* path) {
  const auto program = Load(path);
  const State before{{"acc", {9, -2, 3, 4, 5, 6, -7, 8}}};
  State state = before;

  auto result = Execute(program, "zero", {2}, state);
  Check(result.fault == Fault::kPrecondition && state == before,
        "zero precondition fault must preserve state");

  result = Execute(program, "zero", {-1}, state);
  Check(result.fault == Fault::kInvalidOperands && state == before,
        "zero must reject an invalid u32 operand");

  result = Execute(program, "zero", {}, state);
  Check(result.fault == Fault::kInvalidOperands && state == before,
        "zero must reject the wrong number of operands");

  result = Execute(program, "zero", {0}, state);
  Check(static_cast<bool>(result), result.message);
  Check(state == State{{"acc", {0, 0, 0, 0, 5, 6, -7, 8}}},
        "zero must clear only the selected accumulator");
}

void TestMma(const char* path) {
  const auto program = Load(path);
  const State before{{"spad", {1, 2, -3, 4, 5, -6, 7, 8}},
                     {"acc", {2147483640, -4, 7, -20, 1, 2, 3, 4}}};
  State state = before;

  auto result = Execute(program, "mma", {0, 2, 1}, state);
  Check(result.fault == Fault::kPrecondition && state == before,
        "mma precondition fault must preserve state");

  result = Execute(program, "mma", {0, 0, 1}, state);
  Check(static_cast<bool>(result), result.message);
  State expected = before;
  expected.at("acc") = {-2147483637, 6, 20, 30, 1, 2, 3, 4};
  Check(state == expected, "mma must wrap and preserve all other tiles");

  state = before;
  state.at("acc")[0] = -2147483648;
  result = Execute(program, "mma", {0, 0, 0}, state);
  Check(static_cast<bool>(result), result.message);
  expected.at("acc") = {2147483643, 6, -8, -10, 1, 2, 3, 4};
  Check(state == expected,
        "mma must accept aliasing and wrap negative overflow");
}

void TestSliceCopy(const char* path) {
  const auto program = Load(path);
  const State before{{"buf", {10, 20, 30, 40, 50, 60}}};
  State state = before;

  auto result = Execute(program, "slice_copy", {3, 0, 4}, state);
  Check(result.fault == Fault::kPrecondition && state == before,
        "out-of-bounds destination must fault without a write");

  result = Execute(program, "slice_copy", {0, 4294967295u, 1}, state);
  Check(result.fault == Fault::kPrecondition && state == before,
        "slice bounds arithmetic must not wrap");

  result = Execute(program, "slice_copy", {6, 6, 0}, state);
  Check(static_cast<bool>(result) && state == before,
        "empty slices are valid at the end of a buffer");

  result = Execute(program, "slice_copy", {1, 0, 4}, state);
  Check(static_cast<bool>(result), result.message);
  Check(state == State{{"buf", {10, 10, 20, 30, 40, 60}}},
        "overlapping copy must read the original slice and preserve its frame");
}

void Reject(std::string_view source, std::string_view error) {
  try {
    Validate(Parse(source));
  } catch (const std::invalid_argument& exception) {
    Check(std::string_view(exception.what()).find(error) !=
              std::string_view::npos,
          exception.what());
    return;
  }
  throw std::runtime_error("accepted invalid specification: " +
                           std::string(error));
}

void TestValidation() {
  Reject(R"(
    state { acc: Tile<i32, 1, 1>[2] }
    instruction bad(c: u32) {
      requires c <= 2
      reads none
      writes acc[c]
      ensures acc'[c] = [[0]]
    }
  )",
         "out-of-bounds");

  Reject(R"(
    state { buf: i8[6] }
    instruction bad(src: u32) {
      requires src < 2
      reads buf[0 : 1]
      writes buf[0 : 1]
      ensures buf'[0 : 1] = buf[src : src + 1]
    }
  )",
         "outside declared reads");

  Reject(R"(
    state { buf: i8[6] }
    instruction bad() {
      requires 0 < 1
      reads buf[0 : 1]
      writes buf[0 : 2]
      ensures buf'[0 : 1] = buf[0 : 1]
    }
  )",
         "missing equation targets");

  Reject(R"(
    state { buf: i8[6] }
    instruction bad() {
      requires 0 < 1
      reads buf[0 : 3]
      writes buf[0 : 3]
      ensures buf'[0 : 2] = buf[0 : 2]
      ensures buf'[1 : 3] = buf[1 : 3]
    }
  )",
         "overlapping equation targets");

  Reject(R"(
    state { buf: i8[6] }
    instruction bad() {
      requires 0 < 1
      reads buf[0 : 2]
      writes buf[0 : 2]
      ensures buf'[0 : 2] = buf[0 : 1]
    }
  )",
         "slice length mismatch");

  Reject("state { buf: i8[6] } @", "line 1:");
}

void TestAtomicEquations() {
  const auto program = Validate(Parse(R"(
    state { acc: Tile<i32, 1, 1>[2] }
    instruction swap() {
      requires 0 < 1
      reads acc[0], acc[1]
      writes acc[0], acc[1]
      ensures acc'[0] = acc[1]
      ensures acc'[1] = acc[0]
    }
    instruction fail() {
      requires 0 < 1
      reads none
      writes acc[0], acc[1]
      ensures acc'[0] = [[5]]
      ensures acc'[1] = [[18446744073709551620]]
    }
    instruction wrap() {
      requires 0 < 1
      reads none
      writes acc[0]
      ensures acc'[0] = wrap_i32([[18446744073709551620]])
    }
  )"));

  const State before{{"acc", {7, 31}}};
  State state = before;
  auto result = Execute(program, "fail", {}, state);
  Check(result.fault == Fault::kUnrepresentable && state == before,
        "a late assignment fault must leave every target untouched");

  result = Execute(program, "swap", {}, state);
  Check(static_cast<bool>(result) && state == State{{"acc", {31, 7}}},
        "all equations must read the pre-state");

  result = Execute(program, "wrap", {}, state);
  Check(static_cast<bool>(result) && state == State{{"acc", {4, 7}}},
        "mathematical integers can exceed host integer widths");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 4, "expected paths to all three examples");
    TestZero(argv[1]);
    TestMma(argv[2]);
    TestSliceCopy(argv[3]);
    TestValidation();
    TestAtomicEquations();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
