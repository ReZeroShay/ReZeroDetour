#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ReZeroDetour/ReZeroDetour.h>

using namespace ReZeroDetour;

// ============================================================
// Test helpers
// ============================================================

static std::size_t g_side_effect = 0;
static std::int64_t g_last_arg = 0;
static std::vector<std::int64_t> g_args_log;

// ============================================================
// Test functions
// ============================================================

REZERO_NOINLINE int simple_add(int a, int b) {
    g_last_arg = (static_cast<std::int64_t>(a) << 32) | static_cast<std::uint32_t>(b);

    g_side_effect += 100;

    return a + b;
}

REZERO_NOINLINE void simple_void_noargs() { g_side_effect += 777; }

REZERO_NOINLINE int multi_param_add(int a, int b, int c, int d) {
    g_args_log.push_back((static_cast<std::int64_t>(a) << 48) | (static_cast<std::int64_t>(b) << 32) |
                         (static_cast<std::int64_t>(c) << 16) | static_cast<std::uint16_t>(d));

    g_side_effect += 100;

    return a + b + c + d;
}

REZERO_NOINLINE double float_return(double x, float y) {
    g_side_effect += static_cast<std::size_t>(x + y);

    return x * y;
}

REZERO_NOINLINE std::string string_return(const std::string &s1, std::string s2) {
    g_side_effect += s1.size() + s2.size();

    return s1 + s2;
}

REZERO_NOINLINE void multi_param_void(int a, char b, const char *c) {
    g_side_effect += static_cast<std::size_t>(a) + static_cast<std::size_t>(b) + std::strlen(c);
}

// 站点绑定测试用的两个目标函数。函数体故意写成不一样，避免 /OPT:ICF 把它们
// 折叠成同一个地址（那样一次打补丁会同时命中两个函数）。
REZERO_NOINLINE int site_add(int a, int b) {
    g_side_effect += 5;

    return a + b;
}

REZERO_NOINLINE int site_mul(int a, int b) {
    g_side_effect += 7;

    return a * b;
}

static int g_site_hook_a_calls = 0;
static int g_site_hook_b_calls = 0;

// ============================================================
// Replace
// ============================================================

TEST_CASE("InlineHook - replace hook") {
    g_side_effect = 0;
    g_last_arg = 0;

    REQUIRE(simple_add(3, 4) == 7);
    REQUIRE(g_side_effect == 100);

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 9000;

        g_last_arg = (static_cast<std::int64_t>(b) << 32) | static_cast<std::uint32_t>(a);

        return a * b;
    };

    auto hook = makeInlineHook(&simple_add, detour);

    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    int result = simple_add(3, 4);

    // Replace does not execute the original.
    REQUIRE(result == 12);
    REQUIRE(g_side_effect == 100 + 9000);
    REQUIRE(g_last_arg == ((static_cast<std::int64_t>(4) << 32) | static_cast<std::uint32_t>(3)));

    REQUIRE(hook->uninstall());

    // Original function is restored.
    REQUIRE(simple_add(5, 6) == 11);
    REQUIRE(g_side_effect == 100 + 9000 + 100);
}
// ============================================================
// Before
// ============================================================

TEST_CASE("InlineHook - before hook preserves original result") {
    g_side_effect = 0;
    g_last_arg = 0;

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 2000;

        // Before hook return value is ignored.
        return 999;
    };

    auto hook = makeInlineHook(&simple_add, detour);
    hook->mode(HookMode::Before);

    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    int result = simple_add(10, 20);

    // Before:
    //
    //     detour()
    //        ↓
    //     original()
    //        ↓
    //     original result
    //
    REQUIRE(result == 30);
    REQUIRE(g_side_effect == 2000 + 100);

    REQUIRE(hook->uninstall());
}

// ============================================================
// After
// ============================================================

