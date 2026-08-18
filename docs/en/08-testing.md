# Testing Guide

## Quick start

```bash
# Configure + build (uses CMakePresets.json)
cmake --preset debug
cmake --build --preset debug

# Run all tests (excluding slow stress tests)
cd out/build/debug
./test_runner --gtest_filter=-StressGraduated.*:Stress60s.*:PeakStressTest.*

# Run a specific suite
./test_runner --gtest_filter=ShellEngineTest.*

# Run a single test
./test_runner --gtest_filter=ResultStore.HasResultsIsIdempotent

# Repeat N times to detect flaky tests
./test_runner --gtest_filter=ShellEngineTest.* --gtest_repeat=3
```

## Test suite overview (488 tests, 68 suites)

| Suite | Count | What it tests |
|-------|-------|---------------|
| **ShellEngineTest** | 20 | Main loop, input thread, shutdown race, memory leak, throughput |
| **PeakStressTest** | 6 | Ramp-up, burst, sustained load, pool utilization, recovery, slot exception |
| **ResultStore** | 15 | Push/Drain, HasResults idempotency, concurrent push, deadlock safety |
| **InputThread** | 13 | cv.wait_for predicate, multi-producer, boundary inputs, integration |
| **ConcurrencyStress** | 7 | TOCTOU races, shard data races, Emit blocking, DLL dangling slots |
| **DLLLifecycleTest** | 10 | DLL load/unload, multi-thread emit during unload, chaos |
| **TaskTest** | 12 | Assign, Step, PushShard, Pause/Resume/Cancel, progress |
| **TasksPoolTest** | 7 | Acquire/Release, pool exhaustion, edge cases |
| **CmdParserTest** | 27 | TXT parsing, SendPack, custom formats, concurrent access |
| **BusTest** | 24 | Register/Emit/Link/Remove signals, type safety, weak slots |
| **PlatformFileSystem** | 14 | FindFiles, FileExists, NormalizePath (cross-platform) |
| **PlatformSharedLibrary** | 9 | SharedLibrary Load/GetFunction, concurrent stress |
| **PlatformSharedMemory** | 10 | SharedMemory Create/Open/ReadWrite, concurrent |
| **PlatformProcess** | 9 | Process Launch/ExePath/CpuTimeUs |
| **PlatformBoundary** | 41 | Boundary, smoke, memory, concurrency edge cases |
| *Others* | ~120 | ThreadPool, Formatter, Logger, IModule, Metrics, etc. |

## Writing new tests

### Fixture + aggressive cleanup

Use the `AggressiveCleanup()` pattern in `SetUp` to prevent cross-test pollution:

```cpp
static void AggressiveCleanup() {
    ResultStore::Get().Clear();                     // ① 清空结果仓库
    auto& parser = CommandParser::Get();            // ② 排空命令解析器
    std::unique_ptr<ParmarPack> stale;
    while (parser.TryPopPack(stale)) {}
}

class MyTest : public ::testing::Test {
protected:
    void SetUp() override { AggressiveCleanup(); }
    void TearDown() override { AggressiveCleanup(); }
};
```

When EventBus signal accumulation is a concern, also remove known test signals in cleanup.

### Engine tests — atomic counter pattern

**Do NOT** check `ResultStore::HasResults()` or `ResultStore::Size()` in engine tests — the main loop's `DrainResults()` may have already consumed the result before your assertion runs.

**DO** use an atomic counter in the test module:

> **Sustained-load / stress tests use `RunWithoutInput()`**: in tests, `stdin` is EOF, so
> `engine.Run()`'s input thread `getline` returns immediately and stops the engine —
> delayed injections never get processed. Stress tests (like PeakStressTest) should call
> `engine.RunWithoutInput()` (main loop only) and feed commands via `InjectCommand`.
> Count completions with `SetResultSink` — don't drain `ResultStore` residue, which is a
> random tail at shutdown, not the real completion count.

