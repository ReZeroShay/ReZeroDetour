#pragma once
#include <ReZeroDetour/ReZeroLib.h>

namespace ReZeroDetour {
    // ============================================================
    // TLS 上下文：实例指针怎么从入口 stub 交到 dispatch
    //
    //   进入 Stub
    //     ↓
    //   保存原始参数寄存器 (RCX/RDX/R8/R9/RAX) 与浮点参数寄存器 (XMM0-3)
    //     ↓
    //   调用 setPendingInstance(instance)
    //     ↓
    //   恢复原始参数寄存器
    //     ↓
    //   跳到该签名专属的 C++ entry (dispatch)
    //
    // 这里刻意不自己推导 TLS 数据块的索引与偏移：那是编译器的知识（索引、块偏移、
    // 访问模型都可能变），手写等于把编译器 ABI 抄一遍。所以让 stub 回调一个普通
    // C++ 函数，由编译器生成那句 `tlsPendingInstance = instance;`。
    // stub 只承担"保存/恢复易失实参寄存器"这件 ABI 上非常稳定的事。
    // ============================================================
    namespace detail {

        // 交接槽：由入口 stub 通过 bindCurrentHook() 设置，
        // 由 dispatch 一进门读走并清空 —— 所以不会留下悬垂的实例指针。
        inline thread_local void *tlsCurrentHookInstance = nullptr;

        // 入口 stub 调用的 setter。TLS 访问由编译器生成，这里不做任何指针运算。
        REZERO_NOINLINE inline void bindCurrentHook(void *instance) noexcept { tlsCurrentHookInstance = instance; }

        // 读走并清空。写入与读出之间不存在任何用户代码，所以单槽就够，不需要栈；
        // 嵌套与重入的恢复由 ReZeroDetour.h 里的每线程活动栈负责。
        [[nodiscard]] inline auto takeCurrentHook() noexcept -> void * {
            void *cur = tlsCurrentHookInstance;
            tlsCurrentHookInstance = nullptr;
            return cur;
        }
    } // namespace detail
} // namespace ReZeroDetour
