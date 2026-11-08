#!/usr/bin/env bash
# Measures the real CLI and config parsers with the existing allocation hooks.
# Usage: scripts/check_allocations.sh [source directory]
set -euo pipefail

source_dir=${1:-$(cd "$(dirname "$0")/.." && pwd)}
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

cat >"$work_dir/probe.cpp" <<'CPP'
// Keep the CLI parser private while testing its real implementation.
#define main LoadgenMain
#include "tools/loadgen.cpp"
#undef main
#include "tests/alloc_count_test.cpp"
#include "config.hpp"

int main(int p_argc, char **p_argv)
{
  const std::span<char *const> args{p_argv, static_cast<std::size_t>(p_argc)};
  g_new_calls.store(0);
  g_count_news.store(true);
  const auto options = ParseOptions(args);
  g_count_news.store(false);
  const auto symbol_allocations = g_new_calls.load();
  if (!options.has_value() || options->symbols_.size() != 12)
  {
    return 1;
  }
  for (std::size_t index = 0; index < options->symbols_.size(); ++index)
  {
    if (options->symbols_[index] != index + 1)
    {
      return 1;
    }
  }
  Options rejected{};
  for (const std::string_view symbols : {"", "1,", ",1", "1,,2", "invalid"})
  {
    if (ApplyOption(rejected, "--symbols", symbols).has_value())
    {
      return 1;
    }
  }

  g_new_calls.store(0);
  g_count_news.store(true);
  const auto invalid = mex::ParseConfig("engine invalid\n");
  g_count_news.store(false);
  const auto error_allocations = g_new_calls.load();
  if (invalid.has_value() || invalid.error().line_ != 1 ||
      invalid.error().reason_ != "core must be an integer in [0, 4294967295], got 'invalid'")
  {
    return 1;
  }
  std::println("CLI symbol allocations: {}; config error allocations: {}",
               symbol_allocations, error_allocations);
  constexpr std::uint64_t kSymbolAllocationBudget = 2;
  constexpr std::uint64_t kErrorAllocationBudget = 1;
  return symbol_allocations <= kSymbolAllocationBudget && error_allocations <= kErrorAllocationBudget
             ? 0
             : 1;
}
CPP

"${CXX:-c++}" -std=c++26 -fcontracts -O2 -Wall -Wextra -Wpedantic -Wconversion \
  -Wsign-conversion -Werror -I"$source_dir" -I"$source_dir/src" \
  "$work_dir/probe.cpp" "$source_dir"/src/{book,config,engine,gateway}.cpp \
  -lgtest -pthread -o "$work_dir/probe"
"$work_dir/probe" --symbols 1,2,3,4,5,6,7,8,9,10,11,12
