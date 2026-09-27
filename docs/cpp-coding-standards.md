# C++ Coding Standards

Human reference. Cursor enforces a short subset in `.cursor/rules/`; agents should open one heading here, not this whole file.

Modern C++ (C++26) coding standards for this
project. Built on the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines),
but diverges from them in three deliberate ways: **no exceptions**
(Result/Option error handling instead), **this project's own naming
convention** (not the Guidelines' `snake_case` default), and an added
emphasis on **low-latency, high-throughput** code paths. Every
divergence from the source guidelines is called out explicitly — this
document doesn't silently disagree with its source.

See [naming-conventions.md](naming-conventions.md) for the full naming
reference; this file summarizes it and applies it in every example below.

## When to Use

- Writing new C++ code (classes, functions, templates)
- Reviewing or refactoring existing C++ code
- Making architectural decisions in C++ projects, especially
  performance-sensitive ones
- Choosing between language features (`enum` vs `enum class`, exceptions
  vs `std::expected`, inheritance vs composition)

### When NOT to Use

- Non-C++ projects
- Legacy C codebases that cannot adopt modern C++ features
- Any codebase that already commits to exceptions throughout — don't
  half-migrate; the no-exceptions rule below is a project-wide decision,
  not a per-file one

## Cross-Cutting Principles

1. **RAII everywhere** — bind resource lifetime to object lifetime.
   Applies with or without exceptions in the picture.
2. **No exceptions — Result/Option instead** — `std::expected<T, E>` for
   fallible operations, `std::optional<T>` for values that may be
   absent. See "Error Handling" below; this is the biggest divergence
   from the stock Core Guidelines.
3. **Simplicity is a first-order goal** — not a tiebreaker applied after
   the "real" design work. See "Simplicity" below.
4. **Composition before inheritance** — inheritance is for genuine
   polymorphic "is-a" interfaces only, not code reuse. See "Classes &
   Composition" below.
5. **Immutability by default** — start with `const`/`constexpr`;
   mutability is the exception, stated explicitly when needed.
6. **Type safety** — use the type system to catch errors at compile
   time rather than runtime.
7. **Design for the hot path** — on performance-critical code, prefer
   compile-time dispatch, contiguous data, and predictable branches over
   generality. See "Low-Latency & High-Throughput" below.
8. **This project's naming and layout, always** — PascalCase
   functions/types, snake_case variables, Allman braces, mandatory braces
   on every conditional/loop, **2-space indentation** (no tabs). See
   [naming-conventions.md](naming-conventions.md). Layout is enforced by
   `.clang-format` at the repo root.

## Naming (summary — full rules in [naming-conventions.md](naming-conventions.md))

- Functions: PascalCase — `SubmitLimitOrder`, `CancelOrder`
- Function parameters: `p_` + snake_case — `p_order_id`, `p_price`
- Locals: snake_case — `order_id`, `price_level`
- Struct data members: snake_case + trailing `_` — `order_id_`, `filled_qty_`
- Class data members: snake_case with a **mandatory** trailing
  underscore — `pool_`, `free_list_`
- Classes / structs / types / template parameters: PascalCase —
  `OrderNode`, `PriceLevel`
- Constants / `constexpr`: `k` + PascalCase — `kMaxOrders`
- Enumerators: PascalCase, no `k` — `Side::Buy`; first enumerator
  explicitly initialized (`Buy = 0`)
- Namespaces: lowercase, underscores if multi-word
- Curly braces: **always on their own line** (Allman) — a deliberate
  divergence from the Core Guidelines' and Google's own end-of-line
  brace convention
- Curly braces: **mandatory** for every `if`/`else`/`while`/`for`, even
  single-statement bodies
- Indentation: **2 spaces** per level, no tabs — run
  `cmake --build build --target format` or `clang-format -i` on touched
  files; `clang-format-check` fails if sources drift from `.clang-format`

## Simplicity

Simple code is correct code, and simple code is easier to make fast —
in that order. Prefer the boring solution until you have a measured
reason not to.

- Don't introduce a design pattern (Visitor, Strategy, Observer, a
  plugin/factory framework) unless the complexity it removes is bigger
  than the complexity it adds. Most of the time, a function or a plain
  `if`/`switch` is enough.
