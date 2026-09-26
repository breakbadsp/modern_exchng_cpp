# Naming Conventions

Base: [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
Where this project's own rule differs from Google's, the project's rule
wins and the difference is called out explicitly below — nothing here is
silently overridden.

## This project's explicit rules (locked)

- **Functions**: PascalCase — e.g. `SubmitLimitOrder`, `CancelOrder`
- **Variables**: snake_case — e.g. `order_id`, `price_level`
- **Classes / structs / types**: PascalCase — e.g. `OrderNode`,
  `PriceLevel`, `Book`
- **Curly braces**: always on their own line (Allman style), for every
  block
- **Curly braces**: mandatory for every `if`/`else`/`while`/`for`, even a
  single-statement body — never omitted
- **Composition preferred over inheritance** wherever a design choice
  exists

## Filled in from Google's guide (not specified above)

- **File names**: lowercase, underscores or dashes, `.h`/`.cc` (Google's
  own extension choice — this project currently uses `.hpp`; keep `.hpp`
  for consistency with the files already produced unless you'd rather
  switch)
- **Class data members**: `variable_name_` — snake_case plus a trailing
  underscore, so a member is visually distinct from a local or a
  parameter at the call site. Classes only.
- **Struct data members**: plain snake_case, no trailing underscore —
  structs are for passive data (Google's struct-vs-class distinction,
  which this project already follows: `OrderNode` and `PriceLevel` are
  structs)
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
