#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <cstddef>
#include <cstdint>
#include <new>
#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/Sync.h>
#include <ReZeroDetour/IntrusiveList.h>
#include <ReZeroDetour/CodeGen.h>
#include <SmpAllocator/SmpAllocator.h>

// 补丁表：字节补丁的统一管理。
//
// 它只认「目标地址 + 原始字节 + 补丁跳去哪」，与任何 hook 的签名无关，
// 所以是非模板的。模板层（InlineHook）只持有表里的 PatchId，
// 不再自己背一份 InlinePatch。
//
// 两条约束：
//   - 同一个目标地址只允许一条补丁（重复安装返回 AlreadyHooked），
//     否则后装的那条会把前一条的 jmp 当成"原始字节"备份，卸载顺序一反就还原成野跳转；
//   - restoreAll() 遇错继续，跑完再报告计数与首个错误码。

namespace ReZeroDetour {

    struct PatchRestoreReport {
        std::size_t Restored = 0;
        std::size_t Failed = 0;
        bool HasError = false;
        ErrorCode FirstError = ErrorCode::PatchNotFound;
    };

    class PatchTable {
      public:
        using PatchId = std::uint32_t;

        static constexpr PatchId InvalidPatchId = 0;
        // 单条补丁需要备份的最大字节数：目前最长的编码是 14 字节绝对跳转。
        static constexpr std::size_t kMaxBackupSize = kAbsoluteJumpSize;

        PatchTable() = default;

        ~PatchTable() {
            // 先摘节点再释放：释放之后再读 node.Next 就是 use-after-free。
            for (auto it = Records.begin(); it != Records.end();) {
                Record &record = *it;
                it = Records.erase(it);
                smp::destroy(&record);
            }
        }

        PatchTable(const PatchTable &) = delete;
        PatchTable &operator=(const PatchTable &) = delete;

        // 登记一个目标并备份原始字节。此时还不写补丁。
        [[nodiscard]] auto add(void *target, std::size_t length, void *destination) -> Result<PatchId, ErrorCode> {
            REZERO_ASSERT(target != nullptr);
            REZERO_ASSERT(destination != nullptr);

            if (length != kRel32JumpSize && length != kAbsoluteJumpSize) {
                return Error{ErrorCode::UnsupportedPatchSize};
            }

            detail::LockGuard<detail::CriticalSection> guard(Lock);

            if (findLocked(target) != nullptr) {
                return Error{ErrorCode::AlreadyHooked};
            }

            auto *record = smp::create<Record>();
            if (record == nullptr) {
                return Error{ErrorCode::AllocationFailed};
            }

            // smp::create 是值初始化，这里显式清一遍，免得将来加了字段忘了填。
            memset(record, 0, sizeof(Record));
            record->Id = NextId++;
            record->Target = target;
            record->Destination = destination;
            record->Length = length;
            record->State = Status::BackedUp;
            memcpy(record->Original, target, length);
            Records.pushFront(*record);

            return record->Id;
        }

        // 把已登记的补丁写进目标。重复调用是幂等的。
        [[nodiscard]] auto install(PatchId id) -> Result<void, ErrorCode> {
            detail::LockGuard<detail::CriticalSection> guard(Lock);

            Record *record = recordForWrite(id);
            if (record == nullptr) {
                return Error{ErrorCode::PatchNotFound};
            }            if (record->State == Status::Installed) {
                return {};
            }

            std::uint8_t encoded[kMaxBackupSize] = {};
            REZERO_CHECK(buildJump(*record, encoded));

            auto written = writeProtectedMemory(record->Target, encoded, record->Length);
            if (!written) {
                // 保护切换失败时字节可能已经写进去了，状态不确定，留下记录等 restore 收拾。
                record->State = Status::Dirty;
                return written;
            }

            record->State = Status::Installed;
            return {};
        }

        // 还原一个补丁并释放记录。记录已经不在表里时视为已还原（重复卸载不报错）。
        [[nodiscard]] auto restore(PatchId id) -> Result<void, ErrorCode> {
            detail::LockGuard<detail::CriticalSection> guard(Lock);

            for (auto it = Records.begin(); it != Records.end(); ++it) {
                if (it->Id != id) {
                    continue;
                }

                Record &record = *it;
                auto result = restoreRecord(record);
                // 保护切换失败的那条留着，下次 restoreAll 还能再试一次；其余情况记录都作废。
                const bool keep = result.is_err() && result.error() == ErrorCode::VirtualProtectFailed;
                if (!keep) {
                    Records.erase(it);
                    smp::destroy(&record);
                }
                return result;
            }

            return {};
        }

