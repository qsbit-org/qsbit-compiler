# Engineering rules

- Use English for repository documentation, comments, diagnostics, and commits.
- Use C++20, LLVM 21, CMake, Ninja, and CTest. Dependencies come from system packages.
- Keep QIR parsing, target mapping, scheduling, and code generation separate.
- Reject unsupported input with a diagnostic; never silently discard semantics.
- Keep controller instructions and executable ABI consistent with qsbit-sim.
- Test malformed input, generated ELF, timing, result order, and numerical quantum behavior.
- Treat warnings as errors for project code. Format C++ with clang-format-21.
- Run CTest and clang-tidy-21 on changed C++ before completing work.
- Keep generated artifacts, virtual environments, and local reports out of Git.
- Document only implemented behavior and state the supported QIR subset explicitly.