- Don't add a layer of abstraction (an interface with one
  implementation, a config system with one config) for hypothetical
  future needs. Add it when the second real need shows up.
- Prefer flat control flow to deep nesting. An early `return` on an
  error/edge case beats wrapping the main logic in another `if` level.
- Keep functions short (see F.3 below) — a long function is usually
  several functions that haven't been separated yet.
- Avoid template metaprogramming unless `constexpr` genuinely can't do
  the job (T.120 below). Clever template code is a maintenance tax paid
  by everyone who touches the file after you.
- When a design choice is a toss-up between "clever and fast-looking"
  and "obvious and measured", pick obvious, measure it, and only reach
  for clever if the measurement says you need to (Per.1, Per.2, Per.6
  below still apply).

## Philosophy & Interfaces (P.*, I.*)

| Rule | Summary |
|------|---------|
| **P.1** | Express ideas directly in code |
| **P.3** | Express intent |
| **P.4** | Prefer static type safety over runtime checks |
| **P.5** | Prefer compile-time checking to run-time checking |
| **P.8** | Don't leak any resources |
| **P.10** | Prefer immutable data to mutable data |
| **I.1** | Make interfaces explicit |
| **I.2** | Avoid non-const global variables |
| **I.4** | Make interfaces precisely and strongly typed |
| **I.11** | Never transfer ownership by a raw pointer or reference |
| **I.23** | Keep the number of function arguments low |

```cpp
// P.10 + I.4: immutable, strongly typed interface
struct Temperature
{
    double kelvin;
};

Temperature Boil(const Temperature& water);
```

```cpp
// Weak interface: unclear ownership, unclear units
double Boil(double* temp);

// Non-const global variable — I.2 violation
int g_counter = 0;
```

## Functions (F.*)

| Rule | Summary |
|------|---------|
| **F.1** | Package meaningful operations as carefully named functions |
| **F.2** | A function should perform a single logical operation |
| **F.3** | Keep functions short and simple |
| **F.4** | If a function could run at compile time, declare it `constexpr` |
| **F.8** | Prefer pure functions |
| **F.16** | Trivially-copyable and cheap-to-copy "in" parameters by value (value semantics); larger types by reference. A pointer only when null is a valid state |
| **F.20** | For "out" values, prefer return values to output parameters |
| **F.21** | To return multiple "out" values, prefer a struct |
| **F.43** | Never return a pointer or reference to a local object |

```cpp
// F.16: trivially-copyable and cheap-to-copy → pass by value (value semantics).
// A reference is never null; a pointer only when null is a valid state.
void Print(int x);
void Analyze(const std::string& data);  // large: const reference
void Transform(std::string s);  // sink: by value, will move
void Bump(Counter c);  // cheap handle: value semantics
void Observe(const Widget* widget);  // null is a valid "nothing to draw" state

// F.20 + F.21: return a struct, not output parameters
struct ParseResult
{
    std::string token;
    int position;
};

ParseResult Parse(std::string_view input);  // GOOD

void Parse(std::string_view input, std::string& token, int& pos);  // avoid
```

```cpp
// F.4 + F.8: pure, constexpr where possible
constexpr int Factorial(int n) noexcept
{
    return (n <= 1) ? 1 : n * Factorial(n - 1);
}

static_assert(Factorial(5) == 120);
```

### Anti-patterns

- Returning `T&&` from functions
- C-style variadics (`va_arg`)
- Capturing by reference in a lambda handed to another thread
- Returning `const T` by value — inhibits move semantics

## Classes & Composition (C.*)

**Composition before inheritance, as a default, not a special case.**
Reach for inheritance only when the relationship is genuinely "is-a"
and you need runtime polymorphism through a common interface. Reusing
another class's code is not, by itself, a reason to inherit from it —
that's what a member (composition) is for.

| Rule | Summary |
|------|---------|
| **C.2** | `class` if an invariant exists; `struct` if members vary independently |
| **C.9** | Minimize exposure of members |
| **C.20** | Rule of Zero — avoid defining default operations if you can |
| **C.21** | Rule of Five — if you define/delete one, handle all five |
| **C.35** | Base class destructor: public virtual, or protected non-virtual |
| **C.41** | A constructor should produce a fully initialized object |
| **C.46** | Single-argument constructors are `explicit` |
| **C.128** | Virtual overrides: exactly one of `virtual`, `override`, `final` |
| — | Prefer composition; restrict inheritance to genuine "is-a" cases |
| — | All inheritance is `public`. Want private inheritance? Use a member instead |
| — | Mark a class `final` when you don't intend it as a base |

