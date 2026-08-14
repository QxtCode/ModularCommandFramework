# Performance & Limits — test_shell v2.7

> Measured on: Intel i7-13700H (16 logical cores), Debug build, cross-platform

## Peak throughput

| Metric | Value | Notes |
|--------|-------|-------|
| **Peak throughput** | ~23,000 cmd/s | Ramp-up injection, 16-slot pool, 8 workers |
| **Bottleneck** | Main loop cycle (100ms cv timeout) | Each command needs 2 cycles: input→process + complete→drain |
| **Theoretical max** | POOL_SIZE × 10 cycles/sec | With 16 slots: ~160 cmd/s per-client visible, higher internally |

### How throughput was measured

```bash
./test_runner --gtest_filter=PeakStressTest.RampUpThroughput
```

Full-speed injection for 1 second, counting completed results in `ResultStore`.

## Sustained load

| Metric | Value |
|--------|-------|
| **Duration** | 10 seconds at ~200 cmd/s injection |
| **Memory delta** | < 50 MB (measured via `GetProcessMemoryInfo`) |
| **Pool recovery** | Full — all tasks returned to pool after test |

## Latency characteristics

| Percentile | Approximate | Cause |
|------------|-------------|-------|
| P50 | ~100ms | One main loop cycle |
| P95 | ~300ms | Two cycles + thread scheduling |
| P99 | ~500ms | Max wait + OS scheduling jitter |

The dominant latency source is the `ShellEngine::WaitForWork()` 100ms timeout.
Commands complete quickly on worker threads, but result visibility depends on
the main loop's next `DrainResults()` cycle.

## Concurrency & safety

| Test | Result |
|------|--------|
| **Slot exception catch** | ✅ Process survives — `catch(...)` in `Slot::Run` marks slot dead |
| **Burst overload** | ✅ 500 commands burst, pool recovers fully |
| **Recovery** | ✅ Overload → idle within 3 seconds |
| **Concurrent push** | ✅ 2 pushers + 2 drainers, 0 errors, 231K operations |
| **TOCTOU races** | ✅ 4-thread AddModule/UnloadModule, zero zombie signals |

## Limits & tuning

### Pool exhaustion

When all task slots are occupied, `TasksPool::Acquire()` returns `nullptr`.
The engine logs `[ERROR] All tasks busy` and skips the command.

**Tuning:** increase `pool_size` in config or `--pool_size=N` CLI argument.

### Worker saturation

When all workers are busy, tasks queue up in the thread pool. The thread pool
has an unbounded queue — in extreme cases this can cause memory growth.

**Tuning:** increase `workers` in config. Rule of thumb: `workers = CPU cores - 2`.

### Main loop cycle time

The `WaitForWork()` timeout (100ms) is the minimum latency for result visibility.
For lower latency, reduce the timeout. Trade-off: lower timeout = higher CPU usage
when idle.

**Tuning:** modify `ShellEngine::WaitForWork()` timeout parameter.

## Configuration reference

Configuration is loaded by `ShellConfig` in `main.cpp` at startup.  
See [07-architecture](07-architecture.md) for the loading flow.

### test_shell.cfg (place next to the executable)

```ini
# Engine
pool_size = 8
workers = 4

# Paths
plugin_dir = plugins

# Logging (debug | info | error | fatal)
log_level = info

# Alerts (0 = disabled)
queue_warn = 0
```

### Command-line overrides

```bash
./test_shell --pool_size=32 --workers=12 --plugin_dir=./my_plugins
```

Priority: CLI args > config file > defaults.

## Test baseline

All performance assertions are validated in `test/core/peak_stress_test.cpp`:

```bash
# Run performance tests only
./test_runner --gtest_filter=PeakStressTest.*

# Expected output: 6 tests, all PASSED
```
