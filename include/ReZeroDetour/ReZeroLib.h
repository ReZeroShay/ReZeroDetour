#pragma once
#include <cstdint>
#include <type_traits>
#include <cassert>
#include <memory>

#if defined(_MSC_VER)
#    pragma warning(push)
#    pragma warning(disable : 4141)
#endif

#if defined(_MSC_VER)
#    define REZERO_COMPILER_MSVC 1
#    define REZERO_BREAK() __debugbreak()
#    define REZERO_INLINE inline __forceinline
#    define REZERO_NOINLINE __declspec(noinline)
#elif defined(__clang__)
#    define REZERO_COMPILER_CLANG 1
#    define REZERO_BREAK() __builtin_debugtrap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#elif defined(__GNUC__)
#    define REZERO_COMPILER_GCC 1
#    define REZERO_BREAK() __builtin_trap()
#    define REZERO_INLINE inline __attribute__((always_inline))
#    define REZERO_NOINLINE __attribute__((noinline))
#else
#    error "ReZeroDetour: unsupported compiler"
#endif

#if !defined(REZERO_INLINE)
#    define REZERO_INLINE inline
#endif
#if !defined(REZERO_NOINLINE)
#    define REZERO_NOINLINE
#endif
#if !defined(REZERO_BREAK)
#    define REZERO_BREAK() ((void)0)
#endif

#define REZERO_NODISCARD [[nodiscard]]
#define REZERO_LIKELY(x) (!!(x))
#define REZERO_UNLIKELY(x) (!!(x))

#if !defined(REZERO_ENABLE_ASSERT)
#    if defined(NDEBUG)
#        define REZERO_ENABLE_ASSERT 0
#    else
#        define REZERO_ENABLE_ASSERT 1
#    endif
#endif

#if REZERO_ENABLE_ASSERT
#    define REZERO_ASSERT(expr) assert(expr)
#    define REZERO_ASSERT_MSG(expr, msg) assert((expr) && (msg))
#else
#    define REZERO_ASSERT(expr) ((void)0)
#    define REZERO_ASSERT_MSG(expr, msg) ((void)0)
#endif

#define REZERO_CHECK(expr)                                                                                             \
    do {                                                                                                               \
        auto &&checked_ = (expr);                                                                                      \
        if (!checked_)                                                                                                 \
            return Error{checked_.error()};                                                                            \
    } while (false)