### Rule of Zero

```cpp
// C.20: let the compiler generate the special members
struct Employee
{
    std::string name;
    std::string department;
    int id;
};
```

### Composition over inheritance — a concrete before/after

```cpp
// AVOID: inheriting from Formatter purely to reuse FormatLine().
// TimestampedLogger and Formatter have no "is-a" relationship — this
// is code reuse wearing an inheritance costume.
class Formatter
{
public:
    std::string FormatLine(std::string_view text);
};

class TimestampedLogger : public Formatter  // wrong tool for the job
{
public:
    void Log(std::string_view text);
};
```

```cpp
// PREFER: TimestampedLogger HAS-A Formatter. No inheritance relationship
// to reason about, no risk of slicing, no accidental exposure of
// Formatter's public interface through Logger.
class TimestampedLogger
{
public:
    void Log(std::string_view text)
    {
        std::println("[{}] {}", Timestamp(), formatter_.FormatLine(text));
    }

private:
    Formatter formatter_;
};
```

### Where inheritance is the right call: genuine polymorphism

```cpp
// C.35 + C.128: virtual destructor, override, final on the leaf
class Shape
{
public:
    virtual ~Shape() = default;
    virtual double Area() const = 0;
};

class Circle final : public Shape  // final: not meant to be a base itself
{
public:
    explicit Circle(double r) : radius_(r)
    {
    }

    double Area() const override
    {
        return 3.14159 * radius_ * radius_;
    }

private:
    double radius_;
};
```

Note: on a hot path, prefer compile-time dispatch (templates,
`if constexpr`) over this kind of runtime polymorphism — see
"Low-Latency & High-Throughput" below. Virtual dispatch here is the
right tool for a genuinely open set of shapes decided at runtime; it's
the wrong tool inside a matching loop.

### Anti-patterns

- Calling virtual functions from constructors/destructors
- `memset`/`memcpy` on non-trivial types
- Making data members `const` or references — suppresses move/copy
- Inheriting to reuse code rather than to model "is-a"

## Resource Management (R.*)

| Rule | Summary |
|------|---------|
| **R.1** | Manage resources automatically via RAII |
| **R.3** | A raw pointer (`T*`) is non-owning, and only when null is a valid state. Otherwise use a reference |
| **R.5** | Prefer scoped objects; don't heap-allocate unnecessarily |
| **R.10** | Avoid `malloc()`/`free()` |
| **R.11** | Avoid calling `new`/`delete` explicitly |
| **R.20** | Use `unique_ptr`/`shared_ptr` to represent ownership |
| **R.21** | Prefer `unique_ptr` over `shared_ptr` unless sharing is needed |

```cpp
// R.11 + R.20 + R.21: RAII with smart pointers
auto widget = std::make_unique<Widget>("config");
auto cache = std::make_shared<Cache>(1024);

// R.3: a reference is never null. A raw pointer is a non-owning observer
// only when null is a valid state.
void Render(const Widget& w)
{
    w.Draw();
}

void RenderIfPresent(const Widget* w)
{
    if (w)
    {
        w->Draw();
    }
}
```

RAII does not depend on exceptions — a destructor still runs
deterministically on scope exit regardless of whether the codebase uses
exceptions or `std::expected`. Keep using RAII for every resource
(file handles, sockets, locks, memory) exactly as before; only the
*error-reporting* mechanism changes (see below), not the cleanup
mechanism.

```cpp
// RAII construction that can fail: no throwing constructor. A private
// constructor + a static factory returning std::expected is the pattern
// (this is also the Core Guidelines' own recommended alternative to a
// throwing constructor — we're just pairing it with std::expected
// instead of an exception).
class FileHandle
{
public:
    static std::expected<FileHandle, std::error_code> Create(const std::string& path)
    {
        std::FILE* handle = std::fopen(path.c_str(), "r");
        if (handle == nullptr)
        {
            return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
        }
        return FileHandle(handle);
    }

    ~FileHandle()
    {
        if (handle_ != nullptr)
        {
            std::fclose(handle_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    FileHandle(FileHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    FileHandle& operator=(FileHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                std::fclose(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

private:
    explicit FileHandle(std::FILE* handle) : handle_(handle)
    {
    }

    std::FILE* handle_;
};
```

