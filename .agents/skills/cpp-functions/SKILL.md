---
name: cpp-functions
description: "C++ function design: one operation, short functions, constexpr, pure functions, return values instead of out parameters, parameter count, branching, explicit options, recursion. Read when you write or change a function."
disable-model-invocation: true
---

# Functions

For the type of each parameter (value, reference, or pointer; `const` or
not), use `cpp-references-pointers-const`.

| Rule | Summary |
|------|---------|
| **F.2** | One function, one logical operation |
| **F.3** | Keep functions short: less than about 70 lines (TigerStyle). Splitting a long function usually shows the parts that it was made of |
| **F.4** | If it can run at compile time, make it `constexpr` |
| **F.8** | Prefer pure functions; keep leaf functions pure |
| **F.20 / F.21** | Return values, not output parameters. Return a struct for several values |
| **F.43** | Never return a pointer or reference to a local |
| **I.23** | Few parameters. Two adjacent parameters of the same type are easy to swap at the call site; use a struct or a distinct type |

- **Push `if`s up and `for`s down.** Keep the branches in the parent
  function, and move the straight-line work into helpers.
- **Split compound conditions** into nested `if`s when that makes it
  clearer which cases are handled. State conditions positively: write
  `index < count`, not `!(index >= count)`.
- **Pass options to library calls explicitly** at the call site (flags,
  timeouts, `O_CLOEXEC`). Do not rely on defaults that can change.

```cpp
struct RateWindow
{
  double start_s_{};
  double end_s_{};
};

// The two doubles travel together in a struct, so a caller cannot swap
// start and end.
[[nodiscard]] double Rate(std::uint64_t p_delta, RateWindow p_window);
```

Do not use:

- a return type of `T&&`, or `const T` by value (a `const` return value
  cannot be moved from);
- C-style variadics (`va_arg`). Use `std::format` or a variadic template;
- recursion over data from outside the process, because its depth has no
  limit. Use a loop with an explicit limit. A recursive parser must carry a
  depth limit.
