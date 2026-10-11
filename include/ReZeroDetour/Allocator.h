#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <memory>
#include <ReZeroDetour/ReZeroLib.h>

namespace ReZeroDetour {
    class LinearAllocator {
      public:
        constexpr LinearAllocator(std::byte *buffer, std::size_t size) noexcept
            : Begin(buffer), Current(buffer), End(buffer + size) {}

        LinearAllocator(const LinearAllocator &) = delete;
        LinearAllocator &operator=(const LinearAllocator &) = delete;

        constexpr LinearAllocator(LinearAllocator &&other) noexcept
            : Begin(std::exchange(other.Begin, nullptr)), Current(std::exchange(other.Current, nullptr)),
              End(std::exchange(other.End, nullptr)) {}

        constexpr LinearAllocator &operator=(LinearAllocator &&other) noexcept {
            if (this != &other) {
                Begin = std::exchange(other.Begin, nullptr);
                Current = std::exchange(other.Current, nullptr);
                End = std::exchange(other.End, nullptr);
            }
            return *this;
        }

        [[nodiscard]] void *allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
            auto current = reinterpret_cast<std::uintptr_t>(Current);
            auto aligned = (current + alignment - 1) & ~(alignment - 1);
            auto next = aligned + size;

            if (next > reinterpret_cast<std::uintptr_t>(End)) {
                return nullptr;
            }

            Current = reinterpret_cast<std::byte *>(next);
            return reinterpret_cast<void *>(aligned);
        }

        template <typename T, typename... Args>
        // requires std::is_trivially_destructible_v<T>
        [[nodiscard]] T *create(Args &&...args) {
            void *mem = allocate(sizeof(T), alignof(T));

            REZERO_ASSERT(mem != nullptr);
            if (mem == nullptr) {
                return nullptr;
            }

            return std::construct_at(static_cast<T *>(mem), std::forward<Args>(args)...);
        }

        constexpr void reset() noexcept { Current = Begin; }

        [[nodiscard]] constexpr std::size_t remaining() const noexcept {
            return static_cast<std::size_t>(End - Current);
        }

        [[nodiscard]] constexpr std::size_t used() const noexcept { return static_cast<std::size_t>(Current - Begin); }

        [[nodiscard]] constexpr std::size_t capacity() const noexcept { return static_cast<std::size_t>(End - Begin); }

      private:
        std::byte *Begin = nullptr;
        std::byte *Current = nullptr;
        std::byte *End = nullptr;
    };
} // namespace ReZeroDetour