### Anti-patterns

- Naked `new`/`delete`
- `malloc()`/`free()` in C++ code
- `shared_ptr` where `unique_ptr` suffices

## Expressions & Statements (ES.*)

| Rule | Summary |
|------|---------|
| **ES.5** | Keep scopes small |
| **ES.20** | Always initialize an object |
| **ES.23** | Prefer `{}` initializer syntax |
| **ES.25** | Declare objects `const`/`constexpr` unless mutation is intended |
| **ES.45** | Avoid magic constants; use named constants |
| **ES.46** | Avoid narrowing/lossy arithmetic conversions |
| **ES.47** | `nullptr`, never `0` or `NULL` |
| **ES.48** | Avoid casts; prefer brace-init or the named C++ casts |
| **ES.50** | Don't cast away `const` |

```cpp
// ES.20 + ES.23 + ES.25: always initialize, prefer {}, default to const
const int kMaxRetries{3};
const std::string name{"widget"};
const std::vector<int> primes{2, 3, 5, 7, 11};
```

### Anti-patterns

- Uninitialized variables
- `0`/`NULL` instead of `nullptr`
- C-style casts instead of `static_cast`/`const_cast`/`std::bit_cast`
- Magic numbers with no named constant
- Mixing signed and unsigned arithmetic

## Error Handling — Result & Option, No Exceptions

**This project does not use C++ exceptions.** No `throw`, no `try`/
`catch`, no exception types anywhere in project code, including
third-party integration points — wrap them at the boundary instead of
letting exceptions cross into this codebase. This is a deliberate,
project-wide, latency-motivated decision (exception unwinding has
unpredictable cost and doesn't pair with the no-allocation, hot-path
guidance elsewhere in this project) — see "Low-Latency &
High-Throughput" below for why, not just what.

Instead, this project follows the same shape as Rust's `Result<T, E>`
and `Option<T>`, using the C++23 standard types built for exactly this:

- **`std::expected<T, E>`** (C++23, `<expected>`) — the direct
  equivalent of Rust's `Result<T, E>`. Holds either a value `T` or an
  error `E`.
- **`std::optional<T>`** (C++17, monadic operations added in C++23) —
  the direct equivalent of `Option<T>`. Holds either a value or nothing.

```cpp
#include <expected>

enum class ParseError
{
    kEmptyInput,
    kInvalidFormat,
};

std::expected<int, ParseError> ParseCount(std::string_view input)
{
    if (input.empty())
    {
        return std::unexpected(ParseError::kEmptyInput);
    }
    // ... parse ...
    return 42;
}
```

Both types got monadic operations in C++23 — `and_then`, `or_else`,
`transform` — for chaining without a manual check-and-branch at every
step, the same shape as Rust's combinator chains:

```cpp
std::expected<double, std::string> SafeDivide(double a, double b)
{
    if (b == 0.0)
    {
        return std::unexpected("division by zero");
    }
    return a / b;
}

// Chain without an intermediate check at every step
auto result = SafeDivide(10.0, 2.0)
    .and_then([](double r) { return SafeDivide(r, 0.5); })
    .transform([](double r) { return r * 2.0; });
```

```cpp
// std::optional monadic chain — the Option<T> equivalent
std::optional<int> FindUserId(std::string_view name);

auto greeting = FindUserId("sachin")
    .transform([](int id) { return std::format("Hello, user {}", id); })
    .value_or("Hello, guest");
```

### Rules

- Every function that can fail returns `std::expected<T, E>`, not `T`
  with an out-parameter error code and not a thrown exception.
- Every function returning `std::expected`/`std::optional` is marked
  `[[nodiscard]]` — this is the compile-time enforcement that replaces
  "the compiler makes you handle the exception or crash." An ignored
  `Result` should be a compile warning/error, the same way an unused
  `Result` is a clippy lint in Rust.
