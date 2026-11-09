---
name: cpp-resource-management
description: "C++ resource ownership: RAII, unique_ptr and shared_ptr, no naked new/delete or malloc/free, raw pointers do not own, wrapping C handles. Read when you acquire a file descriptor, socket, database handle, memory, or other resource."
disable-model-invocation: true
---

# Resource management

| Rule | Summary |
|------|---------|
| **R.1** | RAII for every resource |
| **R.3** | A raw `T*` never owns |
| **R.5** | Prefer scoped objects; do not use the heap without a reason |
| **R.10 / R.11** | No `malloc` / `free`, no naked `new` / `delete` |
| **R.20 / R.21** | `std::unique_ptr` for ownership; `std::shared_ptr` only when ownership really is shared |

RAII does not depend on exceptions. A destructor runs on every exit from a
scope, also on an early `return` of an error. `std::expected` changes how a
failure is reported, not how resources are released. Destructors,
deallocation, and swap must never fail.

For a C API (`open`, `socket`, `sqlite3_open_v2`), put the handle in its
owner immediately after you acquire it, before the next line that can
return early.

If the resource is a pointer with a close function (a database handle, a
`FILE*`), use a `std::unique_ptr` with a custom deleter. For a handle that
is not a pointer, such as a file descriptor, write one small owner class,
and let other classes hold it and follow the Rule of Zero:

```cpp
class UniqueFd final
{
public:
  explicit UniqueFd(int p_fd) noexcept : fd_(p_fd)
  {
  }

  ~UniqueFd()
  {
    if (fd_ >= 0)
    {
      ::close(fd_);
    }
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& p_other) noexcept : fd_(std::exchange(p_other.fd_, -1))
  {
  }

  UniqueFd& operator=(UniqueFd&& p_other) noexcept
  {
    if (this != &p_other)
    {
      if (fd_ >= 0)
      {
        ::close(fd_);
      }
      fd_ = std::exchange(p_other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int Get() const noexcept
  {
    return fd_;
  }

private:
  int fd_;
};
```
