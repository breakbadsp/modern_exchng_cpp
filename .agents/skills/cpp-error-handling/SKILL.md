---
name: cpp-error-handling
description: "C++ error handling: operating versus programmer errors, std::expected and std::optional, choosing the error type, fallible constructors (static Create), when exceptions are allowed, noexcept. Read when a function can fail, or when you write throw, try, catch, or noexcept."
disable-model-invocation: true
---

# Error handling

## Two kinds of failure

TigerStyle separates two kinds of failure, and these rules do too:

- **Operating errors** are expected: malformed input, a missing file, a
  busy database, a port that is already in use, a process that exited
  between two reads. Handle them. Return them as values.
- **Programmer errors** are bugs: an index past the end, a broken
  invariant, a state that "cannot happen". Do not return them. Check them
  with an assertion (`cpp-assertions`), which stops the program. A crash
  makes a silent correctness bug visible.

## Return values, not exceptions

- A function that can fail returns `std::expected<T, E>`. If it has no
  value to return, use `std::expected<void, E>`.
- A value that can be missing, where missing is not an error, is a
  `std::optional<T>`. Do not use sentinels (`-1`, `""`, `nullptr`).
- Every function that returns `std::expected` or `std::optional` is
  `[[nodiscard]]`. With `-Werror`, a caller that ignores the result does
  not compile.
- Choose the error type `E` by how hot the path is:
  - Hot paths (each packet, each sample): an `enum class`, or a
    `std::string_view` that points to a string literal. Making the error
    never allocates.
  - System calls: `std::error_code{errno, std::generic_category()}`.
    Capture `errno` immediately, before another call can change it.
  - Startup and configuration: `std::string`, so that the message can name
    the file, key, or value.
- Use an early `return` for each error, so that the main path stays flat.
  Use `and_then`, `transform`, or `or_else` only when the chain is clearer
  than the `if` statements that it replaces.
- Do not call `.value()` on a `std::expected` or `std::optional`. It throws
  when the object is empty. Check first, then use `*result`, `result->`,
  or `value_or`.

```cpp
enum class DecodeError
{
  ShortHeader = 0,
  BadVersion,
};

[[nodiscard]] std::expected<Header, DecodeError> DecodeHeader(
    std::span<const std::byte> p_bytes)
{
  if (p_bytes.size() < kHeaderBytes)
  {
    return std::unexpected(DecodeError::ShortHeader);
  }
  const auto version = ReadLittleEndian<std::uint16_t>(p_bytes, 0);
  if (version != kProtocolVersion)
  {
    return std::unexpected(DecodeError::BadVersion);
  }
  return Header{version};
}
```

## A constructor that can fail

Do not throw from a constructor. Make the constructor private, and add a
static `Create` that returns `std::expected`. The constructor then only
stores a resource that `Create` already acquired, so it cannot fail.

```cpp
class File final
{
public:
  [[nodiscard]] static std::expected<File, std::error_code> Create(
      const std::filesystem::path& p_path)
  {
    const int fd = ::open(p_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
      return std::unexpected(std::error_code{errno, std::generic_category()});
    }
    return File{UniqueFd{fd}};
  }

private:
  explicit File(UniqueFd p_fd) noexcept : fd_(std::move(p_fd))
  {
  }

  // UniqueFd closes the descriptor (cpp-resource-management), so File
  // follows the Rule of Zero.
  UniqueFd fd_;
};
```

## When an exception is allowed

Each exception must have a reason. An exception is allowed only in these
cases:

1. **Code that you do not own throws:** the `std::thread` constructor,
   `std::filesystem` calls that have no `std::error_code` overload,
   allocation. Use the non-throwing overload when one exists. Otherwise,
   catch at the nearest boundary and convert to `std::expected`. Do not
   recover from `std::bad_alloc`; let it stop the program. A standard
   interface that requires a throw, such as an allocator that reports
   failure with `std::bad_alloc`, is also in this case.
2. **Last-resort handlers** in `main` and in each thread entry function.
   One `catch (const std::exception&)` logs the error. `main` returns a
   non-zero exit code. A thread ends its failed unit of work. These
   handlers catch bugs. They are not the error-handling design.
3. **Test helpers.** In tests, a failed check can throw to stop the current
   test case.
4. **Compile-time errors.** A `throw` in a `consteval` function never runs.
   If the compiler must evaluate it, the build fails.

Never use exceptions:

- on the hot path;
- for operating errors (bad input, a missing file, a busy database);
- for control flow, such as `try { ... } catch (...) { continue; }` in a
  loop;
- across a module boundary. A function that other modules call returns
  `std::expected`.

Do not build with `-fno-exceptions`. The standard library still throws,
and the cases above need the mechanism.

Some existing code throws in other cases. When you change such a function,
convert it to `std::expected` in the same change. Do not mix the conversion
with unrelated edits.

## `noexcept`

Mark a function `noexcept` when it cannot throw: moves, swaps, destructors,
accessors, and arithmetic on trivial types. Do not add `noexcept` to a
function that allocates, or that calls code that can throw. With exceptions
enabled, a wrong `noexcept` turns an exception into `std::terminate`.