- Use `and_then`/`transform`/`or_else` to chain fallible operations
  instead of a pyramid of `if (!result) { return ...; }` checks, where
  it actually reads more clearly — don't force the monadic style where
  a plain early-return `if` is clearer (see "Simplicity" above).
- RAII still handles cleanup (see "Resource Management" above) —
  Result/Option replaces exceptions for *signaling* failure, not RAII
  for *cleaning up* after it.
- Destructors, deallocation, and swap must never fail — this held true
  under exceptions and holds true here; a destructor has nowhere to
  report a `std::expected` to anyway.
- If you're integrating a third-party library that throws, catch at the
  narrowest possible boundary and convert to `std::expected` there. Do
  not let an exception propagate past that boundary.
- `noexcept`: with exceptions off project-wide, mark functions
  `noexcept` unconditionally where it's accurate — it documents intent
  and can still help codegen, even though nothing in project code will
  ever throw across a `noexcept` boundary. Build with `-fno-exceptions`
  to make this a compiler-enforced guarantee, not a convention.

```cpp
[[nodiscard]] std::expected<int, ParseError> ParseCount(std::string_view input);

// Ignoring this won't compile silently — matches Rust's #[must_use] Result
auto result = ParseCount(raw_input);
```

### Anti-patterns

- `throw`, `try`, `catch` anywhere in project code
- A `bool`/error-code out-parameter instead of `std::expected`
- A `std::expected`-returning function without `[[nodiscard]]`
- Letting a third-party exception cross into project code unconverted

## Constants & Immutability (Con.*)

| Rule | Summary |
|------|---------|
| **Con.1** | Objects immutable by default |
| **Con.2** | Member functions `const` by default |
| **Con.3** | Pass pointers/references to `const` by default |
| **Con.4** | `const` for values fixed after construction |
| **Con.5** | `constexpr` for values computable at compile time |

```cpp
class Sensor
{
public:
    explicit Sensor(std::string id) : id_(std::move(id))
    {
    }

    const std::string& Id() const
    {
        return id_;
    }

    double LastReading() const
    {
        return reading_;
    }

    void Record(double value)
    {
        reading_ = value;
    }

private:
    const std::string id_;  // Con.4: never changes after construction
    double reading_{0.0};
};

// Con.5: compile-time constants, k-prefixed per this project's convention
constexpr double kPi = 3.14159265358979;
constexpr int kMaxSensors = 256;
```

## Concurrency & Parallelism (CP.*)

This project's matching core is single-threaded by design (see
`design-spec.md`) — these rules apply to any surrounding
infrastructure (feed handlers, logging, admin interfaces) that isn't.

| Rule | Summary |
|------|---------|
| **CP.2** | Avoid data races |
| **CP.3** | Minimize explicit sharing of writable data |
| **CP.8** | Don't use `volatile` for synchronization |
| **CP.20** | Use RAII locks, never plain `lock()`/`unlock()` |
| **CP.21** | `std::scoped_lock` for multiple mutexes |
| **CP.22** | Never call unknown code while holding a lock |
| **CP.44** | Always name your `lock_guard`/`unique_lock` |

```cpp
class ThreadSafeQueue
{
public:
    void Push(int value)
    {
        std::lock_guard<std::mutex> lock(mutex_);  // CP.44: named
        queue_.push(value);
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<int> queue_;
};
```

### False sharing (added — not in the original Core Guidelines)

Two atomics/variables written by different threads but sitting on the
same cache line silently serialize those threads through cache
coherency traffic, with no data race and no compiler warning to catch
it. Pad or `alignas(64)` any variable that's written frequently by one
thread while a neighboring field is written by another.

```cpp
struct alignas(64) PaddedCounter
{
    std::atomic<uint64_t> value{0};
    // implicit padding to the next 64-byte boundary
};
```

### Anti-patterns

- `volatile` for synchronization — it's for hardware I/O only
- Detaching threads
- Unnamed lock guards (destroyed immediately, lock never actually held)
- Holding a lock while calling into unknown/callback code
- Two hot-path variables from different threads sharing a cache line

## Templates & Generic Programming (T.*)

| Rule | Summary |
|------|---------|
| **T.10** | Constrain template arguments with concepts |
| **T.11** | Use standard concepts where possible |
| **T.43** | Prefer `using` over `typedef` |
| **T.120** | Template metaprogramming only when genuinely needed |

