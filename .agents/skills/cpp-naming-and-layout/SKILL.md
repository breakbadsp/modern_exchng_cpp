---
name: cpp-naming-and-layout
description: "C++ naming and layout: functions, p_ parameters, locals, types, trailing _ members, k constants, enumerators, namespaces, macros, files, Allman braces, indentation, units in names, abbreviations. Read when you name or format anything in C++."
disable-model-invocation: true
---

# Naming and layout

These are the user's own conventions. For naming that they do not cover,
use the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
Where the two differ (the `p_` prefix, the trailing `_` on struct members,
enumerator names, Allman braces), these rules win.

| Kind | Convention | Example |
|------|------------|---------|
| Functions and methods | PascalCase | `DecodePacket`, `CancelOrder` |
| Function parameters | `p_` + snake_case, for every named parameter of free functions, methods, and lambdas | `p_packet`, `p_order_id` |
| Local variables | snake_case, no `p_` | `thread_count` |
| Classes, structs, aliases, template parameters | PascalCase | `Engine`, `Record`, `TNumber` |
| Class and struct data members | snake_case + trailing `_`, also in nested classes and test fixtures | `order_id_`, `last_seen_` |
| Constants and `constexpr` variables | `k` + PascalCase | `kMaxDatagramBytes` |
| Enumerators | PascalCase, no `k`; the first one has an explicit initializer, usually `= 0` | `Side::Buy`, `Debug = 0` |
| Namespaces | lowercase, underscores between words | `order_book` |
| Macros | avoid; if needed, `ALL_CAPS` with underscores | `PROJECT_VERSION` |
| Files | lowercase, underscores or dashes; keep the project's extensions (`.hpp` / `.cpp` or `.h` / `.cc`) | `order_book.hpp` |

- **Braces:** Allman. Every `{` and `}` is on its own line.
- **Braces are mandatory** on every `if`, `else`, `for`, and `while` body,
  also when the body is one statement.
- **Indentation:** 2 spaces, never tabs. Run the project's formatter, and
  keep its configuration consistent with these rules.
- **Units go last in a name** (TigerStyle): `interval_ms`, `latency_ms_max`,
  and `latency_ms_min`. Related names then align and sort together.
- **No abbreviations** unless they are standard in the domain (`tid`, `pid`,
  `fd`, `io`).
- **Structs are passive aggregates.** If a type has an invariant to protect,
  make it a `class`. The trailing `_` on struct members keeps them
  consistent with class members. It does not mean that the struct hides
  anything.
