# Monitoring System — Watch Your Framework

## Three MetricsCollector commands

| Command | Usage | Consumes task slot? |
|---------|-------|:---:|
| `-f:show` | Print an inline snapshot to the console | Yes |
| `-f:metrics` | Print a one-line summary | Yes |
| `-f:dashboard` | Launch `shell_monitor.exe` as a separate FTXUI fullscreen window | **No** |

### 1. Inline snapshot

```
> -m:MetricsCollector -f:show
[OK] === test_shell v2.7 Dashboard ===
  CPU:     0.00%
  Memory:  0 KB
  Threads: 4 workers / 16 total
  Tasks:   0 active / 8 total
  Modules: 4 loaded
  Uptime:  15s
```

Help: `-m:MetricsCollector -f:help`

### 2. One-line summary

```
> -m:MetricsCollector -f:metrics
[OK] Uptime:15s Tasks:0/8 Modules:4 Signals:12 Threads:4 Stuck:0
```

### 3. External dashboard (zero task cost)

```
> -m:MetricsCollector -f:dashboard
[OK] Dashboard launched in new window (close with q)
```

Uses `platform::Process::Launch` to spawn `shell_monitor` as an independent process (new console on Windows, same terminal on Linux/macOS). It reads shared memory in real time via FTXUI — no framework task slots consumed, no console input blocked.

Manual launch also works:
```
Terminal 1: ./test_shell
Terminal 2: ./shell_monitor
```

## How it works

```
test_shell                        shell_monitor
┌──────────────────┐              ┌────────────────────┐
│ MetricsCollector │   shared     │ main()             │
│   Flush()        │──memory──→  │   read MetricsData │
│   per main loop  │  test_shell │   FTXUI dashboard  │
└──────────────────┘  _metrics    └────────────────────┘
```

- `MetricsCollector` is a regular IModule — loaded like any other module
- Writes to named shared memory via `platform::SharedMemory` (wraps `CreateFileMapping`/`shm_open+mmap`)
- Monitor reads from the same memory via `platform::SharedMemory::Open`
- Seqlock protocol (`BeginWrite`/`EndWrite`/`TryRead`) ensures consistent reads without locks
- Framework crash → monitor shows "NO SIGNAL"
- Monitor crash → framework unaffected
- **Cross-platform**: same protocol works on Windows (File Mapping) and POSIX (`shm_open`)

## MetricsCollector interface

Same as any module: `GetName()`, `OnInit()`, `OnShutdown()`.

Added public methods for the main loop:
```cpp
metrics->SetTasks(active, queued, total, completed);
metrics->SetModules(count);
metrics->Flush();  // write to shared memory
```

Called from `ShellEngine::FlushMetrics()` every main loop iteration — no background thread needed.
