#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <concepts>
#include <utility>
#include <functional>
#include <intrin.h>
#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/Error.h>
#include <ReZeroDetour/TlsContext.h>
#include <ReZeroDetour/Trampoline.h>
#include <ReZeroDetour/Thunk.h>
#include <ReZeroDetour/Patch.h>
#include <ReZeroDetour/Disassembler.h>
#include <SmpAllocator/SmpAllocator.h>

#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>

namespace ReZeroDetour {

    namespace detail {
        // 每个 hook 的可执行页内布局：trampoline 在页首，入口 stub 从这里开始
        inline constexpr std::size_t kStubOffsetInPage = 200;

        inline std::uint8_t *tryAllocateNear(std::uint8_t *nearest) {
            if (!nearest)
                return nullptr;

            const size_t allocSize = 0x1000;    // 分配一个页 (4KB)
            const uintptr_t range = 0x7FFFFFFF; // 2GB 限制 (INT_MAX)
            const uintptr_t align = 0x10000;    // Windows 分配粒度通常为 64KB

            uintptr_t base = reinterpret_cast<uintptr_t>(nearest);

            // 5 字节补丁从 base 跳到 stub（页内偏移 kStubOffsetInPage）：rel32 位移必须
            // 装进 int32，否则打补丁那一步会把位移截断成野地址。搜索时就把候选页卡进
            // 这个窗口，而不是只在事后截断。
            constexpr uintptr_t patchSize = 5;
            const auto patchFits = [&](uintptr_t candidate) noexcept {
                const intptr_t displacement =
                    static_cast<intptr_t>(candidate + kStubOffsetInPage) - static_cast<intptr_t>(base + patchSize);
                return displacement >= INT32_MIN && displacement <= INT32_MAX;
            };

            // 计算搜索边界，注意防止溢出
            uintptr_t minAddr = (base > range) ? (base - range) : 0x10000;
            uintptr_t maxAddr = (UINTPTR_MAX - base > range) ? (base + range) : UINTPTR_MAX;

            MEMORY_BASIC_INFORMATION mbi;

            // 1. 向上搜索 (从 nearest 到 maxAddr)
            for (uintptr_t addr = base; addr < maxAddr;) {
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
                    break;

                if (mbi.State == MEM_FREE && mbi.RegionSize >= allocSize) {
                    // 在空闲区内寻找一个对齐的起始点
                    uintptr_t candidate = (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + (align - 1)) & ~(align - 1);
                    if (candidate + allocSize <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize &&
                        patchFits(candidate)) {
                        void *res = VirtualAlloc(reinterpret_cast<void *>(candidate), allocSize,
                                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                        if (res)
                            return static_cast<std::uint8_t *>(res);
                    }
                }
                addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            }

            // 2. 向下搜索 (从 nearest 到 minAddr)
            for (uintptr_t addr = base; addr > minAddr;) {
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)))
                    break;

                // 向下搜索时，VirtualQuery 得到的是当前块的基址，我们需要跳到前一个块
                uintptr_t prevAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) - 1;

                if (mbi.State == MEM_FREE && mbi.RegionSize >= allocSize) {
                    // 尽量分配在空闲区的末尾，这样离目标更近
                    uintptr_t candidate =
                        (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - allocSize) & ~(align - 1);
                    if (candidate >= reinterpret_cast<uintptr_t>(mbi.BaseAddress) && patchFits(candidate)) {
                        void *res = VirtualAlloc(reinterpret_cast<void *>(candidate), allocSize,
                                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                        if (res)
                            return static_cast<std::uint8_t *>(res);
                    }
                }
                if (prevAddr < minAddr)
                    break;
                addr = prevAddr;
            }