        // 全部还原：遇错继续，跑完报告计数与首个错误码。
        auto restoreAll() -> PatchRestoreReport {
            PatchRestoreReport report{};

            detail::LockGuard<detail::CriticalSection> guard(Lock);

            auto it = Records.begin();
            while (it != Records.end()) {
                Record &record = *it;
                auto result = restoreRecord(record);

                if (result) {
                    ++report.Restored;
                } else {
                    ++report.Failed;
                    if (!report.HasError) {
                        report.HasError = true;
                        report.FirstError = result.error();
                    }
                }

                const bool keep = result.is_err() && result.error() == ErrorCode::VirtualProtectFailed;
                if (keep) {
                    ++it;
                } else {
                    it = Records.erase(it);
                    smp::destroy(&record);
                }
            }

            return report;
        }

        // 目标地址是否已经被登记过。
        [[nodiscard]] auto find(const void *target) const -> PatchId {
            detail::LockGuard<detail::CriticalSection> guard(Lock);
            const Record *record = findLocked(target);
            return record != nullptr ? record->Id : InvalidPatchId;
        }

        [[nodiscard]] auto contains(PatchId id) const -> bool {
            detail::LockGuard<detail::CriticalSection> guard(Lock);
            return findByIdLocked(id) != nullptr;
        }

        [[nodiscard]] auto size() const -> std::size_t {
            detail::LockGuard<detail::CriticalSection> guard(Lock);
            return Records.size();
        }

      private:
        enum class Status : std::uint8_t {
            BackedUp,  // 原始字节已备份，还没写补丁
            Installed, // 补丁已写入
            Dirty,     // 写入或还原时保护切换失败，字节状态不确定
        };

        struct Record {
            IntrusiveListNode Link;
            PatchId Id;
            void *Target;
            void *Destination;
            std::size_t Length;
            Status State;
            std::uint8_t Original[kMaxBackupSize];
        };

        [[nodiscard]] auto findLocked(const void *target) const -> const Record * {
            for (const Record &record : Records) {
                if (record.Target == target) {
                    return &record;
                }
            }
            return nullptr;
        }

        [[nodiscard]] auto recordForWrite(PatchId id) -> Record * {
            for (Record &record : Records) {
                if (record.Id == id) {
                    return &record;
                }
            }
            return nullptr;
        }

        [[nodiscard]] auto findByIdLocked(PatchId id) const -> const Record * {
            for (const Record &record : Records) {
                if (record.Id == id) {
                    return &record;
                }
            }
            return nullptr;
        }

        // 按记录重新生成当初写进去的补丁字节。补丁只由地址决定，所以可以确定性重建，
        // 不必额外存一份"我们写的是什么"，还原前也就不必再算一次。
        [[nodiscard]] auto buildJump(const Record &record, std::uint8_t *out) const -> Result<void, ErrorCode> {
            const Span<std::uint8_t> payload{out, record.Length};

            if (record.Length == kRel32JumpSize) {
                return makeRel32JumpAt(payload, reinterpret_cast<intptr_t>(record.Target),
                                       reinterpret_cast<intptr_t>(record.Destination));
            }

            return makeAbsoluteJumpAt(payload, reinterpret_cast<uintptr_t>(record.Destination));
        }

        [[nodiscard]] auto restoreRecord(Record &record) -> Result<void, ErrorCode> {
            if (record.State == Status::BackedUp) {
                // 从没写过字节，没什么可还原的。
                return {};
            }

            if (record.State == Status::Installed) {
                std::uint8_t encoded[kMaxBackupSize] = {};
                REZERO_CHECK(buildJump(record, encoded));

                // 目标处的字节已经被别人改过：这时候写回原始字节等于破坏别人的补丁。
                if (memcmp(record.Target, encoded, record.Length) != 0) {
                    return Error{ErrorCode::PatchOverwritten};
                }
            }

            return writeProtectedMemory(record.Target, record.Original, record.Length);
        }

        // 改代码页的字节：临时放开写权限，写完还原原来的保护。
        static auto writeProtectedMemory(void *address, const void *data, std::size_t size)
            -> Result<void, ErrorCode> {
            if (size == 0) {
                return {};
            }

            DWORD previous = 0;
            if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous)) {
                return Error{ErrorCode::VirtualProtectFailed};
            }

            memcpy(address, data, size);

            DWORD ignored = 0;
            if (!VirtualProtect(address, size, previous, &ignored)) {
                return Error{ErrorCode::VirtualProtectFailed};
            }
            return {};
        }

        mutable detail::CriticalSection Lock;
        IntrusiveList<Record, &Record::Link> Records;
        PatchId NextId = 1;
    };

    // 进程级默认补丁表。
    //
    // 永不析构：它的析构会去碰 SmpAllocator 的全局状态，而静态析构顺序跨 TU 无法保证。
    // 表在进程退出时也没有需要归还的东西，所以让它活到最后最省事。
    inline auto defaultPatchTable() noexcept -> PatchTable & {
        alignas(PatchTable) static std::byte storage[sizeof(PatchTable)]{};
        static PatchTable *table = ::new (static_cast<void *>(storage)) PatchTable();
        return *table;
    }

    // 一行全部卸载：还原默认表里所有补丁。
    inline auto uninstallAllPatches() -> PatchRestoreReport { return defaultPatchTable().restoreAll(); }

} // namespace ReZeroDetour