```cpp
#include <concepts>

template <std::integral T>
T Gcd(T a, T b)
{
    while (b != 0)
    {
        a = std::exchange(b, a % b);
    }
    return a;
}

template <typename T>
concept Serializable = requires(const T& t)
{
    { t.Serialize() } -> std::convertible_to<std::string>;
};
```

## Standard Library (SL.*) — including C++23 additions

| Rule | Summary |
|------|---------|
| **SL.con.1** | Prefer `std::array`/`std::vector` over C arrays |
| **SL.con.2** | Prefer `std::vector` by default |
| **SL.str.1** | `std::string` owns character sequences |
| **SL.str.2** | `std::string_view` observes character sequences |
| — | Prefer `std::flat_map`/`std::flat_set` (C++23) when lookups and iteration dominate over insertion — matches this project's own sorted-vector price-level design in `book.hpp` |
| — | `std::mdspan` (C++23) for a non-owning multi-dimensional view — zero-allocation, useful for market-data buffers laid out as a 2D grid |
| — | `std::print`/`std::println` (C++23) over `<<` chains — faster, type-safe, no stream-state footguns |
| — | `std::unreachable()` (C++23) to mark a genuinely impossible branch, instead of a comment |
| — | `std::to_underlying(e)` (C++23) instead of `static_cast<std::underlying_type_t<E>>(e)` |

```cpp
#include <flat_map>
#include <print>

// C++23: flat_map is a drop-in replacement for std::map when the access
// pattern favors lookup/iteration over insertion — same shape as the
// deliberate choice already made for bids_/asks_ in book.hpp.
std::flat_map<int64_t, PriceLevel> levels;

std::println("best bid: {}", best_bid_price);
```

## Enumerations (Enum.*)

| Rule | Summary |
|------|---------|
| **Enum.1** | Prefer enumerations over macros |
| **Enum.3** | `enum class` over plain `enum` |

```cpp
// This project's convention: k-prefixed PascalCase enumerators
enum class Color
{
    kRed,
    kGreen,
    kBlue,
};

enum class LogLevel
{
    kDebug,
    kInfo,
    kWarning,
    kError,
};
```

## Low-Latency & High-Throughput

Added section — not in the original Core Guidelines, which are
latency-agnostic. This is where this project's performance targets
(100k-500k+ orders/sec, single-threaded, no allocation once running)
turn into concrete code-level rules.

- **Compile-time dispatch over runtime dispatch on the hot path.**
  A virtual call costs an indirect branch and defeats some inlining;
  a template/`if constexpr` resolves at compile time with zero runtime
  cost. Reserve `virtual` for genuinely open-ended, rarely-hit code
  (see the `Shape`/`Circle` example above) — never for the matching
  loop itself.
- **No heap allocation once the system is running** — already locked
  project-wide in `design-spec.md`; restated here because it's a
  language-level habit as much as an architectural one. Prefer
  fixed-capacity containers (`std::array`, a preallocated `std::vector`
  with `.reserve()` honored by a hard capacity check) over anything that
  can silently grow.
- **Contiguous data over pointer-chasing.** `std::vector<Order>` beats
  `std::vector<std::unique_ptr<Order>>` for anything walked in a loop —
  the indirection defeats hardware prefetching. This is the same
  reasoning already applied to `pool_` and the price-level vectors.
- **`[[likely]]`/`[[unlikely]]`** (C++20) on the hot/cold branch of a
  frequently-executed `if` — helps icache layout and branch prediction
  on the paths that actually run millions of times a second. Don't
  sprinkle these everywhere; they're for measured hot spots, not a
  default.
- **`alignas`/cache-line awareness** for anything touched every
  iteration of a hot loop, and to avoid false sharing across threads
  (see Concurrency above).
- **Avoid RTTI (`dynamic_cast`, `typeid`) on the hot path** — same
  category of cost as virtual dispatch, for the same reason: it's a
  runtime type check where a compile-time one should exist instead.
- **Measure before optimizing, still** — none of the above licenses
  guessing. Per.1/Per.2/Per.6 below still apply: profile first, then
  apply the specific technique the profile justifies.

