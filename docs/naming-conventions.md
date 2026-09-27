# Naming Conventions

Base: [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
Where this project's own rule differs from Google's, the project's rule
wins and the difference is called out explicitly below — nothing here is
silently overridden.

## This project's explicit rules (locked)

- **Functions**: PascalCase — e.g. `SubmitLimitOrder`, `CancelOrder`
- **Function parameters**: `p_` prefix + snake_case — e.g. `p_order_id`,
  `p_price_level`. Applies to every named parameter (free functions,
  methods, lambdas). Call sites never use these names; the prefix marks
  parameters at a glance inside the function body.
- **Local variables**: snake_case — e.g. `order_id`, `price_level` (no
  `p_` prefix)
- **Classes / structs / types**: PascalCase — e.g. `OrderNode`,
  `PriceLevel`, `Book`
- **Curly braces**: always on their own line (Allman style), for every
  block
- **Curly braces**: mandatory for every `if`/`else`/`while`/`for`, even a
  single-statement body — never omitted
- **Indentation**: **2 spaces** per level; never tabs. Enforced by
  [clang-format](https://clang.llvm.org/docs/ClangFormat.html) via
  `.clang-format` at the repo root (`cmake --build build --target format`
  to apply, `clang-format-check` in CI-style verify).
- **Composition preferred over inheritance** wherever a design choice
  exists

## Filled in from Google's guide (not specified above)

- **File names**: lowercase, underscores or dashes, `.h`/`.cc` (Google's
  own extension choice — this project currently uses `.hpp`; keep `.hpp`
  for consistency with the files already produced unless you'd rather
  switch)
- **Class data members**: `variable_name_` — snake_case plus a **mandatory
  trailing underscore** on every class data member (including nested
  helper classes), so members are distinct from `p_` parameters and
  locals.
- **Struct data members**: plain snake_case, no `p_` prefix and no
  trailing underscore — structs are passive aggregates only (`OrderNode`,
  `PriceLevel`, `SubmitResult`, test fixtures). Do not use a struct
  solely to avoid the trailing underscore on class members.
- **Constants / `constexpr`**: `k` prefix + PascalCase — e.g.
  `kMaxOrders`, `kInvalid`
- **Enumerators**: named like constants — `k` prefix + PascalCase
  (`Side::kBuy`, not `Side::Buy`)
- **Namespaces**: lowercase, underscores if multi-word
- **Macros**: avoid entirely where possible; if unavoidable,
  `ALL_CAPS_WITH_UNDERSCORES`
- **Template parameters**: PascalCase, same as type names

## Where this project's rule diverges from Google's default

- **Brace placement**: Google places the opening `{` at the end of the
  line. This project puts every `{` on its own line (Allman). This is an
  explicit, intentional divergence from Google's default — not an
  oversight, and not filled in from the guide.
- Everything else above is additive (filling a gap Google covers and
  this project hadn't specified), not a divergence.

## Known gaps against current code (not yet applied)

- `INVALID` in `book.hpp` should become `kInvalid` under the constant
  rule above.
- `Side::Buy` / `Side::Sell` should become `Side::kBuy` / `Side::kSell`
  under the enumerator rule above.
- Neither has been renamed yet — say the word if you want these applied
  to the existing files.
