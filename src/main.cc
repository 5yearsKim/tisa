#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "parser.h"
#include "semantics.h"

namespace {

template <typename T>
T ParseNumber(std::string_view text) {
  T value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid integer '" + std::string(text) + "'");
  }
  return value;
}

std::vector<std::int32_t> ParseBank(std::string_view text) {
  std::vector<std::int32_t> values;
  if (text.empty()) {
    return values;
  }
  for (;;) {
    const auto comma = text.find(',');
    values.push_back(ParseNumber<std::int32_t>(text.substr(0, comma)));
    if (comma == std::string_view::npos) {
      return values;
    }
    text.remove_prefix(comma + 1);
  }
}

int Run(int argc, char** argv) {
  if (argc < 4) {
    throw std::invalid_argument(
        "usage: tisa FILE INSTRUCTION [OPERAND ...] -- BANK=v1,v2,... "
        "[BANK=...]");
  }

  std::filesystem::path path(argv[1]);
  // Bazel runs the binary from its runfiles tree.
  if (const char* cwd = std::getenv("BUILD_WORKING_DIRECTORY");
      cwd && path.is_relative()) {
    path = std::filesystem::path(cwd) / path;
  }
  std::ifstream file(path);
  if (!file) {
    throw std::invalid_argument("cannot open '" + std::string(argv[1]) + "'");
  }
  const std::string source((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
  const auto program = tisa::Validate(tisa::Parse(source));

  std::vector<tisa::Integer> operands;
  int arg = 3;
  for (; arg < argc && std::string_view(argv[arg]) != "--"; ++arg) {
    operands.emplace_back(ParseNumber<std::uint32_t>(argv[arg]));
  }
  if (arg == argc) {
    throw std::invalid_argument("expected '--' before initial state");
  }

  tisa::State state;
  for (++arg; arg < argc; ++arg) {
    const std::string_view input(argv[arg]);
    const auto equals = input.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      throw std::invalid_argument("expected BANK=VALUE,... in initial state");
    }
    auto name = std::string(input.substr(0, equals));
    if (!state.emplace(name, ParseBank(input.substr(equals + 1))).second) {
      throw std::invalid_argument("duplicate initial state bank '" + name +
                                  "'");
    }
  }
  const auto result = tisa::Execute(program, argv[2], operands, state);
  if (!result) {
    throw std::invalid_argument(result.message);
  }

  for (const auto& bank : program.program().banks) {
    std::cout << bank.name << '=';
    const auto& values = state.at(bank.name);
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i != 0) {
        std::cout << ',';
      }
      std::cout << values[i];
    }
    std::cout << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "tisa: " << error.what() << '\n';
    return 1;
  }
}
