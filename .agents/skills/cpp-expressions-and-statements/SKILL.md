---
name: cpp-expressions-and-statements
description: "C++ expressions and statements: scope, initialization, {} versus (), constexpr values, magic numbers, narrowing, nullptr, casts, signed and unsigned comparison, index, count, and size. Read when you declare variables, convert types, or do integer arithmetic."
disable-model-invocation: true
---

# Expressions and statements

For `const`, references, and pointers on local variables, use
`cpp-references-pointers-const`.

| Rule | Summary |
|------|---------|
| **ES.5** | Keep scopes small; declare a variable where it is first needed |
| **ES.20** | Always initialize |
| **ES.23** | Prefer `{}` initialization (but see below) |
| **ES.45 / Con.5** | No magic numbers. A value known at compile time is `constexpr` with a `k` name: `constexpr std::size_t kMaxDatagramBytes = 65'507;` |
| **ES.46** | No narrowing. Build with `-Wconversion` |
| **ES.47** | `nullptr`, never `0` or `NULL` |
| **ES.48 / ES.50** | No C-style casts; use the named casts or `std::bit_cast`. Never cast away `const` |

`{}` and `()` differ for containers: `std::vector<int>{3}` holds one
element, the value 3, but `std::vector<int>(3)` holds three zeros. Use `()`
when you mean the size.

Mixing signed and unsigned values is a frequent bug. Use `std::cmp_less`
and the other comparison functions in `<utility>`, or make the conversion
explicit with a `static_cast` and a reason.

**Index, count, and size are different** (TigerStyle). An index starts at 0
and a count starts at 1, so a conversion between them needs a `+ 1`. A size
is a count multiplied by a unit. Name variables so that the difference is
clear (`slot_index`, `slot_count`, `buffer_bytes`). When integer division
rounds, make it clear in which direction, and why.