            return nullptr;
        }
        class CriticalSection {
          public:
            CriticalSection() { InitializeCriticalSection(&Cs); }

            ~CriticalSection() { DeleteCriticalSection(&Cs); }

            CriticalSection(const CriticalSection &) = delete;
            CriticalSection &operator=(const CriticalSection &) = delete;

            void lock() noexcept { EnterCriticalSection(&Cs); }

            void unlock() noexcept { LeaveCriticalSection(&Cs); }

          private:
            CRITICAL_SECTION Cs{};
        };

        template <typename T>
        concept LockLike = requires(T &lock) {
            lock.lock();
            lock.unlock();
        };
        template <LockLike T> class LockGuard {
          public:
            explicit LockGuard(T &lock) : Lock(lock) { Lock.lock(); }

            ~LockGuard() { Lock.unlock(); }

            LockGuard(const LockGuard &) = delete;
            LockGuard &operator=(const LockGuard &) = delete;

          private:
            T &Lock;
        };

        // 线程活动栈 (thread-local active stack)
        //
        // 每个线程一份，记录本线程当前正在执行哪些 hook 实例。它同时解决两件事：
        //
        //   1. 重入检测：同一个实例在本线程已经活动 -> 说明是递归调用，直接走 trampoline。
        //   2. 线程隔离：栈是每线程独立的，线程 A 停在 hook 内部不会让线程 B 被误判为重入。
        //
        // 所以语义是「线程内不可重入」，而不是「全局不可重入」：同一个 hook 可以同时
        // 在多个线程上执行，只有同一线程的递归才被抑制。
        //
        // 为什么是栈而不是一个标志位：A 的 detour 进入被 B hook 的函数时，B 并不在栈上，
        // 于是 B 正常执行；如果只有一个标志位，B 会被误判为重入而整个失效。
        class ThreadActiveHookStack {
          public:
            static constexpr std::size_t Capacity = 64;

            // 本线程当前是否已经在这个实例里执行
            [[nodiscard]] auto contains(const void *instance) const noexcept -> bool {
                for (std::size_t i = 0; i < Depth; ++i) {
                    if (Slots[i] == instance) {
                        return true;
                    }
                }
                return false;
            }

            void push(void *instance) noexcept {
                REZERO_ASSERT(Depth < Capacity);

                if (Depth < Capacity) {
                    Slots[Depth++] = instance;
                }
            }

            auto pop(void *instance) noexcept -> void {
                for (std::size_t i = Depth; i-- > 0;) {
                    if (Slots[i] != instance) {
                        continue;
                    }
                    for (std::size_t j = i; j + 1 < Depth; ++j) {
                        Slots[j] = Slots[j + 1];
                    }
                    Depth -= 1;
                    return;
                }
            }

          private:
            void *Slots[Capacity]{};
            std::size_t Depth = 0;
        };

        static_assert(std::is_trivially_destructible_v<ThreadActiveHookStack>);

        inline thread_local ThreadActiveHookStack ActiveHooks{};

        class ThreadActiveHookReentryGuard {
          public:
            explicit ThreadActiveHookReentryGuard(void *instance) noexcept
                : Instance(instance), AlreadyActive(ActiveHooks.contains(instance)) {
                if (!AlreadyActive) {
                    ActiveHooks.push(instance);
                }
            }

            ~ThreadActiveHookReentryGuard() {
                if (!AlreadyActive) {
                    ActiveHooks.pop(Instance);
                }
            }

            ThreadActiveHookReentryGuard(const ThreadActiveHookReentryGuard &) = delete;
            ThreadActiveHookReentryGuard &operator=(const ThreadActiveHookReentryGuard &) = delete;

            // true = first time this thread enters; run the hook logic normally
            // false = re-entry in the same thread; the caller should go straight to the trampoline
            explicit operator bool() const noexcept { return !AlreadyActive; }

          private:
            void *Instance;
            bool AlreadyActive;
        };

        // ============================================================
        // 在飞调用计数
        //
        // 入口 stub 最先执行 `lock inc [counter]`（见 Thunk::build），
        // dispatch 在每一条出口上减回去（下面这个 RAII）。uninstall() 摘掉补丁后
        // 用"计数归零"来判断还有没有调用在飞。
        //
        // 为什么两边要这样配对：
        //   - 自增放在 stub 最前面，是为了让"已经执行了补丁那条 jmp、但还没计数"的
        //     窗口尽量短（这个窗口无法减到 0，详见 InlineHook::uninstall()）；
        //   - 自减用 RAII，是为了覆盖 dispatch 的所有 return（含重入的提前返回）；
        //     而且因为析构有副作用，编译器不会把 dispatch 优化成尾调用。
        // ============================================================
        class ActiveCallGuard {
          public:
            explicit ActiveCallGuard(std::atomic<std::uint32_t> &activeCalls) noexcept : Counter(&activeCalls) {}

            ~ActiveCallGuard() { Counter->fetch_sub(1, std::memory_order_acq_rel); }

            ActiveCallGuard(const ActiveCallGuard &) = delete;
            ActiveCallGuard &operator=(const ActiveCallGuard &) = delete;

          private:
            std::atomic<std::uint32_t> *Counter;
        };

        // uninstall() 等待在飞调用归零的上限（毫秒）。
        // 超时只表示"没能确认排空"，不代表卸载失败 —— 语义见 InlineHook::uninstall()。
        inline constexpr std::uint64_t kUninstallDrainTimeoutMs = 2000;
    } // namespace detail

    enum class HookMode : uint8_t {
        Before,
        After,
        Replace,
    };

    template <typename F, typename Self, typename R, typename... Args>
    concept HookCallable =
        std::invocable<F &, Self &, Args...> && std::same_as<std::invoke_result_t<F &, Self &, Args...>, R>;

    // 实例与入口的对应关系：
    //
    // 每个 InlineHook 实例在自己的可执行页里生成一份入口 thunk，thunk 内嵌了
    // 实例指针，把它写进本线程的 TLS 交接槽，然后跳到共享的 dispatch。
    // 所以实例的传递完全发生在运行期，不需要编译期站点身份（Tag），
    // 同一个 callable 类型可以有任意多个实例同时存在。
    template <typename Signature, typename F> class InlineHook;

    template <typename R, typename... Args, typename F>

    class InlineHook<R(Args...), F> {
        template <typename T> using Result = Result<T, ErrorCode>;
        using signature = R(Args...);
        using return_type = R;
        using pointer = R (*)(Args...);

      public:
        template <typename T>
            requires HookCallable<T, InlineHook, R, Args...>
        InlineHook(pointer src, T &&f) : Src(src), Callable(std::forward<T>(f)) {}

        // 卸载，并尽量保证"返回后不再有 detour 在跑"。
        //
        // 流程：
        //   1) 持锁摘掉补丁（原始 5 字节写回去）—— 之后不会再有新线程从 Src 进来；
        //   2) 放锁，等在飞调用计数归零；
        //   3) 重新持锁收尾。
        //
        // 为什么第 2 步必须在锁外：detour 里可能回过来调 mode() / install() 抢同一把锁，
        // 持锁等待就会死锁。
        //
        // 为什么能收敛：补丁已经摘掉，在飞数量只会单调下降，不会再产生新的自增。
        //
        // 为什么不是绝对保证：线程从"执行补丁里那条 jmp"到"stub 里把计数加上去"之间
        // 有几条指令的窗口。这期间它既不在计数里、也还没开始跑 detour；若恰好在这个窗口
        // 里被我们观察到"计数为 0"，就会误判成已排空。这个窗口用计数关不掉（计数器本身
        // 也在被卸载的代码路径上），要关掉只能挂起其它线程再改字节（Detours 的做法）。
        // 这里做的是把风险从"整个 detour 执行期（微秒~毫秒级）"压缩到"几条指令（纳秒级）"。
        //
        // 返回值语义：
        //   - 成功              => 计数已归零，可以不再担心 detour 在跑（上述窗口除外），
        //                          此时可以安全销毁 detour 捕获的状态；
        //   - UninstallTimedOut => 卸载本身已经完成（补丁已摘、Installed 已复位），
        //                          但超过 detail::kUninstallDrainTimeoutMs 仍有调用没跑完。
        //                          这时**先不要销毁 detour 捕获的状态**，可以自己轮询
        //                          activeCallCount() 再等，或者换一个更静止的时机卸载。
        auto uninstall() -> Result<void> {
            if (!Installed)
                return {};
            //  先卸载再等 ⇒ 不会再有新自增 ⇒ 计数单调下降 ⇒ 必然收敛 ✓

            // 1. 先卸载补丁：不再有新线程进来
            {
                detail::LockGuard<detail::CriticalSection> guard(Lock);
                REZERO_CHECK(Patch.uninstall());
            }

            const bool drained = waitForActiveCallsToDrain();

            {
                detail::LockGuard<detail::CriticalSection> guard(Lock);
                Installed = false;
                releaseExecutionMemory();
                FlushInstructionCache(GetCurrentProcess(), NULL, 0);
            }

            if (!drained) {
                return Error(ErrorCode::UninstallTimedOut);
            }
            return {};
        }

        auto install() -> Result<void> {
            detail::LockGuard<detail::CriticalSection> guard(Lock);
            if (Installed) {
                return {};
            }
            auto *ptr = detail::tryAllocateNear(reinterpret_cast<std::uint8_t *>(Src));
            if (ptr == nullptr) {
                return Error(ErrorCode::AllocationFailed);
            }
            ExecutionMem = ptr;

            auto trampoline = ptr;
            auto thunk = ptr + detail::kStubOffsetInPage;

            auto trampolineBuilt = Trampoline.at(trampoline).build(Src);
            if (!trampolineBuilt) {
                releaseExecutionMemory();
                return trampolineBuilt;
            }

            // 入口 stub：计数 +1 -> 保存实参寄存器 -> 调 bindCurrentHook(this) -> 恢复 -> 跳 dispatch。
            Thunk.at(thunk).build(this, reinterpret_cast<void *>(&dispatch), &ActiveCalls);

            auto patched = Patch.install(Src, Thunk.pointer());
            if (!patched) {
                releaseExecutionMemory();
                return patched;
            }

            Installed = true;
            FlushInstructionCache(GetCurrentProcess(), NULL, 0);
            return {};
        }

        // 入口 thunk 的唯一跳转目标：thunk 先把实例写进 TLS 交接槽再跳过来。
        static auto dispatch(Args... args) -> return_type {
            auto *instance = detail::takeCurrentHook();
            // 入口 thunk 一定在跳过来之前写过交接槽
            REZERO_ASSERT(instance != nullptr);

            auto &self = *static_cast<InlineHook *>(instance);

            // 与入口 stub 的 `lock inc` 配对。构造在最前面，保证后面任何一条 return
            // （包括重入时直接走 trampoline 的那条）都会把计数减回去。
            detail::ActiveCallGuard activeCalls(self.ActiveCalls);
            /*
             * Without re-entry protection:
             *
             *   Hook()
             *     │
             *     ▼
             *   Execute Hook
             *     │
             *     ├── Hook() is called again
             *     │
             *     ▼
             *   Execute Hook
             *     │
             *     ├── Hook() is called again
             *     │
             *     ▼
             *   ...
             *
             * This causes recursive re-entry and may eventually lead to
             * infinite recursion, stack overflow, or repeated execution
             * of the hook logic.
             *
             * 重入检测是「线程内」的：活动栈每线程一份，所以线程 A 停在 hook 里
             * 不会让线程 B 被误判为重入；同一个 hook 可以同时在多个线程上执行。
             */
            detail::ThreadActiveHookReentryGuard guard(&self);
            if (!guard) {
                auto trampoline = reinterpret_cast<pointer>(self.Trampoline.pointer());
                return std::invoke(trampoline, std::forward<Args>(args)...);
            }

            if (self.Mode == HookMode::Before) {
                std::invoke(self.Callable, self, std::forward<Args>(args)...);
                auto trampoline = reinterpret_cast<pointer>(self.Trampoline.pointer());
                return std::invoke(trampoline, std::forward<Args>(args)...);
            } else if (self.Mode == HookMode::After) {
                auto trampoline = reinterpret_cast<pointer>(self.Trampoline.pointer());
                if constexpr (std::is_void_v<R>) {
                    std::invoke(trampoline, std::forward<Args>(args)...);
                    std::invoke(self.Callable, self, std::forward<Args>(args)...);
                    return;
                } else {
                    R result = std::invoke(trampoline, std::forward<Args>(args)...);
                    std::invoke(self.Callable, self, std::forward<Args>(args)...);
                    return result;
                }
            } else if (self.Mode == HookMode::Replace) {
                return std::invoke(self.Callable, self, std::forward<Args>(args)...);
            }

            auto trampoline = reinterpret_cast<pointer>(self.Trampoline.pointer());
            return std::invoke(trampoline, std::forward<Args>(args)...);
        }

        auto trampoline(Args... args) -> return_type {
            auto trampoline = reinterpret_cast<pointer>(Trampoline.pointer());
            return std::invoke(trampoline, std::forward<Args>(args)...);
        }

        auto &&mode(this auto &&self, HookMode type) {
            self.Mode = type;
            return std::forward<decltype(self)>(self);
        }

        // 当前在飞调用数：入口 stub 自增、dispatch 出口自减。
        // uninstall() 就是等它归零；这里暴露出来，是为了让调用方在
        // 拿到 UninstallTimedOut 之后能自己再轮询等待。
        [[nodiscard]] auto activeCallCount() const noexcept -> std::uint32_t {
            return ActiveCalls.load(std::memory_order_acquire);
        }

      private:
        // 等在飞调用归零。返回 false 表示超时。
        [[nodiscard]] auto waitForActiveCallsToDrain() noexcept -> bool {
            // 当前线程自己就在这个 hook 里（例如 detour 内部调 uninstall）：
            // 计数里就包含自己，等下去必然死锁，直接放弃。
            if (detail::ActiveHooks.contains(this)) {
                return false;
            }

            const std::uint64_t deadline = GetTickCount64() + detail::kUninstallDrainTimeoutMs;
            while (ActiveCalls.load(std::memory_order_acquire) != 0) {
                if (GetTickCount64() >= deadline) {
                    return false;
                }
                // 让出时间片就够了：Sleep 会把在飞调用一起拖慢，反而更晚才排空。
                SwitchToThread();
            }
            return true;
        }

        // 处置当前这份可执行页：释放它，并同时清空 Thunk / Trampoline 的指针。
        //
        // 两个指针必须一起清空：它们的析构会 memset 那块内存，否则就是写已释放内存。
        // 清空之后 `trampoline()` 也就不再可用 —— 卸载后要调原函数就直接调 `Src`
        // （补丁已经摘掉，`Src` 就是原函数本身）。
        //
        // 这里有一个消不掉的窗口：uninstall() 会先摘补丁、等计数归零，再走到这里；
        // 但"线程已经执行了补丁那条 jmp、还没把计数加上去"的那几条指令落在窗口里，
        // 这种调用会在本页释放之后执行已释放的代码。要彻底关掉它只能挂起其它线程再改字节。
        //
        // 安装失败的清理路径也走这里：即使 Patch.install 报错，补丁字节也可能已经写进去了
        // （只是保护还原失败）。
        void releaseExecutionMemory() noexcept {
            if (ExecutionMem == nullptr) {
                return;
            }

            VirtualFree(ExecutionMem, 0, MEM_RELEASE);
            Trampoline.at(nullptr);
            Thunk.at(nullptr);

            ExecutionMem = nullptr;
        }

        // 在飞调用计数：入口 stub 自增（见 Thunk::build），dispatch 每条出口自减
        // （见 detail::ActiveCallGuard）。它必须活在对象里、而不是那份可执行页里。
        std::atomic<std::uint32_t> ActiveCalls{0};

        F Callable;
        HookMode Mode = HookMode::Replace;
        pointer Src = nullptr;
        Trampoline Trampoline;
        InlinePatch Patch;
        Thunk Thunk;
        bool Installed = false;
        detail::CriticalSection Lock;
        void *ExecutionMem = nullptr;
    };

    template <typename R, typename... Args, typename F> inline auto *makeInlineHook(R (*src)(Args...), F &&f) {
        return smp::create<InlineHook<R(Args...), std::decay_t<F>>>(src, std::forward<F>(f));
    }

} // namespace ReZeroDetour
