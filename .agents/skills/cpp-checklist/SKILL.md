---
name: cpp-checklist
description: "C++ completion checklist across all cpp-* skills. Read before you declare C++ work done, and when you review C++ code."
disable-model-invocation: true
---

# Checklist

Before you declare C++ work done:

- [ ] The project's format check and tests pass, with warnings as errors
- [ ] Functions that can fail return `std::expected`; missing values are
      `std::optional`; both are `[[nodiscard]]`
- [ ] No new `throw`, `try`, or `catch` outside the cases in
      `cpp-error-handling`
- [ ] No `.value()` on `std::expected` or `std::optional`
- [ ] Assertions for programmer errors; never assertions on external input
- [ ] Every loop, queue, and cache over external data has a limit
- [ ] No allocation on the hot path, except at points the project permits
- [ ] RAII for every handle; no naked `new` / `delete`; a raw pointer never
      owns
- [ ] Rule of Zero, or all five special members
- [ ] Single-argument constructors are `explicit`; leaf classes are `final`
- [ ] Each parameter and local uses a form from
      `cpp-references-pointers-const`, or has a comment that says why not
- [ ] Every variable is initialized; `const` by default; member functions
      are `const` unless they change the object; no magic numbers
- [ ] No narrowing, no C-style casts, `nullptr`
- [ ] Locks are RAII and named; no `detach`
- [ ] PascalCase functions, `p_` parameters, members with a trailing `_`,
      `k` constants, PascalCase enumerators with the first one initialized
- [ ] Allman braces, braces on every branch, 2-space indentation
