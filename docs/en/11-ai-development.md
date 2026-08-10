# AI-Assisted Development Guide

test_shell includes an auto-memory system (`CLAUDE.md` + `memory/`) that helps AI coding assistants understand the codebase. This guide explains how to use it effectively.

## Auto-memory (Claude Code)

The project maintains a persistent memory store at `.claude/projects/.../memory/`. Each file captures one fact about the codebase — architecture, key classes, build steps, test strategy.

When you ask an AI assistant to work on this project, it automatically reads these memories and uses them as context. No need to explain the architecture every time.

### Memory files

| File | Content |
|------|---------|
| `project-overview.md` | High-level: what this project is, v2.5 changes, source layout |
| `architecture.md` | Data flow, ShellEngine 4-step pipeline, ResultStore, Slot safety |
| `key-classes.md` | API reference: ShellEngine, ResultStore, EventBus, Config |
| `build-guide.md` | How to configure, build, run, and test |
| `test-strategy.md` | Test patterns: atomic counters, aggressive cleanup, flaky detection |

### How to update memory

After making significant changes (new features, refactoring, API changes), ask the AI to update the relevant memory files:

> "Update memory to reflect the new platform layer"

The AI will read the current code, compare with existing memory, and update or create files as needed.

## Effective prompts for this codebase

### Adding a module

```
Add a new module called NetworkModule with functions: -f:connect, -f:send, -f:receive.
Follow the pattern in modules/PrintModule.h. Register it in main.cpp.
Write tests following the atomic counter pattern in test-strategy.md.
```

### Debugging

```
The test ShellEngineTest.ConcurrentShutdown is failing intermittently.
Read the test in test/core/shell_engine_test.cpp, trace the shutdown path
in ShellEngine::Shutdown(), and find potential race conditions.
```

### Cross-platform work

```
This project was just made cross-platform. Review core/platform/
and verify the POSIX implementations match the Windows API behavior.
Report any incompatibilities.
```

### Performance analysis

```
Run the PeakStressTest suite and analyze throughput. Compare against
the numbers in docs/en/10-performance.md. Identify bottlenecks.
```

### Documentation

```
Update docs/en/07-architecture.md to include the new platform layer.
Also create the Chinese version in docs/zh/07-架构概览.md.
```

## Project conventions (for AI assistants)

### Code style
- C++20, header-only core (inline implementations in `.h` files)
- Chinese comments alongside English code
- RAII everywhere — no manual resource management
- Atomic counters for test verification (never poll ResultStore in engine tests)
- `LOG_PLAIN` for output (never raw `std::cout`)

### Architecture rules
- Modules implement `IModule` interface: `GetName()`, `OnInit()`, `OnShutdown()`
- Use `REGISTER_FUNC(name, desc, lambda)` in `OnInit()`
- Worker threads execute `task->Step(bus)` in a loop
- Results flow through `ResultStore` (producer-consumer pattern)
- `ShellEngine` owns the main loop; `main()` only assembles

### Platform rules
- NEVER add `#ifdef _WIN32` or `#ifdef __linux__` outside `core/platform/`
- Use `PLATFORM_WINDOWS`, `PLATFORM_LINUX`, `PLATFORM_MACOS` from `platform.h`
- Use `PLATFORM_EXPORT` instead of `__declspec(dllexport)`
- Use `platform::SharedLibrary`, `platform::SharedMemory`, `platform::Process` for OS APIs
- Implement new platform features in `_win.cpp` and `_posix.cpp` pair files

### Test rules
- Engine tests: use atomic counters (NOT ResultStore)
- Fixture SetUp: `AggressiveCleanup()` — clear ResultStore, drain CommandParser
- Use `--gtest_repeat=3` to detect flaky tests
- Platform tests: write cross-platform (test edge cases, not OS-specific behavior)
- Slow tests (60s+) are excluded from CI: `--gtest_filter=-StressGraduated.*:Stress60s.*`

### Build rules
- Build: `cmake --preset debug && cmake --build --preset debug`
- Test: `cd out/build/debug && ./test_runner --gtest_filter=-StressGraduated.*:Stress60s.*:PeakStressTest.*`
- New `.cpp` files: add to the appropriate `CMakeLists.txt`
- Platform-specific `.cpp`: add under the `if(WIN32)` / `else()` branch
