#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>

// 侵入式双向链表。
//
// 节点是独立类型（IntrusiveListNode），以**成员**方式嵌在元素里；链表本身
// 不分配内存、不拥有元素，只维护表头和两个指针。
//
//   struct Record {
//       IntrusiveListNode Link;   // 成员嵌入
//       int Value;
//   };
//   IntrusiveList<Record, &Record::Link> list;
//
// 双向的代价是每元素多一个 Prev，换来的是 erase 不需要前驱、也能反向遍历。
// 这里只提供正向迭代：目前没有用到反向的地方，先不加。

namespace ReZeroDetour {

    struct IntrusiveListNode {
        IntrusiveListNode *Prev = nullptr;
        IntrusiveListNode *Next = nullptr;
    };

    template <typename T, IntrusiveListNode T::*Link> class IntrusiveList {
        // 成员指针偏移依赖标准布局（和 offsetof 的前提一样）
        static_assert(std::is_standard_layout_v<T>, "T must be standard-layout");

      public:
        class ConstIterator {
          public:
            ConstIterator() = default;
            explicit ConstIterator(const IntrusiveListNode *current) noexcept : Current(current) {}

            auto operator*() const noexcept -> const T & { return valueOf(*Current); }
            auto operator->() const noexcept -> const T * { return std::addressof(valueOf(*Current)); }

            auto operator++() noexcept -> ConstIterator & {
                Current = Current->Next;
                return *this;
            }

            friend auto operator==(const ConstIterator &lhs, const ConstIterator &rhs) noexcept -> bool {
                return lhs.Current == rhs.Current;
            }

          private:
            const IntrusiveListNode *Current = nullptr;
        };

        class Iterator {
          public:
            Iterator() = default;
            explicit Iterator(IntrusiveListNode *current) noexcept : Current(current) {}

            auto operator*() const noexcept -> T & { return valueOf(*Current); }
            auto operator->() const noexcept -> T * { return std::addressof(valueOf(*Current)); }

            auto operator++() noexcept -> Iterator & {
                Current = Current->Next;
                return *this;
            }

            friend auto operator==(const Iterator &lhs, const Iterator &rhs) noexcept -> bool {
                return lhs.Current == rhs.Current;
            }

          private:
            friend class IntrusiveList;

            IntrusiveListNode *Current = nullptr;
        };

        IntrusiveList() = default;

        IntrusiveList(const IntrusiveList &) = delete;
        IntrusiveList &operator=(const IntrusiveList &) = delete;

        auto begin() noexcept -> Iterator { return Iterator{Head}; }
        auto end() noexcept -> Iterator { return Iterator{nullptr}; }

        auto begin() const noexcept -> ConstIterator { return ConstIterator{Head}; }
        auto end() const noexcept -> ConstIterator { return ConstIterator{nullptr}; }

        [[nodiscard]] auto front() noexcept -> T & { return valueOf(*Head); }
        [[nodiscard]] auto front() const noexcept -> const T & { return valueOf(*Head); }

        [[nodiscard]] auto back() noexcept -> T & { return valueOf(*Tail); }
        [[nodiscard]] auto back() const noexcept -> const T & { return valueOf(*Tail); }

        [[nodiscard]] auto empty() const noexcept -> bool { return Head == nullptr; }
        [[nodiscard]] auto size() const noexcept -> std::size_t { return Count; }

        auto pushFront(T &value) noexcept -> void {
            linkBefore(Head, nodeOf(value));
        }

        auto pushBack(T &value) noexcept -> void {
            linkBefore(nullptr, nodeOf(value));
        }

        // 删除 position 指向的元素，返回指向下一个元素的迭代器。
        // 只把节点摘下来，不析构、不释放元素。
        auto erase(Iterator position) noexcept -> Iterator {
            IntrusiveListNode *next = position.Current->Next;
            unlink(*position.Current);
            return Iterator{next};
        }

        // 按元素地址删除，O(n)。调用方手里只有引用时用这个。
        auto erase(T &value) noexcept -> bool {
            auto &target = nodeOf(value);

            for (auto *node = Head; node != nullptr; node = node->Next) {
                if (node == &target) {
                    unlink(*node);
                    return true;
                }
            }
            return false;
        }

        // 只清空链表，元素本身不管（它们的生命周期由调用方负责）。
        // 注意：节点里的 Prev/Next 不会在这里被改写，因为它们已经不属于这张表了。
        auto clear() noexcept -> void {
            Head = nullptr;
            Tail = nullptr;
            Count = 0;
        }

      private:
        static auto nodeOf(T &value) noexcept -> IntrusiveListNode & { return value.*Link; }
        static auto nodeOf(const T &value) noexcept -> const IntrusiveListNode & { return value.*Link; }

        static auto valueOf(IntrusiveListNode &node) noexcept -> T & {
            auto *address = reinterpret_cast<std::byte *>(std::addressof(node));
            return *reinterpret_cast<T *>(address - linkOffset());
        }

        static auto valueOf(const IntrusiveListNode &node) noexcept -> const T & {
            auto *address = reinterpret_cast<const std::byte *>(std::addressof(node));
            return *reinterpret_cast<const T *>(address - linkOffset());
        }

        // 成员在元素里的偏移。
        //
        // 用"取空指针的成员地址"这个惯用法：只做地址运算，不解引用任何内存，
        // 编译器也不会为它生成访存指令。标准库没有 container_of，而 offsetof
        // 只接受成员名字、不吃成员指针，所以这里没有更干净的写法。
        static auto linkOffset() noexcept -> std::size_t {
            static const std::size_t offset =
                reinterpret_cast<std::size_t>(std::addressof(reinterpret_cast<T *>(0)->*Link));
            return offset;
        }

        // 把 node 插到 before 前面（before 为空表示插到表尾）
        auto linkBefore(IntrusiveListNode *before, IntrusiveListNode &node) noexcept -> void {
            node.Next = before;
            node.Prev = (before != nullptr) ? before->Prev : Tail;

            if (node.Prev != nullptr) {
                node.Prev->Next = &node;
            } else {
                Head = &node;
            }
            if (before != nullptr) {
                before->Prev = &node;
            } else {
                Tail = &node;
            }
            ++Count;
        }

        auto unlink(IntrusiveListNode &node) noexcept -> void {
            if (node.Prev != nullptr) {
                node.Prev->Next = node.Next;
            } else {
                Head = node.Next;
            }
            if (node.Next != nullptr) {
                node.Next->Prev = node.Prev;
            } else {
                Tail = node.Prev;
            }

            node.Prev = nullptr;
            node.Next = nullptr;
            --Count;
        }

        IntrusiveListNode *Head = nullptr;
        IntrusiveListNode *Tail = nullptr;
        std::size_t Count = 0;
    };

} // namespace ReZeroDetour
