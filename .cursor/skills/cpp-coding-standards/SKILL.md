---
name: cpp-coding-standards
description: >-
  Reviews modern_exchng_cpp C++ against the project C++ rules already in
  context. Use only when the user asks for a C++ code review.
---

# C++ review

The naming rule and the C++ constraints rule are already in context. Apply those. Do not re-read `docs/naming-conventions.md` or the whole of `docs/cpp-coding-standards.md`. Open one heading in `docs/cpp-coding-standards.md` only to cite an example for a finding.

## Report

```markdown
## C++ review

### Critical
- finding — fix

### Suggestions
- finding — fix
```

Leave legacy names (`INVALID`, `Side::Buy`, `Side::Sell` in `book.hpp`) unless the user asked to rename them.
