---
name: cpp-classes-and-composition
description: "C++ class design: class versus struct, Rule of Zero and Rule of Five, explicit constructors, final, virtual destructors, overrides, const member functions, composition over inheritance, no const or reference members. Read when you define or change a class or struct."
disable-model-invocation: true
---

# Classes and composition

| Rule | Summary |
|------|---------|
| **C.2** | `class` if there is an invariant; `struct` if the members vary independently |
| **C.9** | Expose as little as possible |
| **C.20** | Rule of Zero: do not write special members that you do not need |
| **C.21** | Rule of Five: if you write or delete one, handle all five |
| **C.35** | A base destructor is public virtual or protected non-virtual |
| **C.41** | A constructor makes a fully initialized object |
| **C.46** | Single-argument constructors are `explicit` |
| **C.128** | An override uses exactly one of `virtual`, `override`, `final` |
| **Con.2** | Member functions are `const` unless they change the object's observable state |
| — | Mark a class `final` unless it is a base class |
| — | Inheritance is always `public`. If you want private inheritance, use a member |

```cpp
// Avoid: inheritance only to reuse FormatLine(). A logger is not a formatter.
class Logger : public Formatter
{
};

// Prefer: the logger has a formatter.
class Logger final
{
public:
  void Write(std::string_view p_text)
  {
    std::fputs(formatter_.FormatLine(p_text).c_str(), stderr);
  }

private:
  Formatter formatter_;
};
```

- **Data members are not `const` and not references.** Either one makes the
  class impossible to assign, and a `const` member cannot be moved from. For
  a member that must not change after construction, make it private with
  no setter.
- A mutex that a `const` member function locks is `mutable`.
- Do not call virtual functions from a constructor or destructor.
- Do not `memset` or `memcpy` a type that is not trivially copyable.
