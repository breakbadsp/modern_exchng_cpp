# Project C++ constraints

Use the shared `cpp-*` skills in `~/ai/skills` for C++ rules. They include
principles from TigerStyle. The reference copy is [TigerStyle](../prep/TIGER_STYLE.md).

- Keep `.hpp` and `.cpp` file extensions. Use `.clang-format` for layout.
  Run `cmake --build build --target clang-format-check` to check it.
- Return errors as values. Do not use exceptions for project error handling.
  Catch third-party exceptions at the boundary when conversion is necessary.
- After startup, the matching path does not allocate, use virtual calls, or
  use RTTI. Keep hot data contiguous. See [design spec](../prep/design-spec.md).
- Use C++26 contracts for API preconditions and book-structure invariants.
  Invalid input and capacity limits are normal failures, returned as values.
  Do not repeat checks in private helpers without a reason.
- Build contracts with GCC 16 or later and `-fcontracts`. The `bench_book`
  target ignores contract evaluation to measure the book without checks.
