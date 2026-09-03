# test/ — 测试目录

## 写测试的约定

1. **优先复用已有测试案例**：新测试优先加到 `test/core/`、`test/sdk/`、`test/parser/` 等
   已有的测试文件 / 套件里，复用已有的 fixture 和辅助函数，不重复造轮子：
   - `RegisterFastModule(name)` / `RegisterSlowModule(name)` — 注册极快/慢模块（`peak_stress_test.cpp`）
   - `AggressiveCleanup()` — 清空 ResultStore + drain CommandParser，防跨测试污染
   - `StressWorkers()` — 压测线程数 = `hardware_concurrency() - 2`
2. **只有现有套件覆盖不了时才新建测试文件**，并在 `test/CMakeLists.txt` 的
   `add_executable(test_runner ...)` 源文件列表里添加。
3. **不要为性能测量单独建可执行目标**（如 perf_real）。gtest 用例用
   `--gtest_filter=SuiteName.*` 就能单独跑，无需独立 target。临时测量程序跑完即删，
   不留在 CMakeLists 里。
4. **性能测试的可信测法**见 [docs/zh/10-性能测试.md](../docs/zh/10-性能测试.md)
   「可信吞吐测法」章节（完成判据用池空闲、计数用 result sink、stdout 重定向、N≥20000）。

## 目录结构

```
test/
├── main.cpp                  — gtest main（可在此设日志级别）
├── core/                     — 核心测试（ShellEngine/Task/TasksPool/ResultStore/压测/平台…）
│   ├── peak_stress_test.cpp  — 峰值吞吐 + 极限压力（6 tests）
│   ├── task_persistence_perf_test.cpp — 任务存储/加载性能（3 tests：单次耗时/批量总用时/吞吐影响）
│   ├── shell_engine_test.cpp — 引擎集成测试
│   └── ...
├── sdk/                      — SDK 契约测试（IModule/ModuleBase/接线）
├── parser/                   — 命令解析测试
├── eventbus/                 — EventBus 测试
├── modules/                  — 内置模块测试
├── test_dll/                 — 测试用 DLL 模块
└── CMakeLists.txt            — test_runner 构建（源文件列表 + gtest_discover_tests）
```

## 运行

```bash
cd out/build/debug    # 或 release

# 全套（排除慢速压力测试）
./test_runner --gtest_filter=-StressGraduated.*:Stress60s.*:PeakStressTest.*

# 单个套件
./test_runner --gtest_filter=PeakStressTest.*

# 重复验证稳定性
./test_runner --gtest_filter=ShellEngineTest.* --gtest_repeat=3
./test_runner --gtest_shuffle --gtest_repeat=5
```

## 慢速测试（CI 排除）

`Stress60s` (60s) / `StressGraduated.TwoMinutes` (120s) / `ThreeMinutes` (180s)
排除：`--gtest_filter=-StressGraduated.*:Stress60s.*`

## DLL 查找策略

测试需加载 `TestDLLModule.dll` 时，按两级查找：
1. 当前工作目录（test_runner.exe 同目录）
2. `out/build/debug/`（CMake 输出目录）
找不到则 `GTEST_SKIP()`。

## 常见坑

- **stdin EOF**：测试环境 stdin 是 EOF，`Run()` 会立即退出。集成/压力测试用
  `RunWithoutInput()`，命令靠 `InjectCommand` 喂。
- **别查 ResultStore 验证完成数**：结果一进去就被 DrainResults 消费。用
  `SetResultSink` + `atomic<int>` 计数。
- **背压丢命令**：全速注入 N 条且池子 < N 会丢命令。断言「全部完成」时池子要 ≥ N。
- **压测 stdout 噪声**：`show_explanation` 默认 true，每命令打 `--- [Mod.func] ---`，
  多线程下拖慢 350×。压测时重定向 stdout（`> /dev/null` / `> nul`）。