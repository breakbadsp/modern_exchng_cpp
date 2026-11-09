---
name: cpp-principles
description: "C++ core principles and how the cpp-* skills fit together. Read at the start of any C++ writing, editing, or review."
disable-model-invocation: true
---

# C++ principles

The `cpp-*` skills set the rules for new and edited C++ code, including
headers and tests. Do not rename or reformat unrelated existing code.

The base is the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines),
with ideas from [TigerStyle](https://github.com/tigerbeetle/tigerbeetle/blob/main/docs/TIGER_STYLE.md).
Rule IDs such as F.16 and C.20 refer to the Core Guidelines. These skills
differ from both on purpose: errors are values, not exceptions; functions are
PascalCase and parameters have a `p_` prefix; braces are Allman with 2-space
indentation.

A project can add stricter rules, for example in its `AGENTS.md` or its own
C++ standards document. Where a project rule is stricter, it wins.

## Principles

1. **Simple first.** Simple code is easier to get right, and easier to make
   fast after that. Use the obvious solution unless a measurement shows a
   better one.
2. **RAII for every resource.** Every file descriptor, socket, database
   handle, thread, and lock belongs to an object that releases it
   (`cpp-resource-management`).
3. **Errors are values.** A function that can fail returns `std::expected`
   or `std::optional` (`cpp-error-handling`).
4. **Composition before inheritance.** Inherit only for a real "is-a"
   interface with runtime polymorphism (`cpp-classes-and-composition`).
5. **Immutable by default.** Start with `const` or `constexpr`, and make a
   value mutable only when it must change (`cpp-references-pointers-const`).
6. **Let the type system catch mistakes.** Prefer compile-time checks to
   runtime checks.
7. **Put a limit on everything.** Every loop, queue, buffer, cache, and
   recursion over external data has an upper bound (`cpp-hot-path`).
8. **Always say why.** A comment explains why the code is written this way,
   not what it does (`cpp-comments`).
