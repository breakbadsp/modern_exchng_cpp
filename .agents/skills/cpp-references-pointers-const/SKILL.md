---
name: cpp-references-pointers-const
description: "C++ choice of value, reference, or pointer, and of const or non-const, for function parameters, local variables, and range-for variables. Read when you write a parameter or a local variable."
disable-model-invocation: true
---

# References, pointers, and const

Use only the forms in the two tables below. They cover almost all code. If
you need another form, write a one-line comment that says why.

Based on the Core Guidelines F.15 to F.18, F.60, R.30, Con.1, Con.4, and
ES.25.

## Function parameters

| The function... | Use | Example |
|-----------------|-----|---------|
| reads a cheap value | `T` | `int p_count`, `std::string_view p_name`, `std::span<const std::byte> p_bytes` |
| reads a larger or non-copyable object | `const T&` | `const Engine& p_engine` |
| keeps its own copy (a sink) | `T`, then `std::move` it into place | `std::string p_name` |
| changes an object that the caller keeps (in-out) | `T&` | `std::vector<Row>& p_rows` |
| reads an object that can be absent | `const T*`; null means absent | `const Sample* p_previous` |
| changes an object that can be absent | `T*` | `Stats* p_stats` |
| takes ownership | `std::unique_ptr<T>` | `std::unique_ptr<Job> p_job` |
| forwards an argument, in a template | `T&&` with `std::forward` | `TArg&& p_arg` |

- **Cheap** means trivially copyable and at most about two words (16 bytes):
  built-in types, enumerations, `std::string_view`, `std::span`, and small
  structs of these. Pass `std::string_view` and `std::span` by value. Never
  pass them by `const&`.
- **No output-only parameters.** Return the value instead, or a struct for
  several values. `T&` is for an object that the caller keeps and the
  function changes, such as a buffer to reuse.
- **A pointer parameter means that null is a valid input.** The function
  checks it before use. A required object is a reference. A raw pointer
  never owns.
- **Do not pass a smart pointer** unless the function takes or shares
  ownership. Pass the object as `const T&`, `T&`, or `T*`.
- **No top-level `const` on a by-value parameter** (`const int p_count`). It
  is not part of the interface, and the compiler ignores it in a
  declaration. Do not assign to a by-value parameter. If you need a changed
  value, declare a new local. Moving from a sink parameter is permitted.
- **Do not keep a `const T&` parameter after the call** (in a member, a
  lambda, or a returned view). It can refer to a temporary. If the function
  keeps the object, take it as a sink.

## Local variables

| The local... | Use | Example |
|--------------|-----|---------|
| does not change after initialization (the default) | `const auto` or `const T` | `const auto count = rows.size();` |
| is known at compile time | `constexpr` with a `k` name | `constexpr std::size_t kMaxRows = 1024;` |
| changes later (counter, accumulator, buffer) | `auto` or `T` | `std::size_t total_bytes = 0;` |
| is moved from or returned by name | `auto` or `T` | `std::string body;` |
| names part of a long-lived object without a copy | `const auto&` | `const auto& config = engine.Config();` |
| names part of a long-lived object that this code changes | `auto&` | `auto& slot = slots_[slot_index];` |
| can refer to nothing, or must point to different objects | `const T*` or `T*` | `const Node* node = head;` |

- **`const` blocks a move.** `std::move` on a `const` local copies it. A
  returned `const` local is copied when the compiler does not elide the
  copy. Make such a local non-const.
- **A local reference must not outlive its object.** Bind a reference only
  to an object that has a name and an owner that lives longer than the
  reference. A reference that a function returns from a temporary dangles:
  - `const auto& name = MakeRecord().Name();` when `Name` returns a
    reference to a member;
  - `const auto& row = *FindRow(p_key);` when `FindRow` returns a
    `std::optional` by value.
- **Do not bind a reference to a returned value.** Write
  `const auto rows = Load();`, not `const auto& rows = Load();`. The copy is
  elided, so the reference gives nothing.
- **A reference or pointer into a container becomes invalid** when the
  container reallocates (`push_back`, `insert`, `erase`). Do not keep one
  across such a change. Keep an index.
- **Before you use a pointer local**, try an index, an iterator, or
  `std::optional`. If the pointer itself does not change, make it `const`
  too: `const Node* const parent = node->parent_;`.

## Range-for variables

| The loop body... | Use |
|------------------|-----|
| reads cheap elements | `for (const auto value : values)` |
| reads larger elements | `for (const auto& row : rows)` |
| changes the elements | `for (auto& row : rows)` |
| is generic code over any range | `for (auto&& item : range)` |

## Example

```cpp
// A cheap value, a large read-only object, and an optional input.
[[nodiscard]] double Rate(const Sample& p_sample, const Sample* p_previous,
                          std::uint64_t p_interval_ms);

class Job final
{
public:
  // A sink: take the string by value, then move it into place.
  void Rename(std::string p_name)
  {
    name_ = std::move(p_name);
  }

private:
  std::string name_;
};

// In-out: the caller reuses one buffer for each call.
void FillRows(std::span<const Sample> p_samples, std::vector<Row>& p_rows)
{
  p_rows.clear();
  for (const auto& sample : p_samples)
  {
    const auto rate = Rate(sample, nullptr, kIntervalMs);
    p_rows.push_back(Row{rate});
  }
}
```
