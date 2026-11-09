---
name: cpp-concurrency
description: "C++ concurrency: data races, RAII locks, named lock guards, publishing immutable snapshots, jthread, no detach, reference captures, false sharing. Read when you add or change threads, locks, atomics, or shared data."
disable-model-invocation: true
---

# Concurrency

Keep the number of threads small, and give each thread one clear job. A
slow helper thread must never delay the main loop. The project's
architecture rules say which threads exist.

| Rule | Summary |
|------|---------|
| **CP.2 / CP.3** | No data races; share as little writable data as possible |
| **CP.8** | Never use `volatile` for synchronization |
| **CP.20** | RAII locks only (`std::scoped_lock`, `std::unique_lock`) |
| **CP.22** | Never call unknown code while you hold a lock |
| **CP.44** | Always name a lock guard. An unnamed guard is destroyed immediately, so the lock is not held |

- Publish immutable data to other threads (for example, a
  `std::shared_ptr<const T>` that is swapped under a short lock). Do not
  share mutable state.
- Use `std::jthread`, not `std::thread`: its destructor joins, so a thread
  cannot be left running by mistake. Never `detach`.
- Do not give another thread a lambda that captures by reference.
- Prevent false sharing. If two threads each write their own variable
  frequently, put those variables on different cache lines (`alignas(64)`).

```cpp
class Snapshot final
{
public:
  void Publish(std::shared_ptr<const std::string> p_body)
  {
    const std::scoped_lock lock{mutex_};
    body_ = std::move(p_body);
  }

  [[nodiscard]] std::shared_ptr<const std::string> Live() const
  {
    const std::scoped_lock lock{mutex_};
    return body_;
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<const std::string> body_;
};
```