TEST_CASE("InlineHook - after hook preserves original result") {
    g_side_effect = 0;

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 5000;

        // After hook return value is ignored.
        return 7777;
    };

    auto hook = makeInlineHook(&simple_add, detour);
    hook->mode(HookMode::After);

    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    int result = simple_add(1, 1);

    // After:
    //
    //     original()
    //        ↓
    //     detour()
    //        ↓
    //     original result
    //
    REQUIRE(result == 2);
    REQUIRE(g_side_effect == 100 + 5000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Void + Replace
// ============================================================

TEST_CASE("InlineHook - void replace hook") {
    g_side_effect = 0;

    auto detour = [](auto &) { g_side_effect += 30000; };

    auto hook = makeInlineHook(&simple_void_noargs, detour);
    hook->mode(HookMode::Replace);
    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    simple_void_noargs();

    // Original must not execute.
    REQUIRE(g_side_effect == 30000);

    REQUIRE(hook->uninstall());

    // Original works again.
    simple_void_noargs();

    REQUIRE(g_side_effect == 30000 + 777);
}

// ============================================================
// Void + Before
// ============================================================

TEST_CASE("InlineHook - void before hook") {
    g_side_effect = 0;

    auto detour = [](auto &) { g_side_effect += 30000; };

    auto hook = makeInlineHook(&simple_void_noargs, detour);
    hook->mode(HookMode::Before);
    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    simple_void_noargs();

    REQUIRE(g_side_effect == 30000 + 777);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Void + After
// ============================================================

TEST_CASE("InlineHook - void after hook") {
    g_side_effect = 0;

    auto detour = [](auto &) { g_side_effect += 30000; };

    auto hook = makeInlineHook(&simple_void_noargs, detour);
    hook->mode(HookMode::After);

    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    simple_void_noargs();

    // Original executes first, then detour.
    REQUIRE(g_side_effect == 777 + 30000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Multi parameters
// ============================================================

TEST_CASE("InlineHook - replace hook with multiple parameters") {
    g_side_effect = 0;
    g_args_log.clear();

    REQUIRE(multi_param_add(1, 2, 3, 4) == 10);
    REQUIRE(g_side_effect == 100);
    REQUIRE(g_args_log.size() == 1);

    auto detour = [](auto &, int a, int b, int c, int d) -> int {
        g_side_effect += 9000;

        return a * b * c * d;
    };

    auto hook = makeInlineHook(&multi_param_add, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    int result = multi_param_add(1, 2, 3, 4);

    REQUIRE(result == 24);

    // Replace executes detour only.
    REQUIRE(g_side_effect == 100 + 9000);

    // Original was not executed again.
    REQUIRE(g_args_log.size() == 1);

    REQUIRE(hook->uninstall());

    REQUIRE(multi_param_add(5, 6, 7, 8) == 26);

    REQUIRE(g_side_effect == 100 + 9000 + 100);
    REQUIRE(g_args_log.size() == 2);
}

// ============================================================
// Floating point
// ============================================================

TEST_CASE("InlineHook - before hook with floating point parameters") {
    g_side_effect = 0;

    REQUIRE(float_return(2.5, 3.0f) == 7.5);
    REQUIRE(g_side_effect == 5);

    auto detour = [](auto &, double x, float y) -> double {
        g_side_effect += 2000;

        // Ignored by Before.
        return x + y;
    };

    auto hook = makeInlineHook(&float_return, detour);
    hook->mode(HookMode::Before);

    REQUIRE(hook->install());

    double result = float_return(2.5, 3.0f);

    REQUIRE(result == 7.5);
    REQUIRE(g_side_effect == 5 + 2000 + 5);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Floating point + After
// ============================================================

TEST_CASE("InlineHook - after hook with floating point return") {
    g_side_effect = 0;

    auto detour = [](auto &, double x, float y) -> double {
        g_side_effect += 2000;

        // Ignored by After.
        return 999.0;
    };

    auto hook = makeInlineHook(&float_return, detour);
    hook->mode(HookMode::After);

    REQUIRE(hook->install());

    double result = float_return(2.5, 3.0f);

    REQUIRE(result == 7.5);

    // Original: +5
    // After hook: +2000
    REQUIRE(g_side_effect == 5 + 2000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// String return
// ============================================================

TEST_CASE("InlineHook - replace hook with string return") {
    g_side_effect = 0;

    auto detour = [](auto &, const std::string &s1, std::string s2) -> std::string {
        g_side_effect += 5000;

        return s2 + s1;
    };

    auto hook = makeInlineHook(&string_return, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    std::string result = string_return("hello", "world");

    REQUIRE(result == "worldhello");

    // Original must not execute.
    REQUIRE(g_side_effect == 5000);

    REQUIRE(hook->uninstall());

    std::string original = string_return("hello", "world");

    REQUIRE(original == "helloworld");
}

// ============================================================
// Void + multiple parameters
// ============================================================

TEST_CASE("InlineHook - replace void function with multiple parameters") {
    g_side_effect = 0;

    multi_param_void(10, 'A', "test");

    REQUIRE(g_side_effect == 10 + 65 + 4);

    auto detour = [](auto &, int a, char b, const char *c) { g_side_effect += 30000 + std::strlen(c); };

    auto hook = makeInlineHook(&multi_param_void, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    multi_param_void(10, 'A', "test");

    // Original does not execute.
    REQUIRE(g_side_effect == 10 + 65 + 4 + 30000 + 4);

    REQUIRE(hook->uninstall());

    multi_param_void(10, 'A', "test");

    REQUIRE(g_side_effect == 10 + 65 + 4 + 30000 + 4 + 10 + 65 + 4);
}

// ============================================================
// Hook type switching
// ============================================================

TEST_CASE("InlineHook - hook type switching") {
    g_side_effect = 0;

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 1000;

        return 999;
    };

    auto hook = makeInlineHook(&simple_add, detour);

    // Default type is Replace.
    REQUIRE(hook->install());

    int result = simple_add(1, 2);

    REQUIRE(result == 999);
    REQUIRE(g_side_effect == 1000);

    REQUIRE(hook->uninstall());

    // Change to Before.
    hook->mode(HookMode::Before);

    REQUIRE(hook->install());

    result = simple_add(1, 2);

    REQUIRE(result == 3);
    REQUIRE(g_side_effect == 1000 + 1000 + 100);

    REQUIRE(hook->uninstall());

    // Change to After.
    hook->mode(HookMode::After);

    REQUIRE(hook->install());

    result = simple_add(1, 2);

    REQUIRE(result == 3);
    REQUIRE(g_side_effect == 1000 + 1000 + 100 + 100 + 1000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Lvalue callable
//
// This specifically verifies:
//
// template <typename T>
// InlineHook(Function src, T&& f)
//
// and:
//
// std::forward<T>(f)
//
// ============================================================

TEST_CASE("InlineHook - lvalue callable") {
    g_side_effect = 0;

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 9000;

        return a * b;
    };

    // detour is an lvalue here.
    auto hook = makeInlineHook(&simple_add, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    int result = simple_add(3, 4);

    REQUIRE(result == 12);
    REQUIRE(g_side_effect == 9000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Temporary callable
//
// Verifies that rvalue lambdas are also accepted.
// ============================================================

TEST_CASE("InlineHook - rvalue callable") {
    g_side_effect = 0;

    auto hook = makeInlineHook(&simple_add, [](auto &, int a, int b) -> int {
        g_side_effect += 9000;
        return a * b;
    });
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    int result = simple_add(3, 4);

    REQUIRE(result == 12);
    REQUIRE(g_side_effect == 9000);

    REQUIRE(hook->uninstall());
}

// ============================================================
// Reentrancy
// ============================================================

TEST_CASE("InlineHook - reentrant hook falls back to trampoline") {
    g_side_effect = 0;

    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += 1000;

        return simple_add(a, b);
    };

    auto hook = makeInlineHook(&simple_add, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    int result = simple_add(1, 2);

    /*
     * First entry:
     *
     *     simple_add()
     *          ↓
     *       detour()
     *          ↓
     *     simple_add()
     *
     * The second entry is detected as re-entry.
     * It therefore calls the trampoline directly.
     *
     * Result:
     *
     *     detour + original
     */
    REQUIRE(result == 3);
    REQUIRE(g_side_effect == 1000 + 100);

    REQUIRE(hook->uninstall());
}

// ============================================================
// 线程不可重入
//
// 重入状态是「每线程一份活动栈」，而不是实例上的共享标志位：
//
//   同一线程递归进入同一个 hook -> 判定重入 -> 直接走 trampoline
//   不同线程同时进入同一个 hook  -> 各自独立，都正常执行 hook 逻辑
//
// 也就是不可重入的范围从「全局」缩小到了「线程内」。
// ============================================================

REZERO_NOINLINE int thread_target(int a, int b) { return a + b; }

static std::atomic<int> g_thread_first_entries{0};
static std::atomic<int> g_thread_detour_calls{0};
static std::atomic<int> g_thread_bad_results{0};
static std::atomic<int> g_thread_inside{0};
static std::atomic<int> g_thread_peak{0};

static auto WINAPI threadWorker(void *) -> DWORD {
    for (int i = 0; i < 100; ++i) {
        if (thread_target(i, i + 1) != 2 * i + 1) {
            g_thread_bad_results.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return 0;
}

TEST_CASE("InlineHook - reentry is suppressed per thread, not globally") {
    g_thread_first_entries = 0;
    g_thread_detour_calls = 0;
    g_thread_bad_results = 0;
    g_thread_inside = 0;
    g_thread_peak = 0;

    auto detour = [](auto &, int a, int b) -> int {
        // 记录「同时位于这个 hook 内部」的线程数峰值。旧实现下这个峰值恒为 1，
        // 因为有线程在里面时，其它线程会被实例上的共享标志位挡在 detour 之外。
        int inside = g_thread_inside.fetch_add(1, std::memory_order_acq_rel) + 1;
        int peak = g_thread_peak.load(std::memory_order_relaxed);
        while (peak < inside && !g_thread_peak.compare_exchange_weak(peak, inside, std::memory_order_acq_rel,
                                                                     std::memory_order_relaxed)) {
        }

        // first 是 thread_local，每个线程一份。本用例每次都新建 worker 线程，
        // 新线程拿到的一定是 true，所以重复运行也成立。
        //
        // 每个线程的第一次进入参与汇合：如果线程 B 因为实例上的共享标志位被误判
        // 为重入，它永远进不到这里，g_thread_first_entries 就会停在 1。
        static thread_local bool first = true;
        if (first) {
            first = false;
            g_thread_first_entries.fetch_add(1, std::memory_order_acq_rel);

            constexpr int SpinBudget = 20'000'000;
            for (int spin = 0; spin < SpinBudget && g_thread_first_entries.load(std::memory_order_acquire) < 2;
                 ++spin) {
                YieldProcessor();
            }
        }

        g_thread_detour_calls.fetch_add(1, std::memory_order_relaxed);

        // 按名字调用原函数 -> 本线程重入 -> 应该直接走 trampoline。
        int result = thread_target(a, b);

        g_thread_inside.fetch_sub(1, std::memory_order_acq_rel);

        return result;
    };

    auto *hook = makeInlineHook(&thread_target, detour);
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    HANDLE workers[2] = {};
    workers[0] = CreateThread(nullptr, 0, threadWorker, nullptr, 0, nullptr);
    workers[1] = CreateThread(nullptr, 0, threadWorker, nullptr, 0, nullptr);

    REQUIRE(workers[0] != nullptr);
    REQUIRE(workers[1] != nullptr);

    REQUIRE(WaitForMultipleObjects(2, workers, TRUE, 30000) == WAIT_OBJECT_0);

    // 两个线程都进入过同一个 hook：线程 A 停在 hook 里没有阻塞线程 B。
    CHECK(g_thread_first_entries.load() == 2);

    // 而且确实同时位于这个 hook 内部（旧实现下峰值恒为 1）。
    CHECK(g_thread_peak.load() >= 2);

    // 两个线程各 100 次调用，每次都应该执行 detour（递归的那次走 trampoline）。
    CHECK(g_thread_detour_calls.load() == 200);

    // 并发下返回值依然正确。
    CHECK(g_thread_bad_results.load() == 0);

    CloseHandle(workers[0]);
    CloseHandle(workers[1]);

    REQUIRE(hook->uninstall());

    // 卸载后原函数恢复。
    CHECK(thread_target(1, 2) == 3);
}

// ============================================================
// 实例传递：TLS 上下文
//
//     patch -> stub -> [保存实参寄存器] -> setPendingInstance(instance)
//           -> [恢复实参寄存器] -> dispatch -> [读走并清空交接槽]
//
// 实例身份在运行期确定（每实例一份 stub，stub 内嵌实例指针），所以：
//   - 不需要编译期站点身份（Tag），同一个 callable 类型可以有任意多个实例；
//   - TLS 的索引与偏移不在我们这边推导 —— 写入由 setPendingInstance() 交给编译器生成；
//   - 交接槽读走即清空，不会留下悬垂实例指针；
//   - stub 结束时 RSP 与实参寄存器都精确还原，所以栈参数与栈回溯都不受影响。
// ============================================================

TEST_CASE("InlineHook - leaves TEB ArbitraryUserPointer untouched") {
    g_side_effect = 0;

    auto *teb = reinterpret_cast<_NT_TIB *>(NtCurrentTeb());

    auto *saved = teb->ArbitraryUserPointer;

    // 旧实现会在入口 thunk 里往这个槽写实例指针，这里放一个哨兵盯着它。
    auto *sentinel = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0xA5A5A5A5));

    teb->ArbitraryUserPointer = sentinel;

    auto detour = [](auto &, int a, int b) -> int { return a * b; };

    auto hook = makeInlineHook(&simple_add, detour);

    REQUIRE(hook != nullptr);
    REQUIRE(hook->install());

    REQUIRE(simple_add(3, 4) == 12);

    // 进入 hook 之后，这个槽必须还是我们写进去的哨兵。
    CHECK(teb->ArbitraryUserPointer == sentinel);

    REQUIRE(hook->uninstall());

    CHECK(teb->ArbitraryUserPointer == sentinel);

    teb->ArbitraryUserPointer = saved;
}

TEST_CASE("InlineHook - nested hooks keep their own instance") {
    g_side_effect = 0;
    g_site_hook_a_calls = 0;
    g_site_hook_b_calls = 0;

    auto *hookB = makeInlineHook(&site_mul, [](auto &, int a, int b) -> int {
        ++g_site_hook_b_calls;

        // 重入自己 -> 按设计走 trampoline。
        return site_mul(a, b);
    });
    hookB->mode(HookMode::Replace);

    REQUIRE(hookB->install());

    auto *hookA = makeInlineHook(&site_add, [](auto &, int a, int b) -> int {
        ++g_site_hook_a_calls;

        // 先进入另一个站点，再重入自己。
        int nested = site_mul(a, b);

        return nested + site_add(a, b);
    });
    hookA->mode(HookMode::Replace);

    REQUIRE(hookA->install());

    // site_mul(3, 4) 重入 -> trampoline -> 12
    // site_add(3, 4) 重入 -> trampoline ->  7
    CHECK(site_add(3, 4) == 12 + 7);

    CHECK(g_site_hook_a_calls == 1);
    CHECK(g_site_hook_b_calls == 1);

    REQUIRE(hookA->uninstall());
    REQUIRE(hookB->uninstall());
}

TEST_CASE("InlineHook - one callable type can back multiple instances") {
    g_side_effect = 0;

    // 同一个闭包类型用在两个目标上：实例身份来自每实例的 thunk，
    // 所以不需要任何标签或宏，两个实例可以同时存在。
    auto detour = [](auto &, int a, int b) -> int {
        g_side_effect += a + b;

        return 0;
    };

    auto *hookAdd = makeInlineHook(&site_add, detour);
    auto *hookMul = makeInlineHook(&site_mul, detour);

    REQUIRE(hookAdd != nullptr);
    REQUIRE(hookMul != nullptr);

    hookAdd->mode(HookMode::Before);
    hookMul->mode(HookMode::Before);

    REQUIRE(hookAdd->install());
    REQUIRE(hookMul->install());

    CHECK(site_add(1, 2) == 3);
    CHECK(site_mul(1, 2) == 2);

    // site_add: detour +3, 原函数 +5
    // site_mul: detour +3, 原函数 +7
    CHECK(g_side_effect == 3 + 5 + 3 + 7);

    REQUIRE(hookAdd->uninstall());

    // 卸载 A 完全不影响 B。
    g_side_effect = 0;

    CHECK(site_add(1, 2) == 3);
    CHECK(g_side_effect == 5);

    CHECK(site_mul(1, 2) == 2);
    CHECK(g_side_effect == 5 + 3 + 7);

    REQUIRE(hookMul->uninstall());
}

// ============================================================
// TLS 上下文的设置与恢复
//
//   设置：入口 thunk 写交接槽 -> dispatch 读走并清空
//   恢复：ContextGuard 在 dispatch 入口压活动栈，在每条退出路径上弹栈
// ============================================================

static void *g_outerHook = nullptr;
static void *g_innerHook = nullptr;

static bool g_outerPendingCleared = false;
static bool g_innerPendingCleared = false;
static bool g_innerSeesOuter = false;
static bool g_innerSeesInner = false;
static bool g_innerPoppedAfterReturn = false;
static bool g_outerStillActiveAfterInner = false;

// ============================================================
// stub 必须原样保留实参
//
// stub 中间要 call 一次 setPendingInstance，所以它必须自己保存/恢复易失实参寄存器。
// 这里用「4 个浮点 + 2 个整数」的目标函数同时覆盖 XMM0-3 与栈参数两条路径；
// 已有的 string_return 用例覆盖隐藏返回指针（占用 RCX）。
// ============================================================

REZERO_NOINLINE double mixed_param_target(double a, double b, double c, double d, int e, int f) {
    g_side_effect += static_cast<std::size_t>(e + f);

    return (a + b) * (c + d) + e - f;
}

TEST_CASE("InlineHook - entry stub preserves argument registers") {
    g_side_effect = 0;

    // 期望值由未打补丁的调用给出
    const double expected = mixed_param_target(1.5, 2.5, 3.5, 4.5, 5, 6);
    const std::size_t expectedSideEffect = g_side_effect;

    static double seenA = 0.0;
    static double seenB = 0.0;
    static double seenC = 0.0;
    static double seenD = 0.0;
    static int seenE = 0;
    static int seenF = 0;

    auto *hook =
        makeInlineHook(&mixed_param_target, [](auto &, double a, double b, double c, double d, int e, int f) -> double {
            seenA = a;
            seenB = b;
            seenC = c;
            seenD = d;
            seenE = e;
            seenF = f;

            return -1.0; // Before 模式下返回值被忽略
        });
    hook->mode(HookMode::Before);

    REQUIRE(hook->install());

    g_side_effect = 0;
    const double actual = mixed_param_target(1.5, 2.5, 3.5, 4.5, 5, 6);

    // detour 收到的实参必须逐位一致：XMM0-3 与栈参数都没被 stub 破坏
    CHECK(seenA == 1.5);
    CHECK(seenB == 2.5);
    CHECK(seenC == 3.5);
    CHECK(seenD == 4.5);
    CHECK(seenE == 5);
    CHECK(seenF == 6);

    // 原函数按原样执行，返回值与副作用都对
    CHECK(actual == expected);
    CHECK(g_side_effect == expectedSideEffect);

    REQUIRE(hook->uninstall());
}

TEST_CASE("InlineHook - TLS context is set on entry and restored on exit") {
    g_side_effect = 0;
    g_outerPendingCleared = false;
    g_innerPendingCleared = false;
    g_innerSeesOuter = false;
    g_innerSeesInner = false;
    g_innerPoppedAfterReturn = false;
    g_outerStillActiveAfterInner = false;

    auto *inner = makeInlineHook(&site_mul, [](auto &, int a, int b) -> int {
        // dispatch 一进门就把交接槽读走并清空了
        g_innerPendingCleared = (detail::tlsCurrentHookInstance == nullptr);
        // 内层进入时，外层仍在本线程的活动栈上
        g_innerSeesOuter = detail::ActiveHooks.contains(g_outerHook);
        g_innerSeesInner = detail::ActiveHooks.contains(g_innerHook);

        // 重入自己 -> 按设计走 trampoline
        return site_mul(a, b);
    });
    g_innerHook = inner;
    inner->mode(HookMode::Replace);
    REQUIRE(inner->install());

    auto *outer = makeInlineHook(&site_add, [](auto &, int a, int b) -> int {
        g_outerPendingCleared = (detail::tlsCurrentHookInstance == nullptr);

        int nested = site_mul(a, b);

        // 内层返回后：内层必须已经弹掉，外层还留着 —— 恢复是精确的
        g_innerPoppedAfterReturn = !detail::ActiveHooks.contains(g_innerHook);
        g_outerStillActiveAfterInner = detail::ActiveHooks.contains(g_outerHook);

        return nested + site_add(a, b);
    });
    g_outerHook = outer;
    outer->mode(HookMode::Replace);
    REQUIRE(outer->install());

    // site_mul 重入 -> trampoline -> 12；site_add 重入 -> trampoline -> 7
    CHECK(site_add(3, 4) == 12 + 7);

    // 进入时：交接槽已被读走，活动栈上是外层 + 内层
    CHECK(g_outerPendingCleared);
    CHECK(g_innerPendingCleared);
    CHECK(g_innerSeesOuter);
    CHECK(g_innerSeesInner);

    // 内层退出时：只弹掉内层，外层保留
    CHECK(g_innerPoppedAfterReturn);
    CHECK(g_outerStillActiveAfterInner);

    REQUIRE(outer->uninstall());
    REQUIRE(inner->uninstall());

    // 全部退出后：活动栈与交接槽都回到空，不留悬垂状态
    CHECK_FALSE(detail::ActiveHooks.contains(g_outerHook));
    CHECK_FALSE(detail::ActiveHooks.contains(g_innerHook));
    CHECK(detail::tlsCurrentHookInstance == nullptr);
}

// ============================================================
// 序言重定位
//
// /O2 下很常见的序言形状就是 `01 0D <disp32>`（add dword ptr [rip+disp32], ecx）。
// 如果只修正相对立即数、不修正 RIP 相对访存，trampoline 就会写到野地址 ——
// Release 构建下一调用就 AV。这里直接把字节喂给重定位器，断言位移被精确修正。
// ============================================================

TEST_CASE("Disassembler - relocates RIP-relative memory operands") {
    uint8_t source[16] = {};
    const std::int32_t originalDisplacement = 0x00012345;

    // add dword ptr [rip+disp32], ecx
    source[0] = 0x01;
    source[1] = 0x0D;
    std::memcpy(&source[2], &originalDisplacement, 4);
    source[6] = 0xC3; // ret：让解码在这里停下

    uint8_t trampolineBytes[128] = {};

    Disassembler disassembler;
    auto built = disassembler.buildTrampolineFromPrologue({source, sizeof(source)},
                                                          {trampolineBytes, sizeof(trampolineBytes)}, 5);
    REQUIRE(built.is_ok());

    // 整条 add 被搬走（6 字节 >= 最小序言长度 5）
    CHECK(*built == 6);
    CHECK(trampolineBytes[0] == 0x01);
    CHECK(trampolineBytes[1] == 0x0D);

    // 位移必须修正成"目标绝对地址不变"
    const auto sourceNext = reinterpret_cast<std::intptr_t>(source) + 6;
    const auto trampolineNext = reinterpret_cast<std::intptr_t>(trampolineBytes) + 6;
    const auto expectedTarget = sourceNext + originalDisplacement;

    std::int32_t relocated = 0;
    std::memcpy(&relocated, &trampolineBytes[2], 4);
    CHECK(trampolineNext + relocated == expectedTarget);
}

TEST_CASE("Disassembler - relocates relative branch immediates") {
    uint8_t source[16] = {};
    const std::int32_t originalDisplacement = 0x40;

    // jmp rel32
    source[0] = 0xE9;
    std::memcpy(&source[1], &originalDisplacement, 4);

    uint8_t trampolineBytes[128] = {};

    Disassembler disassembler;
    auto built = disassembler.buildTrampolineFromPrologue({source, sizeof(source)},
                                                          {trampolineBytes, sizeof(trampolineBytes)}, 5);
    REQUIRE(built.is_ok());
    CHECK(*built == 5);
    CHECK(trampolineBytes[0] == 0xE9);

    const auto sourceNext = reinterpret_cast<std::intptr_t>(source) + 5;
    const auto trampolineNext = reinterpret_cast<std::intptr_t>(trampolineBytes) + 5;

    std::int32_t relocated = 0;
    std::memcpy(&relocated, &trampolineBytes[1], 4);
    CHECK(trampolineNext + relocated == sourceNext + originalDisplacement);
}

// ============================================================
// Near allocation
// ============================================================

#ifdef _WIN32

static bool isPageExecutableReadWrite(void *ptr, std::size_t size = 0x1000) {
    MEMORY_BASIC_INFORMATION mbi{};

    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }

    return mbi.State == MEM_COMMIT && mbi.Protect == PAGE_EXECUTE_READWRITE && mbi.RegionSize >= size;
}

#endif

TEST_CASE("tryAllocateNear - null input") {
#ifdef _WIN32
    REQUIRE(detail::tryAllocateNear(nullptr) == nullptr);
#else
    SUCCEED("tryAllocateNear is currently Windows-specific.");
#endif
}

#ifdef _WIN32

TEST_CASE("tryAllocateNear - allocate near target") {
    std::uint8_t local = 0;

    auto *target = &local;

    auto *allocated = detail::tryAllocateNear(target);

    if (allocated == nullptr) {
        WARN("tryAllocateNear failed; "
             "this may happen because of address-space fragmentation.");

        return;
    }

    REQUIRE(isPageExecutableReadWrite(allocated));

    const auto targetAddress = reinterpret_cast<std::intptr_t>(target);

    const auto allocatedAddress = reinterpret_cast<std::intptr_t>(allocated);

    const auto distance = std::llabs(static_cast<long long>(allocatedAddress - targetAddress));

    // rel32 requires the target to be within
    // approximately ±2GB.
    REQUIRE(distance <= 0x7FFFFFFFLL);

    // VirtualAlloc page alignment.
    REQUIRE(reinterpret_cast<std::uintptr_t>(allocated) % 0x1000 == 0);

    VirtualFree(allocated, 0, MEM_RELEASE);
}

TEST_CASE("tryAllocateNear - executable memory") {
    std::uint8_t local = 0;

    auto *allocated = detail::tryAllocateNear(&local);

    if (allocated == nullptr) {
        WARN("tryAllocateNear failed.");
        return;
    }

    REQUIRE(isPageExecutableReadWrite(allocated));

    // ret
    allocated[0] = 0xC3;

    REQUIRE(allocated[0] == 0xC3);

    using VoidFn = void (*)();

    auto fn = reinterpret_cast<VoidFn>(allocated);

    // The allocated page should be executable.
    fn();

    VirtualFree(allocated, 0, MEM_RELEASE);
}

TEST_CASE("tryAllocateNear - repeated allocation") {
    std::uint8_t local = 0;

    constexpr int Count = 10;

    std::vector<void *> allocations;
    allocations.reserve(Count);

    for (int i = 0; i < Count; ++i) {
        auto *allocated = detail::tryAllocateNear(&local);

        if (allocated == nullptr) {
            WARN("tryAllocateNear failed during "
                 "repeated allocation.");

            break;
        }

        REQUIRE(isPageExecutableReadWrite(allocated));

        // Every allocation should be unique.
        for (void *previous : allocations) {
            CHECK(previous != allocated);
        }

        const auto targetAddress = reinterpret_cast<std::intptr_t>(&local);

        const auto allocatedAddress = reinterpret_cast<std::intptr_t>(allocated);

        const auto distance = std::llabs(static_cast<long long>(allocatedAddress - targetAddress));

        CHECK(distance <= 0x7FFFFFFFLL);

        allocations.push_back(allocated);
    }

    for (void *ptr : allocations) {
        VirtualFree(ptr, 0, MEM_RELEASE);
    }
}

#endif

// ============================================================
// 卸载：在飞调用计数与排空
//
// 入口 stub 最先执行 `lock inc [ActiveCalls]`，dispatch 的每条出口把它减回去；
// uninstall() 摘掉补丁之后等它归零，尽量保证"返回后不再有 detour 在跑"。
// ============================================================

TEST_CASE("InlineHook - active call counter is balanced") {
    g_side_effect = 0;

    auto *hook = makeInlineHook(&site_add, [](auto &, int a, int b) -> int { return a + b + 1; });
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());
    CHECK(hook->activeCallCount() == 0);

    for (int i = 0; i < 10; ++i) {
        (void)site_add(i, i);
    }

    // 自增（stub）与自减（dispatch）必须配平：漏了任何一边，这里就不是 0，
    // 而且下面的 uninstall() 会一路等到超时才返回。
    CHECK(hook->activeCallCount() == 0);

    REQUIRE(hook->uninstall());
    CHECK(hook->activeCallCount() == 0);
}

static std::atomic<bool> g_drainInsideDetour{false};
static std::atomic<bool> g_drainDetourFinished{false};

static auto WINAPI drainWorker(void *) -> DWORD {
    (void)site_add(1, 2);
    return 0;
}

TEST_CASE("InlineHook - uninstall waits for in-flight calls") {
    g_side_effect = 0;
    g_drainInsideDetour = false;
    g_drainDetourFinished = false;

    auto *hook = makeInlineHook(&site_add, [](auto &, int a, int b) -> int {
        g_drainInsideDetour.store(true, std::memory_order_release);

        // 拖住这次调用（150ms，远小于 2s 的排空上限），好让主线程在"它在飞"时发起卸载。
        // 刻意用有界自旋而不是等信号：否则 uninstall() 等它、它又等主线程，直接死锁。
        const std::uint64_t until = GetTickCount64() + 150;
        while (GetTickCount64() < until) {
            YieldProcessor();
        }

        g_drainDetourFinished.store(true, std::memory_order_release);
        return a + b;
    });
    hook->mode(HookMode::Replace);
    REQUIRE(hook->install());

    HANDLE worker = CreateThread(nullptr, 0, drainWorker, nullptr, 0, nullptr);
    REQUIRE(worker != nullptr);

    // 等它真的进了 detour，再发起卸载
    const std::uint64_t deadline = GetTickCount64() + 2000;
    while (!g_drainInsideDetour.load(std::memory_order_acquire) && GetTickCount64() < deadline) {
        SwitchToThread();
    }
    REQUIRE(g_drainInsideDetour.load(std::memory_order_acquire));

    // 进入 detour 说明 stub 的计数已经加上去了
    CHECK(hook->activeCallCount() >= 1);

    REQUIRE(hook->uninstall());

    // 关键断言：uninstall() 返回时那次调用必须已经结束。
    // 去掉排空等待的话，这里会是 false（卸载在 detour 还在自旋时就返回了）。
    CHECK(g_drainDetourFinished.load(std::memory_order_acquire));
    CHECK(hook->activeCallCount() == 0);

    WaitForSingleObject(worker, 10000);
    CloseHandle(worker);
}

static Result<void, ErrorCode> g_uninstallFromDetour{};

TEST_CASE("InlineHook - uninstall from inside the detour does not deadlock") {
    g_side_effect = 0;
    g_uninstallFromDetour = Result<void, ErrorCode>{};

    // detour 的第一个参数就是 hook 自己，所以"在 detour 里卸载自己"可以写得很自然
    auto *hook = makeInlineHook(&site_add, [](auto &self, int a, int b) -> int {
        // 当前线程正在这个 hook 里 -> 计数包含自己：排空等待必须立刻放弃，而不是死锁
        g_uninstallFromDetour = self.uninstall();
        return a + b;
    });
    hook->mode(HookMode::Replace);
    REQUIRE(hook->install());

    // 不挂死是第一层断言（排空上限 2s，若死等这条用例就会卡住），返回值是第二层
    (void)site_add(1, 2);

    REQUIRE(g_uninstallFromDetour.is_err());
    CHECK(g_uninstallFromDetour.error() == ErrorCode::UninstallTimedOut);

    // 超时同样完成了卸载本身：补丁已摘、状态已复位 -> 原函数恢复、计数归零
    CHECK(hook->activeCallCount() == 0);
    g_side_effect = 0;
    CHECK(site_add(3, 4) == 7);
    CHECK(g_side_effect == 5);

    // 再次 uninstall 是空操作（Installed 已经复位）
    REQUIRE(hook->uninstall());
}

// ============================================================
// detour 里调用 self.trampoline()
//
// 这是"在 detour 里调原函数"的正路：trampoline() 是搬走的序言 + 跳回原函数剩余部分，
// 它**不经过补丁**，所以既不会触发重入检测，也不会再进一次 detour。
// （对比：[reentrant] 那条用例是"按函数名调原函数"，那次会进补丁、靠重入守卫兜住。）
// ============================================================

static int g_trampolineDetourCalls = 0;

TEST_CASE("InlineHook - detour calls the original through self.trampoline()") {
    g_side_effect = 0;
    g_trampolineDetourCalls = 0;

    // 期望值由未打补丁的调用给出
    const int baseline = site_add(3, 4);
    const std::size_t baselineSideEffect = g_side_effect;

    auto *hook = makeInlineHook(&site_add, [](auto &self, int a, int b) -> int {
        ++g_trampolineDetourCalls;

        // 通过 trampoline 调原函数：原函数照常执行（副作用 +5），返回值可被改写
        const int original = self.trampoline(a, b);
        return original * 10;
    });
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    g_side_effect = 0;
    const int hooked = site_add(3, 4);

    CHECK(baseline == 7);
    CHECK(baselineSideEffect == 5);
    CHECK(hooked == baseline * 10);             // trampoline 把原函数的返回值交回来了
    CHECK(g_side_effect == baselineSideEffect); // 而且原函数真的执行了

    // 关键：走的不是补丁，所以 detour 只应该被进一次
    // （按名字调原函数的话这里会是 2 —— 第二次靠重入守卫走 trampoline）
    CHECK(g_trampolineDetourCalls == 1);

    REQUIRE(hook->uninstall());

    g_side_effect = 0;
    CHECK(site_add(3, 4) == 7);
}

TEST_CASE("InlineHook - trampoline forwards all arguments") {
    g_side_effect = 0;

    // 4 个浮点 + 2 个栈实参：正好覆盖 XMM0-3 与栈参数两条通道
    const double baseline = mixed_param_target(1.5, 2.5, 3.5, 4.5, 5, 6);
    const std::size_t baselineSideEffect = g_side_effect;

    auto *hook = makeInlineHook(&mixed_param_target,
                                [](auto &self, double a, double b, double c, double d, int e, int f) -> double {
                                    // 参数原样转交给 trampoline
                                    return self.trampoline(a, b, c, d, e, f) + 1.0;
                                });
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    g_side_effect = 0;
    const double hooked = mixed_param_target(1.5, 2.5, 3.5, 4.5, 5, 6);

    CHECK(hooked == baseline + 1.0);
    CHECK(g_side_effect == baselineSideEffect); // 原函数按原样执行

    REQUIRE(hook->uninstall());
}

TEST_CASE("InlineHook - trampoline works for void returns") {
    g_side_effect = 0;

    // 注意：这里必须显式写 `-> void`。
    // 约束检查 HookCallable<F, InlineHook, R, Args...> 需要知道 lambda 的返回类型；
    // 若靠推导，编译器就得实例化 lambda 体，而那时 InlineHook 还没定义完 ——
    // 一旦访问 self 的成员就是 C2027（使用了未定义类型）。
    // 即：凡是"返回类型靠推导 + 要访问 self"的 detour，都得显式写返回类型。
    auto *hook = makeInlineHook(&simple_void_noargs, [](auto &self) -> void {
        self.trampoline();  // void 返回：直接转发
        g_side_effect += 1; // detour 自己的动作
    });
    hook->mode(HookMode::Replace);

    REQUIRE(hook->install());

    g_side_effect = 0;
    simple_void_noargs();

    CHECK(g_side_effect == 777 + 1);

    REQUIRE(hook->uninstall());
}

// 注：这里曾经有一条 "卸载后 trampoline 仍可调用" 的用例。那条保证依赖
// "卸载时不释放可执行页"；现在 releaseExecutionMemory() 一律释放并清空
// Thunk / Trampoline 的指针，`trampoline()` 在卸载后不再可用（要调原函数直接调 Src），
// 所以该用例已删除。卸载后"在飞调用"的安全性由 [drain] 那组用例覆盖。

// ============================================================
// 补丁表：字节层面的统一管理
//
// 这一层不认识 hook 的签名，只认「目标地址 + 原始字节」，可以脱离
// InlineHook 单独测：登记、写入、还原、重复目标、批量还原。
// ============================================================

TEST_CASE("PatchTable - rejects a second patch on the same target") {
    PatchTable table;

    auto *target = reinterpret_cast<void *>(&site_add);
    auto *destination = reinterpret_cast<void *>(&site_mul);

    auto first = table.add(target, kRel32JumpSize, destination);
    REQUIRE(first);
    CHECK(table.find(target) == *first);

    // 同一个目标第二次登记必须被拒：否则后装的那条会把前一条的 jmp
    // 当成"原始字节"备份下来，卸载顺序一反就还原成野跳转。
    auto second = table.add(target, kRel32JumpSize, destination);
    REQUIRE(second.is_err());
    CHECK(second.error() == ErrorCode::AlreadyHooked);
    CHECK(table.size() == 1);
}

TEST_CASE("PatchTable - install writes the jump and restore puts the bytes back") {
    PatchTable table;

    auto *target = reinterpret_cast<void *>(&site_add);
    auto *destination = reinterpret_cast<void *>(&site_mul);

    std::uint8_t original[kRel32JumpSize] = {};
    std::memcpy(original, target, kRel32JumpSize);

    auto id = table.add(target, kRel32JumpSize, destination);
    REQUIRE(id);

    REQUIRE(table.install(*id));
    CHECK(*static_cast<const std::uint8_t *>(target) == 0xE9);

    REQUIRE(table.restore(*id));
    CHECK(std::memcmp(original, target, kRel32JumpSize) == 0);

    // 还原之后记录就作废了，同一目标可以重新登记
    CHECK(table.size() == 0);
    CHECK(table.find(target) == PatchTable::InvalidPatchId);

    // 重复还原不是错误
    CHECK(table.restore(*id));
}

TEST_CASE("PatchTable - restoreAll restores every patch and reports the count") {
    PatchTable table;

    auto *add = reinterpret_cast<void *>(&site_add);
    auto *mul = reinterpret_cast<void *>(&site_mul);

    std::uint8_t original_add[kRel32JumpSize] = {};
    std::uint8_t original_mul[kRel32JumpSize] = {};
    std::memcpy(original_add, add, kRel32JumpSize);
    std::memcpy(original_mul, mul, kRel32JumpSize);

    auto first = table.add(add, kRel32JumpSize, mul);
    auto second = table.add(mul, kRel32JumpSize, add);
    REQUIRE(first);
    REQUIRE(second);

    REQUIRE(table.install(*first));
    REQUIRE(table.install(*second));
    CHECK(table.size() == 2);

    const auto report = table.restoreAll();
    CHECK(report.Restored == 2);
    CHECK(report.Failed == 0);
    CHECK_FALSE(report.HasError);
    CHECK(table.size() == 0);

    CHECK(std::memcmp(original_add, add, kRel32JumpSize) == 0);
    CHECK(std::memcmp(original_mul, mul, kRel32JumpSize) == 0);

    // 批量还原后表是空的，两个目标都能重新登记
    CHECK(table.add(add, kRel32JumpSize, mul));
    CHECK(table.add(mul, kRel32JumpSize, add));
}
// ============================================================
// Thunk：生成的 stub 与手写版本的字节布局一致
//
// 指令长度是这条链上最容易悄悄变的东西（例如 mov imm64 退化成 7 字节的
// imm32 形式），布局一漂，stub 内偏移、容量判断就全错。这里把每个偏移钉死。
// ============================================================

TEST_CASE("Thunk - generated stub keeps the expected encoding") {
    std::uint8_t stub[256] = {};

    auto *instance = reinterpret_cast<void *>(0x0000112233445566ull);
    auto *entry = reinterpret_cast<void *>(0x0000778899AABBCCull);
    auto *counter = reinterpret_cast<void *>(0x0000DDEEFF001122ull);

    Thunk thunk;
    REQUIRE(thunk.at(stub).build(instance, entry, counter));
    CHECK(thunk.size() == 116);

    // mov r10, imm64(instance)
    CHECK(std::memcmp(&stub[0], "\x49\xBA", 2) == 0);
    CHECK(std::memcmp(&stub[2], &instance, 8) == 0);

    // mov r11, imm64(activeCalls)
    CHECK(std::memcmp(&stub[10], "\x49\xBB", 2) == 0);
    CHECK(std::memcmp(&stub[12], &counter, 8) == 0);

    // lock inc dword ptr [r11]
    CHECK(std::memcmp(&stub[20], "\xF0\x41\xFF\x03", 4) == 0);

    // push rcx / rdx / r8 / r9 / rax
    CHECK(std::memcmp(&stub[24], "\x51\x52\x41\x50\x41\x51\x50", 7) == 0);

    // sub rsp, 0x60
    CHECK(std::memcmp(&stub[31], "\x48\x83\xEC\x60", 4) == 0);

    // movups [rsp+20/30/40/50], xmm0..3
    CHECK(std::memcmp(&stub[35], "\x0F\x11\x44\x24\x20", 5) == 0);
    CHECK(std::memcmp(&stub[40], "\x0F\x11\x4C\x24\x30", 5) == 0);
    CHECK(std::memcmp(&stub[45], "\x0F\x11\x54\x24\x40", 5) == 0);
    CHECK(std::memcmp(&stub[50], "\x0F\x11\x5C\x24\x50", 5) == 0);

    // mov rcx, r10
    CHECK(std::memcmp(&stub[55], "\x4C\x89\xD1", 3) == 0);

    // mov r11, imm64(bindCurrentHook) ; call r11
    CHECK(std::memcmp(&stub[58], "\x49\xBB", 2) == 0);
    auto setter = reinterpret_cast<void *>(&detail::bindCurrentHook);
    CHECK(std::memcmp(&stub[60], &setter, 8) == 0);
    CHECK(std::memcmp(&stub[68], "\x41\xFF\xD3", 3) == 0);

    // movups xmm0..3, [rsp+20/30/40/50]
    CHECK(std::memcmp(&stub[71], "\x0F\x10\x44\x24\x20", 5) == 0);
    CHECK(std::memcmp(&stub[76], "\x0F\x10\x4C\x24\x30", 5) == 0);
    CHECK(std::memcmp(&stub[81], "\x0F\x10\x54\x24\x40", 5) == 0);
    CHECK(std::memcmp(&stub[86], "\x0F\x10\x5C\x24\x50", 5) == 0);

    // add rsp, 0x60
    CHECK(std::memcmp(&stub[91], "\x48\x83\xC4\x60", 4) == 0);

    // pop rax / r9 / r8 / rdx / rcx
    CHECK(std::memcmp(&stub[95], "\x58\x41\x59\x41\x58\x5A\x59", 7) == 0);

    // jmp qword ptr [rip+0] ; <quad entry>
    CHECK(std::memcmp(&stub[102], "\xFF\x25\x00\x00\x00\x00", 6) == 0);
    CHECK(std::memcmp(&stub[108], &entry, 8) == 0);
}
// ============================================================
// Patch：两条跳转也由 CodeGen 编码，长度固定
// ============================================================

TEST_CASE("Patch - rel32 jump is 5 bytes and carries the right displacement") {
    std::uint8_t buffer[kRel32JumpSize] = {};

    const auto src = reinterpret_cast<intptr_t>(&buffer[0]);
    const auto dest = src + 0x1234;

    REQUIRE(makeRel32JumpAt({buffer, kRel32JumpSize}, src, dest));

    CHECK(buffer[0] == 0xE9);

    std::int32_t displacement = 0;
    std::memcpy(&displacement, &buffer[1], 4);
    CHECK(displacement == static_cast<std::int32_t>(dest - (src + kRel32JumpSize)));

    // 位移装不进 int32 时必须报错，而不是截断
    CHECK(makeRel32JumpAt({buffer, kRel32JumpSize}, src, src + 0x80000005ll).is_err());
}

TEST_CASE("Patch - absolute jump is 14 bytes for any address") {
    // 常见情况：低 32 位够大，push 直接就是 5 字节的 68 形式
    {
        std::uint8_t buffer[kAbsoluteJumpSize] = {};
        const auto dest = static_cast<uintptr_t>(0x00007FF67524C930ull);

        REQUIRE(makeAbsoluteJumpAt({buffer, kAbsoluteJumpSize}, dest));

        CHECK(buffer[0] == 0x68);
        std::uint32_t low = 0;
        std::memcpy(&low, &buffer[1], 4);
        CHECK(low == static_cast<std::uint32_t>(dest));

        CHECK(buffer[5] == 0xC7);
        CHECK(buffer[6] == 0x44);
        CHECK(buffer[7] == 0x24);
        CHECK(buffer[8] == 0x04);

        std::uint32_t high = 0;
        std::memcpy(&high, &buffer[9], 4);
        CHECK(high == static_cast<std::uint32_t>(dest >> 32));

        CHECK(buffer[13] == 0xC3);
    }

    // 低 32 位很小：编码器会选 2 字节的 6A 形式，pushImm32 用 NOP 补回 5 字节，
    // 保证这条跳转永远是 14 字节
    {
        std::uint8_t buffer[kAbsoluteJumpSize] = {};
        const auto dest = static_cast<uintptr_t>(0x0000000000000021ull);

        REQUIRE(makeAbsoluteJumpAt({buffer, kAbsoluteJumpSize}, dest));

        CHECK(buffer[0] == 0x6A);
        CHECK(buffer[1] == 0x21);
        CHECK(buffer[2] == 0x90);
        CHECK(buffer[3] == 0x90);
        CHECK(buffer[4] == 0x90);
        CHECK(buffer[5] == 0xC7);
        CHECK(buffer[13] == 0xC3);
    }
}

// ============================================================
// IntrusiveList：补丁表用的侵入式单链表
// ============================================================

TEST_CASE("IntrusiveList - push, iterate, erase while iterating") {
    struct Node {
        IntrusiveListNode Link;
        int Value = 0;
    };

    IntrusiveList<Node, &Node::Link> list;
    Node a;
    Node b;
    Node c;
    Node d;
    a.Value = 1;
    b.Value = 2;
    c.Value = 3;
    d.Value = 4;

    list.pushFront(a);
    list.pushFront(b);
    list.pushFront(c);
    list.pushBack(d);

    CHECK(list.size() == 4);
    CHECK(list.front().Value == 3);
    CHECK(list.back().Value == 4);

    // 头插顺序：3, 2, 1，再 pushBack 一个 4
    const int expected[] = {3, 2, 1, 4};
    int index = 0;
    for (const Node &node : list) {
        CHECK(node.Value == expected[index]);
        ++index;
    }
    CHECK(index == 4);

    // 边遍历边删：删掉偶数，erase 返回下一个迭代器
    for (auto it = list.begin(); it != list.end();) {
        if (it->Value % 2 == 0) {
            it = list.erase(it);
        } else {
            ++it;
        }
    }
    CHECK(list.size() == 2);
    for (const Node &node : list) {
        CHECK(node.Value % 2 == 1);
    }

    // 按引用删除，删过的再删返回 false
    CHECK(list.erase(c));
    CHECK_FALSE(list.erase(c));
    CHECK(list.size() == 1);
    CHECK(list.front().Value == 1);

    list.clear();
    CHECK(list.empty());
    CHECK(list.size() == 0);
}
