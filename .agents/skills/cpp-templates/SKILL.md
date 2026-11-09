---
name: cpp-templates
description: "C++ templates: concepts, using instead of typedef, metaprogramming only when constexpr cannot do the job. Read when you write a template or a type alias."
disable-model-invocation: true
---

# Templates

| Rule | Summary |
|------|---------|
| **T.10 / T.11** | Constrain templates with concepts, standard ones where possible |
| **T.43** | `using`, not `typedef` |
| **T.120** | Template metaprogramming only when `constexpr` cannot do the job |

```cpp
template <std::unsigned_integral TNumber>
[[nodiscard]] TNumber ReadLittleEndian(std::span<const std::byte> p_bytes,
                                       std::size_t p_offset);
```
