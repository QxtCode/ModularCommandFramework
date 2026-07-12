#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_shell 框架性能基准测试
===========================
测试整个命令解析 + 模块执行管道的耗时。
不修改任何源文件 .h/.cpp，只反复运行编译好的可执行文件。

用法:
    python benchmark.py                  # 使用已有 exe 测试
    python benchmark.py --build          # 先 Release 编译再测试
    python benchmark.py --build --conc   # 编译 + 顺序 + 并发
    python benchmark.py --runs 3000      # 跑 3000 次
    python benchmark.py --config Debug   # 用 Debug 配置
"""

import subprocess
import time
import sys
import os
import argparse
import statistics
import tempfile
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

# 修复 Windows GBK 终端编码问题
if sys.platform == "win32":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# ===================== 路径配置 =====================

PROJECT_DIR = Path(r"E:\C语言程序\c++项目\test_shell\test_shell")
SOLUTION   = PROJECT_DIR / "test_shell.sln"
EXE_DEBUG  = PROJECT_DIR / "x64" / "Debug" / "test_shell.exe"
EXE_RELEASE = PROJECT_DIR / "x64" / "Release" / "test_shell.exe"

MSBUILD = r"E:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
CL_EXE  = r"E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.43.34808\bin\Hostx64\x64\cl.exe"

# ===================== 编译 =====================

def build_project(config: str = "Release") -> bool:
    """用 MSBuild 编译 test_shell 项目。"""
    print(f"\n{'='*60}")
    print(f"  [BUILD] 正在编译项目 ({config}) ...")
    print(f"{'='*60}\n")

    result = subprocess.run(
        [MSBUILD, str(SOLUTION),
         f"/p:Configuration={config}", "/p:Platform=x64",
         "/m", "/v:minimal"],
        capture_output=True,
        cwd=str(PROJECT_DIR),
        encoding="utf-8", errors="replace",
    )

    if result.returncode != 0:
        print("[FAIL] 编译失败!")
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        return False

    print("[OK] 编译成功\n")
    return True


# ===================== 基线程序（测量进程创建开销） =====================

BASELINE_CPP = """
// 最小空程序 —— 用于测量进程创建/销毁的纯开销
int main() { return 0; }
"""

def build_baseline() -> Path | None:
    """编译一个最小空程序，用于测量进程创建/销毁开销。"""
    import tempfile
    tmpdir = Path(tempfile.gettempdir()) / "test_shell_bench"
    tmpdir.mkdir(exist_ok=True)

    cpp_file = tmpdir / "baseline.cpp"
    exe_file = tmpdir / "baseline.exe"

    cpp_file.write_text(BASELINE_CPP, encoding="utf-8")

    # 需要 VS 开发者环境，先用 vcvars 或直接调 cl.exe
    # 尝试找到 cl.exe
    cl_paths = list(Path(r"E:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC").glob("**/bin/Hostx64/x64/cl.exe"))
    if not cl_paths:
        # 回退: 用 MSBuild 编译一个最简单的项目？太复杂，跳过基线
        print("[WARN] 找不到 cl.exe，跳过基线测量")
        return None

    cl = str(cl_paths[-1])  # 用最新版本
    print(f"  [BASELINE] 编译空程序: {exe_file}")

    result = subprocess.run(
        [cl, str(cpp_file), "/Fe:" + str(exe_file), "/nologo", "/O2"],
        capture_output=True,
        cwd=str(tmpdir),
        encoding="utf-8", errors="replace",
    )

    if result.returncode != 0:
        print(f"  [WARN] 基线编译失败: {result.stderr[:500]}")
        return None

    print(f"  [OK] 基线程序就绪 ({exe_file.stat().st_size} bytes)")
    return exe_file


# ===================== 运行与计时 =====================

def run_once(exe_path: Path, timeout: float = 10.0) -> tuple[float, bool]:
    """运行一次可执行文件，返回 (耗时秒, 是否成功)。"""
    try:
        start = time.perf_counter()
        result = subprocess.run(
            [str(exe_path)],
            capture_output=True,
            timeout=timeout,
            cwd=str(exe_path.parent),
            encoding="utf-8", errors="replace",
        )
        elapsed = time.perf_counter() - start
        return elapsed, result.returncode == 0
    except subprocess.TimeoutExpired:
        return timeout, False
    except Exception:
        return 0.0, False


def warmup(exe_path: Path, rounds: int = 10):
    """预热：先跑几轮让 OS 文件缓存 / CPU 进入稳态。"""
    print(f"[WARMUP] 预热 {rounds} 轮...")
    for _ in range(rounds):
        run_once(exe_path)


def run_sequential(exe_path: Path, total: int) -> list[float]:
    """顺序运行 total 次，返回每次耗时列表。"""
    times = []
    print(f"\n{'='*60}")
    print(f"  [SEQ] 顺序测试: {total} 次")
    print(f"{'='*60}")

    batch_size = max(1, total // 20)
    fails = 0
    t_start = time.perf_counter()

    for i in range(total):
        elapsed, ok = run_once(exe_path)
        if ok:
            times.append(elapsed)
        else:
            fails += 1
        if (i + 1) % batch_size == 0 or i == total - 1:
            print(f"  进度: {i+1}/{total} ({(i+1)/total*100:.0f}%)  |  失败: {fails}")

    t_total = time.perf_counter() - t_start
    print(f"\n  总耗时: {t_total:.3f}s  |  失败次数: {fails}")
    return times


def run_concurrent(exe_path: Path, total: int, workers: int = 8) -> list[float]:
    """并发运行 total 次（线程池），返回每次耗时列表。"""
    times = []
    print(f"\n{'='*60}")
    print(f"  [CONC] 并发测试: {total} 次 (线程: {workers})")
    print(f"{'='*60}")

    t_start = time.perf_counter()
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [executor.submit(run_once, exe_path) for _ in range(total)]
        for fut in as_completed(futures):
            elapsed, ok = fut.result()
            if ok:
                times.append(elapsed)

    t_total = time.perf_counter() - t_start
    print(f"\n  总耗时: {t_total:.3f}s  |  成功: {len(times)}/{total}")
    return times


# ===================== 统计报告 =====================

def report(times: list[float], label: str, baseline_ms: float | None = None):
    """打印统计报告。"""
    if not times:
        print(f"\n  [WARN] {label}: 无有效数据")
        return

    times.sort()
    n = len(times)
    avg_ms = statistics.mean(times) * 1000
    med_ms = statistics.median(times) * 1000
    total_s = sum(times)

    print(f"\n{'─'*60}")
    print(f"  [STAT] {label}")
    print(f"{'─'*60}")
    print(f"  样本数:      {n}")
    print(f"  最小值:      {times[0]*1000:.3f} ms")
    print(f"  最大值:      {times[-1]*1000:.3f} ms")
    print(f"  平均值:      {avg_ms:.3f} ms")
    print(f"  中位数:      {med_ms:.3f} ms")
    if n >= 10:
        print(f"  P10:         {times[n*10//100]*1000:.3f} ms")
        print(f"  P90:         {times[n*90//100]*1000:.3f} ms")
        print(f"  P95:         {times[n*95//100]*1000:.3f} ms")
        print(f"  P99:         {times[n*99//100]*1000:.3f} ms")
    if n >= 2:
        print(f"  标准差:      {statistics.stdev(times)*1000:.3f} ms")
    print(f"  总耗时:      {total_s:.3f} s")
    print(f"  吞吐量:      {n/total_s:.1f} 次/秒" if total_s > 0 else "")
    if baseline_ms is not None and baseline_ms > 0:
        framework_ms = avg_ms - baseline_ms
        print(f"  进程开销:    {baseline_ms:.3f} ms (基线)")
        print(f"  框架净耗时:  {framework_ms:.3f} ms (平均值 - 基线)")
    print(f"{'─'*60}")


# ===================== 主流程 =====================

def main():
    parser = argparse.ArgumentParser(
        description="test_shell 框架性能基准测试",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python benchmark.py                     # 使用已编译 exe 测试 1000 次
  python benchmark.py --build             # 先 Release 编译再测试
  python benchmark.py --build --conc      # 编译 + 顺序 + 并发测试
  python benchmark.py --runs 5000         # 跑 5000 次
  python benchmark.py --config Debug      # 用 Debug 配置
        """,
    )
    parser.add_argument("--build", action="store_true", help="先编译项目再测试")
    parser.add_argument("--config", default="Release", choices=["Debug", "Release"],
                        help="编译配置 (默认 Release)")
    parser.add_argument("--runs", type=int, default=1000, help="运行次数 (默认 1000)")
    parser.add_argument("--conc", action="store_true", help="额外执行并发测试")
    parser.add_argument("--workers", type=int, default=8, help="并发线程数 (默认 8)")
    parser.add_argument("--warmup", type=int, default=10, help="预热轮数 (默认 10)")
    parser.add_argument("--exe", type=str, default=None, help="直接指定 exe 路径")
    parser.add_argument("--baseline", action="store_true", help="测量进程创建开销基线")

    args = parser.parse_args()

    # 确定 exe 路径
    if args.exe:
        exe = Path(args.exe)
    elif args.config == "Release":
        exe = EXE_RELEASE
    else:
        exe = EXE_DEBUG

    # 编译
    if args.build or not exe.exists():
        if not build_project(args.config):
            sys.exit(1)

    if not exe.exists():
        print(f"[FAIL] 找不到可执行文件: {exe}")
        print(f"       请用 --build 先编译，或用 --exe 指定路径")
        sys.exit(1)

    print(f"\n[INFO] 可执行文件: {exe}")
    print(f"[INFO] 文件大小:   {exe.stat().st_size / 1024:.1f} KB")

    # 测量进程创建开销基线
    baseline_ms = None
    if args.baseline:
        baseline_exe = build_baseline()
        if baseline_exe:
            warmup(baseline_exe, min(args.warmup, 5))
            baseline_times = run_sequential(baseline_exe, min(args.runs, 200))
            if baseline_times:
                baseline_ms = statistics.median(baseline_times) * 1000
                print(f"\n[INFO] 进程创建基线 (中位数): {baseline_ms:.3f} ms")

    # 预热
    warmup(exe, args.warmup)

    # 顺序测试
    seq_times = run_sequential(exe, args.runs)
    report(seq_times, f"顺序测试 ({args.runs} runs)", baseline_ms)

    # 并发测试 (可选)
    if args.conc:
        conc_times = run_concurrent(exe, args.runs, args.workers)
        report(conc_times, f"并发测试 ({args.runs} runs, {args.workers} threads)")

    print(f"\n{'='*60}")
    print(f"  [DONE] 测试完成")
    print(f"{'='*60}\n")


if __name__ == "__main__":
    main()