> **When asserting "all N commands complete", size the pool ≥ N**: `SubmitTask` drops
> commands when the pool is full (backpressure). If the pool < N, rapid injection of N
> commands can occasionally drop 1–2, making `exec_count == N` assertions flaky. For
> "all complete" assertions, size the pool to ≥ N; to only verify "some complete", assert `> 0`.

```cpp
class TestMod : public ModuleBaseObject {
    std::atomic<int>* cnt_;
public:
    TestMod(std::string name, std::atomic<int>* cnt) : name_(name), cnt_(cnt) {}
    const char* GetName() const override { return name_.c_str(); }
    bool OnInit() override {
        REGISTER_FUNC("echo", "", {
            pack->success = true;
            if (cnt_) cnt_->fetch_add(1);   // ★ 原子递增，不受主循环影响
        });
        return true;
    }
};

// In test:
std::atomic<int> exec_count{0};
mgr.AddModule(std::make_unique<TestMod>("MyMod", &exec_count));

ShellEngine engine(4, 2);
std::thread runner([&]() { engine.RunWithoutInput(); });  // not Run(): stdin is EOF
engine.InjectCommand("-m:MyMod -f:echo");

// Wait for counter
for (int w = 0; w < 100 && exec_count.load() < 1; ++w)
    std::this_thread::sleep_for(10ms);

engine.InjectCommand("/exit");
runner.join();

EXPECT_GT(exec_count.load(), 0);
```

### Module tests

For tests that need a real module, use a lightweight inline class:

```cpp
class TestMod : public ModuleBaseObject {
    const char* GetName() const override { return "TestMod"; }
    bool OnInit() override {
        REGISTER_FUNC("echo", "echo back", {
            pack->return_value = pack->GetOr("msg", "");
            pack->success = true;
        });
        return true;
    }
};
mgr.AddModule(std::make_unique<TestMod>());
// ... test ...
mgr.UnloadModule("TestMod");
```

### ResultStore tests

```cpp
auto& store = ResultStore::Get();
store.Clear();

store.PushResult(1, std::make_unique<ParmarPack>(...));
EXPECT_TRUE(store.HasResults());

auto batch = store.Drain();
EXPECT_EQ(batch.size(), 1u);

store.Clear();
```

## Test categories

| Category | Pattern | Focus |
|----------|---------|-------|
| Basic | `PushAndDrain`, `ConstructDestruct` | Single-operation correctness |
| Predicate | `HasResultsIsIdempotent` | cv predicate safety |
| Concurrent | `ConcurrentPushNoDataLoss`, `NoDeadlockPushAndDrain` | Thread safety |
| Boundary | `BoundaryEmptyLine`, `BoundaryNullPack` | Edge cases |
| Memory | `MemoryNoLeak`, `MemoryStability_LongRun` | No leaks under load |
| Integration | `IntegrationWorkerToMainLoop` | Full pipeline |
| Shutdown | `ShutdownRace_LastResultNotLost` | Clean exit |
| Peak | `RampUpThroughput`, `BurstOverload` | Throughput & limits |

## Detecting flaky tests

```bash
# Run a suite 3 times — all must pass
./test_runner --gtest_filter=ShellEngineTest.* --gtest_repeat=3

# Shuffle test order to expose ordering bugs
./test_runner --gtest_shuffle --gtest_repeat=5
```

## Slow tests

| Test | Duration | When to run |
|------|----------|-------------|
| `Stress60s.FullPipeline60Seconds` | 60s | Before release |
| `StressGraduated.TwoMinutes` | 120s | Before release |
| `StressGraduated.ThreeMinutes` | 180s | Pre-release soak |

```bash
# Run everything including slow tests
./test_runner
```

## Adding a test file

1. Create `test/core/your_test.cpp`
2. Add it to `test/CMakeLists.txt`:
   ```cmake
   core/your_test.cpp
   ```
3. Build: `cmake --build --preset debug --target test_runner`
