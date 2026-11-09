---
name: cpp-standard-library
description: "C++ standard library choices: compiler support, containers, string versus string_view and span, std::format, std::unreachable, std::to_underlying, flat_map, new dependencies. Read when you pick a container or text type, format output, or add a dependency."
disable-model-invocation: true
---

# Standard library

- Use only library features that every compiler the project supports
  provides. Recent features need recent compilers: for example, `<print>`
  needs GCC 14, and `std::flat_map` needs GCC 15.
- `std::array` or `std::vector`, not C arrays. `std::vector` is the default
  container.
- `std::string` owns text; `std::string_view` and `std::span` only refer to
  it. Never keep a view after the object that it refers to is gone.
- `std::format` (or `std::print`, where available), not `<<` chains or
  `printf`.
- `std::unreachable()` for a branch that truly cannot happen, after a
  `switch` that covers every enumerator and has no `default:` label.
  `-Wswitch` (in `-Wall`) then reports a new enumerator that the `switch`
  does not handle.
- `std::to_underlying(e)`, not `static_cast<std::underlying_type_t<E>>(e)`.
- `std::flat_map` or `std::flat_set` when lookups and iteration are much
  more frequent than insertions.
- No new third-party dependencies without a good reason.