| Rule | Summary |
|------|---------|
| **Per.1** | Don't optimize without a reason |
| **Per.2** | Don't optimize prematurely |
| **Per.6** | Don't claim a performance win without measuring it |
| **Per.11** | Move computation from runtime to compile time |
| **Per.19** | Access memory predictably |

```cpp
// Per.19: contiguous, predictable access
std::vector<Point> points;                            // GOOD
std::vector<std::unique_ptr<Point>> indirect_points;   // BAD: pointer chasing

// C++20 branch hints for a measured hot/cold split
if (order.quantity == 0) [[unlikely]]
{
    return HandleFullyFilled(order);
}
```

## C++26 contracts

The book uses C++26 contracts (`pre`, `post`, `contract_assert`) at API
boundaries and for book-structure bugs (bad order id, unlinking a node
that is not on the level). Do not put a contract on a helper that only
repeats the assignments in the body, and do not re-check `Side` on every
private method. GCC 16 implements them with `-fcontracts`. The default
semantic is `enforce`: a failed check terminates. Clang 22 does not
parse the syntax, so configure with GCC.

A contract failure is a bug. A zero quantity and a full pool are normal
rejects and stay return values. Cancel of an id that is not resting
returns false. `Order` requires an id inside the pool.

`bench_book` builds with `-fcontract-evaluation-semantic=ignore` so the
measurement is the book, not the checks. Reflection (`-freflection`)
stays unused.

Separately: the "Profiles" proposal (which would have added automatic
signed/unsigned conversion safety, `std::narrow<T>`) was **voted out**
of C++26 in March 2026 and deferred to C++29 with no ship date — it
isn't a compiler-lag issue, it simply isn't in the standard. Continue
using `-Wconversion -Wsign-conversion -Werror` for this instead.

## Source Files (SF.*)

| Rule | Summary |
|------|---------|
| **SF.7** | No `using namespace` at global scope in a header |
| **SF.8** | Include guards (or `#pragma once`) on every header |
| **SF.11** | Headers should be self-contained |

```cpp
#pragma once

#include <string>
#include <vector>

namespace hft
{

class Widget
{
public:
    explicit Widget(std::string name);
    const std::string& Name() const;

private:
    std::string name_;
};

}  // namespace hft
```

## Quick Reference Checklist

Before marking C++ work complete:

- [ ] No `new`/`delete` — smart pointers or RAII (R.11)
- [ ] No heap allocation reachable once the system is running
- [ ] Objects initialized at declaration (ES.20)
- [ ] `const`/`constexpr` by default (Con.1, ES.25)
- [ ] `enum class` with PascalCase enumerators (no `k`); first enumerator
  explicitly initialized; not plain `enum`
- [ ] `nullptr`, never `0`/`NULL` (ES.47)
- [ ] No narrowing conversions (ES.46); `-Wconversion -Wsign-conversion` clean
- [ ] No C-style casts (ES.48)
- [ ] Single-argument constructors are `explicit` (C.46)
- [ ] Rule of Zero or Rule of Five applied, not something in between
- [ ] Composition used unless the relationship is a genuine "is-a"
- [ ] Base class destructors public virtual or protected non-virtual (C.35)
- [ ] No `throw`/`try`/`catch` anywhere in project code
- [ ] Fallible functions return `std::expected`, marked `[[nodiscard]]`
- [ ] Optional values are `std::optional`, not a sentinel or a null pointer
- [ ] References by default; a pointer only when null is a valid state;
  trivially-copyable and cheap-to-copy types passed by value (value semantics)
- [ ] No `using namespace` in headers at global scope (SF.7)
- [ ] Headers have include guards and are self-contained (SF.8, SF.11)
- [ ] Locks use RAII (`scoped_lock`/`lock_guard`), always named (CP.44)
- [ ] Hot-path code avoids virtual dispatch, RTTI, and pointer-chasing
- [ ] Functions: PascalCase. Parameters: `p_` + snake_case. Locals:
  snake_case. Classes: PascalCase.
- [ ] Every class and struct data member ends with `_`
  plain snake_case
- [ ] Every brace on its own line; braces present on every conditional/loop
- [ ] 2-space indentation, no tabs; `clang-format-check` passes
- [ ] No magic numbers (ES.45)