#define REZERO_TRY(name, expr)                                                                                         \
    auto name##checked_ = (expr);                                                                                      \
    if (!name##checked_)                                                                                               \
        return Error{name##checked_.error()};                                                                          \
    auto name = *name##checked_;                                                                                       \
    (void)0

namespace ReZeroDetour {

    constexpr auto memset(void *destination, std::uint8_t value, std::size_t count) noexcept -> void * {
        auto *bytes = static_cast<std::uint8_t *>(destination);

        for (std::size_t i = 0; i < count; ++i)
            bytes[i] = value;

        return destination;
    }

    constexpr auto memcpy(void *destination, const void *source, std::size_t count) noexcept -> void * {
        auto *dst = static_cast<std::uint8_t *>(destination);
        const auto *src = static_cast<const std::uint8_t *>(source);

        for (std::size_t i = 0; i < count; ++i)
            dst[i] = src[i];

        return destination;
    }

    [[nodiscard]] constexpr auto memcmp(const void *lhs, const void *rhs, std::size_t count) noexcept -> int {
        const auto *a = static_cast<const std::uint8_t *>(lhs);
        const auto *b = static_cast<const std::uint8_t *>(rhs);

        for (std::size_t i = 0; i < count; ++i) {
            if (a[i] < b[i])
                return -1;

            if (a[i] > b[i])
                return 1;
        }

        return 0;
    }
    template <typename E> class Error {
      public:
        E error;

        constexpr explicit Error(const E &e) : error(e) {}
        constexpr explicit Error(E &&e) : error(std::move(e)) {}
    };

    template <typename E> Error(E) -> Error<E>;

    template <typename T, typename E> struct Result {
        using ValueType = T;
        using ErrorType = E;

        union Payload {
            T value;
            E error;

            constexpr Payload() noexcept {}
            constexpr ~Payload() noexcept {};
        };

      public:
        constexpr Result(const T &val) : Payload{}, IsOk(true) {
            std::construct_at(std::addressof(Payload.value), val);
        }
        constexpr Result(T &&val) noexcept : Payload{}, IsOk(true) {
            std::construct_at(std::addressof(Payload.value), std::move(val));
        }

        constexpr Result(const Error<E> &err) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), err.error);
        }

        constexpr Result(Error<E> &&err) noexcept(std::is_nothrow_move_constructible_v<E>) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), std::move(err.error));
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<T>, Args &&...args) : Payload{}, IsOk(true) {
            std::construct_at(std::addressof(Payload.value), std::forward<Args>(args)...);
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<E>, Args &&...args) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), std::forward<Args>(args)...);
        }

        template <typename U>
            requires std::is_convertible_v<U, T> && (!std::same_as<U, T>)
        constexpr Result(const Result<U, E> &other) : Payload{}, IsOk(other.is_ok()) {
            if (IsOk) {
                std::construct_at(std::addressof(Payload.value), other.value());
            } else {
                std::construct_at(std::addressof(Payload.error), other.error());
            }
        }

        template <typename U>
            requires std::is_convertible_v<U, T> && (!std::same_as<U, T>)
        constexpr Result(Result<U, E> &&other) : Payload{}, IsOk(other.is_ok()) {
            if (IsOk) {
                std::construct_at(std::addressof(Payload.value), std::move(other.value()));
            } else {
                std::construct_at(std::addressof(Payload.error), std::move(other.error()));
            }
        }

        constexpr Result(const Result &other) : Payload{}, IsOk(other.IsOk) {
            if (IsOk) {
                std::construct_at(std::addressof(Payload.value), other.Payload.value);
            } else {
                std::construct_at(std::addressof(Payload.error), other.Payload.error);
            }
        }

        constexpr Result(Result &&other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                  std::is_nothrow_move_constructible_v<E>)
            : Payload{}, IsOk(other.IsOk) {
            if (IsOk) {
                std::construct_at(std::addressof(Payload.value), std::move(other.Payload.value));
            } else {
                std::construct_at(std::addressof(Payload.error), std::move(other.Payload.error));
            }
        }

        constexpr ~Result() { destroy(); }

        constexpr void destroy() noexcept {
            if (IsOk) {
                std::destroy_at(std::addressof(Payload.value));
            } else {
                std::destroy_at(std::addressof(Payload.error));
            }
        }

        constexpr Result &operator=(const Result &other) {
            if (this != std::addressof(other)) {
                destroy();
                IsOk = other.IsOk;
                if (IsOk) {
                    std::construct_at(std::addressof(Payload.value), other.Payload.value);
                } else {
                    std::construct_at(std::addressof(Payload.error), other.Payload.error);
                }
            }
            return *this;
        }

        constexpr Result &operator=(Result &&other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                             std::is_nothrow_move_constructible_v<E>) {
            if (this != std::addressof(other)) {
                destroy();
                IsOk = other.IsOk;
                if (IsOk) {
                    std::construct_at(std::addressof(Payload.value), std::move(other.Payload.value));
                } else {
                    std::construct_at(std::addressof(Payload.error), std::move(other.Payload.error));
                }
            }
            return *this;
        }

        [[nodiscard]] constexpr bool is_ok() const noexcept { return IsOk; }
        [[nodiscard]] constexpr bool is_err() const noexcept { return !IsOk; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsOk; }

        [[nodiscard]] constexpr auto &&value(this auto &&self) noexcept {
            REZERO_ASSERT(self.IsOk);
            return std::forward<decltype(self)>(self).Payload.value;
        }

        [[nodiscard]] constexpr auto &&operator*(this auto &&self) noexcept {
            REZERO_ASSERT(self.IsOk);
            return std::forward<decltype(self)>(self).Payload.value;
        }
        [[nodiscard]] constexpr auto *operator->(this auto &&self) noexcept {
            REZERO_ASSERT(self.IsOk);
            return std::addressof(std::forward<decltype(self)>(self).Payload.value);
        }

        [[nodiscard]] constexpr auto &&error(this auto &&self) noexcept {
            REZERO_ASSERT(!self.IsOk);
            return std::forward<decltype(self)>(self).Payload.error;
        }

        template <typename U> constexpr T value_or(U &&default_value) const & {
            return IsOk ? Payload.value : static_cast<T>(std::forward<U>(default_value));
        }

        template <typename U> constexpr T value_or(U &&default_value) && {
            return IsOk ? std::move(Payload.value) : static_cast<T>(std::forward<U>(default_value));
        }

        template <typename F>
        [[nodiscard]] auto map(this auto &&self, F &&f) noexcept
            -> Result<std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).value())>, E> {
            using U = std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).value())>;
            if (self.is_ok()) {
                return Result<U, E>(std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).value()));
            }
            return Result<U, E>(Error(std::forward<decltype(self)>(self).error()));
        }

        template <typename F>
        [[nodiscard]] auto map_err(this auto &&self, F &&f) noexcept
            -> Result<T, std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())>> {
            using Err = std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())>;
            if (self.is_ok()) {
                return Result<T, Err>(std::forward<decltype(self)>(self).value());
            }
            return Result<T, Err>(Error(std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).error())));
        }

        template <typename F>
        [[nodiscard]] auto and_then(this auto &&self, F &&f) noexcept
            -> std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).value())> {
            using R = std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).value())>;
            if (self.is_ok()) {
                return std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).value());
            }
            return R(Error(std::forward<decltype(self)>(self).error()));
        }

        template <typename F>
        [[nodiscard]] constexpr auto or_else(this auto &&self, F &&f)
            -> std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())> {
            using R = std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())>;
            static_assert(std::same_as<typename R::ValueType, T>);

            if (self.is_ok()) {
                return R{std::forward<decltype(self)>(self).value()};
            }
            return std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).error());
        }

      private:
        Payload Payload;
        bool IsOk = true;
    };

    template <typename E> struct Result<void, E> {
        using ValueType = void;
        using ErrorType = E;

        union Payload {
            unsigned char dummy;
            E error;

            constexpr Payload() noexcept : dummy(0) {}
            constexpr ~Payload() noexcept {}
        };

      public:
        constexpr Result() noexcept : Payload{}, IsOk(true) {}

        constexpr Result(const Error<E> &err) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), err.error);
        }

        constexpr Result(Error<E> &&err) noexcept(std::is_nothrow_move_constructible_v<E>) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), std::move(err.error));
        }

        template <typename... Args>
        constexpr explicit Result(std::in_place_type_t<E>, Args &&...args) : Payload{}, IsOk(false) {
            std::construct_at(std::addressof(Payload.error), std::forward<Args>(args)...);
        }

        constexpr Result(const Result &other) : Payload{}, IsOk(other.IsOk) {
            if (!IsOk) {
                std::construct_at(std::addressof(Payload.error), other.Payload.error);
            }
        }

        constexpr Result(Result &&other) : Payload{}, IsOk(other.IsOk) {
            if (!IsOk) {
                std::construct_at(std::addressof(Payload.error), std::move(other.Payload.error));
            }
        }

        constexpr ~Result() { destroy(); }

        constexpr void destroy() noexcept {
            if (!IsOk) {
                std::destroy_at(std::addressof(Payload.error));
            }
        }

        constexpr Result &operator=(const Result &other) {
            if (this != std::addressof(other)) {
                destroy();
                IsOk = other.IsOk;
                if (!IsOk) {
                    std::construct_at(std::addressof(Payload.error), other.Payload.error);
                }
            }
            return *this;
        }

        constexpr Result &operator=(Result &&other) {
            if (this != std::addressof(other)) {
                destroy();
                IsOk = other.IsOk;
                if (!IsOk) {
                    std::construct_at(std::addressof(Payload.error), std::move(other.Payload.error));
                }
            }
            return *this;
        }

        [[nodiscard]] constexpr bool is_ok() const noexcept { return IsOk; }
        [[nodiscard]] constexpr bool is_err() const noexcept { return !IsOk; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return IsOk; }

        constexpr void value() const noexcept { REZERO_ASSERT(IsOk); }
        [[nodiscard]] constexpr auto &&error(this auto &&self) noexcept {
            REZERO_ASSERT(!self.IsOk);
            return std::forward<decltype(self)>(self).Payload.error;
        }
        template <typename F>
        [[nodiscard]] auto map(this auto &&self, F &&f) noexcept -> Result<std::invoke_result_t<F>, E> {
            using U = std::invoke_result_t<F>;
            if (self.is_ok()) {
                if constexpr (std::is_void_v<U>) {
                    std::invoke(std::forward<F>(f));
                    return Result<void, E>{};
                }
                return Result<U, E>(std::invoke(std::forward<F>(f)));
            }
            return Result<U, E>(Error(std::forward<decltype(self)>(self).error()));
        }

        template <typename F>
        [[nodiscard]] auto map_err(this auto &&self, F &&f) noexcept
            -> Result<void, std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())>> {
            using Err = std::invoke_result_t<F, decltype(std::forward<decltype(self)>(self).error())>;
            if (self.is_ok()) {
                return Result<void, Err>{};
            }
            return Result<void, Err>(
                Error(std::invoke(std::forward<F>(f), std::forward<decltype(self)>(self).error())));
        }

        template <typename F> [[nodiscard]] auto and_then(this auto &&self, F &&f) noexcept -> std::invoke_result_t<F> {
            using R = std::invoke_result_t<F>;
            static_assert(std::is_same_v<typename R::ErrorType, E>,
                          "and_then:callable must return a Result with the same error type");
            if (self.is_ok()) {
                return std::invoke(std::forward<F>(f));
            }
            return R(Error(std::forward<decltype(self)>(self).error()));
        }

      private:
        Payload Payload;
        bool IsOk = true;
    };

    template <typename T> struct Span {
        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using const_pointer = const T *;
        using reference = T &;
        using const_reference = const T &;
        using iterator = pointer;
        using const_iterator = const_pointer;

        constexpr Span() noexcept = default;
        constexpr Span(pointer ptr, size_type length) : Ptr(ptr), Length(length) {}

        template <size_t N> constexpr Span(T (&array)[N]) : Ptr(array), Length(N) {}

        template <typename U>
            requires std::convertible_to<U (*)[], element_type (*)[]>
        constexpr Span(const Span<U> &other) noexcept : Ptr(other.data()), Length(other.size()) {}

        constexpr Span(const Span &) noexcept = default;
        constexpr Span &operator=(const Span &) noexcept = default;

        constexpr iterator begin() const noexcept { return Ptr; }
        constexpr const_iterator cbegin() const noexcept { return Ptr; }
        constexpr iterator end() const noexcept { return Ptr + Length; }
        constexpr const_iterator cend() const noexcept { return Ptr + Length; }

        constexpr reference operator[](size_type idx) const noexcept {
            REZERO_ASSERT(idx < Length);
            return Ptr[idx];
        }

        constexpr reference front() const noexcept {
            REZERO_ASSERT(!empty());
            return Ptr[0];
        }

        constexpr reference back() const noexcept {
            REZERO_ASSERT(!empty());
            return Ptr[Length - 1];
        }

        constexpr pointer data() const noexcept { return Ptr; }

        constexpr size_type size() const noexcept { return Length; }
        constexpr size_type size_bytes() const noexcept { return Length * sizeof(element_type); }
        [[nodiscard]] constexpr bool empty() const noexcept { return Length == 0; }

        constexpr Span first(size_type size) const noexcept {
            REZERO_ASSERT(size <= Length);
            return {Ptr, size};
        }

        constexpr Span last(size_type size) const noexcept {
            REZERO_ASSERT(size <= Length);
            return {Ptr + (Length - size), size};
        }

        constexpr Span subspan(size_type offset, size_type size = size_type(-1)) const noexcept {
            REZERO_ASSERT(offset <= Length);
            if (size == size_type(-1)) {
                return {Ptr + offset, Length - offset};
            }
            REZERO_ASSERT(size <= Length - offset);
            return {Ptr + offset, size};
        }

      private:
        pointer Ptr = nullptr;
        size_type Length = 0;
    };

    template <typename T, std::size_t N> Span(T (&)[N]) -> Span<T>;

    template <typename T, std::size_t N> class Array {
        static_assert(N > 0, "Array size must be greater than zero.");

      public:
        using value_type = T;
        using size_type = std::size_t;
        using reference = T &;
        using const_reference = const T &;
        using iterator = T *;
        using const_iterator = const T *;

        constexpr Array() = default;

        template <typename... U>
            requires(sizeof...(U) == N)
        constexpr Array(U &&...args) : items{std::forward<U>(args)...} {}

        constexpr auto &&at(this auto &&self, size_type index) noexcept {
            REZERO_ASSERT(index < N);
            return std::forward<decltype(self)>(self).items[index];
        }
        constexpr auto *data(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items; }
        constexpr auto size(this auto &&) noexcept -> size_type { return N; }

        constexpr auto &&front(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items[0]; }
        constexpr auto &&back(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items[N - 1]; }

        constexpr auto *begin(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items; }
        constexpr auto *end(this auto &&self) noexcept { return std::forward<decltype(self)>(self).items + N; }

        constexpr auto &&operator[](this auto &&self, size_type index) noexcept {
            REZERO_ASSERT(index < N);
            return std::forward<decltype(self)>(self).items[index];
        }

        constexpr void fill(this auto &&self, const T &value)
            requires std::assignable_from<T &, const T &>
        {
            for (auto &item : self.items) {
                item = value;
            }
        }

      private:
        // The array member manages the object's lifetime automatically.
        T items[N];
    };

    template <typename T, typename U> struct Tuple2 {
        T first;
        U second;
    };
}; // namespace ReZeroDetour
