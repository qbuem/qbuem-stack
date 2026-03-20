#pragma once

/**
 * @file qbuem/buf/intrusive_list.hpp
 * @brief Zero-allocation doubly-linked intrusive list.
 * @defgroup qbuem_buf_ilist IntrusiveList
 * @ingroup qbuem_buf
 *
 * ## Design
 * Pointers are embedded directly in the stored objects via `IntrusiveNode`.
 * Objects must inherit from `IntrusiveNode` (CRTP or plain). The list
 * manipulates those embedded pointers — no heap allocation ever occurs.
 *
 * ### Benefits
 * - O(1) insertion, removal, and splice.
 * - O(1) removal when only the object pointer is known (not the list).
 * - Objects can participate in multiple lists via multiple base classes.
 * - Fully zero-allocation: the object's lifetime is managed externally.
 *
 * ### Usage
 * @code
 * struct Task : public IntrusiveNode {
 *     int priority;
 * };
 *
 * IntrusiveList<Task> ready_queue;
 * Task t1, t2;
 * ready_queue.push_back(&t1);
 * ready_queue.push_back(&t2);
 *
 * for (Task& t : ready_queue) { ... }
 * ready_queue.remove(&t1);
 * @endcode
 *
 * @warning Objects must be removed from the list before their lifetime ends.
 *          Leaving dangling pointers in the list is undefined behaviour.
 */

#include <cassert>
#include <cstddef>
#include <iterator>

namespace qbuem {

/**
 * @brief Base class that provides intrusive prev/next pointers.
 *
 * Derive from this to make an object insertable into `IntrusiveList<T>`.
 * The pointers are null when the node is not in any list.
 */
struct IntrusiveNode {
    IntrusiveNode* prev{nullptr};
    IntrusiveNode* next{nullptr};

    /** @brief Returns true if the node is currently linked in a list. */
    [[nodiscard]] bool linked() const noexcept { return prev != nullptr || next != nullptr; }
};

/**
 * @brief Zero-allocation doubly-linked list of `IntrusiveNode`-derived objects.
 * @tparam T Element type. Must inherit from `IntrusiveNode`.
 */
template <typename T>
    requires std::is_base_of_v<IntrusiveNode, T>
class IntrusiveList {
public:
    IntrusiveList() noexcept {
        // Sentinel: head_.next points to the first element, head_.prev to the last.
        head_.next = &head_;
        head_.prev = &head_;
    }

    IntrusiveList(const IntrusiveList&)            = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    // Move: transfer all nodes, re-link sentinel pointers to the new head.
    IntrusiveList(IntrusiveList&& other) noexcept {
        head_.next = &head_;
        head_.prev = &head_;
        *this = std::move(other);
    }
    IntrusiveList& operator=(IntrusiveList&& other) noexcept {
        if (this == &other) return *this;
        if (!other.empty()) {
            head_.next          = other.head_.next;
            head_.prev          = other.head_.prev;
            head_.next->prev    = &head_;
            head_.prev->next    = &head_;
            other.head_.next    = &other.head_;
            other.head_.prev    = &other.head_;
        } else {
            head_.next = &head_;
            head_.prev = &head_;
        }
        return *this;
    }

    // ── Capacity ─────────────────────────────────────────────────────────────

    [[nodiscard]] bool   empty() const noexcept { return head_.next == &head_; }
    [[nodiscard]] size_t size()  const noexcept {
        size_t n = 0;
        for (auto* p = head_.next; p != &head_; p = p->next) ++n;
        return n;
    }

    // ── Access ───────────────────────────────────────────────────────────────

    [[nodiscard]] T* front() noexcept {
        assert(!empty());
        return static_cast<T*>(head_.next);
    }
    [[nodiscard]] const T* front() const noexcept {
        assert(!empty());
        return static_cast<const T*>(head_.next);
    }

    [[nodiscard]] T* back() noexcept {
        assert(!empty());
        return static_cast<T*>(head_.prev);
    }
    [[nodiscard]] const T* back() const noexcept {
        assert(!empty());
        return static_cast<const T*>(head_.prev);
    }

    // ── Modifiers ────────────────────────────────────────────────────────────

    /**
     * @brief Insert node at the end of the list. O(1).
     * @param node Must not already be linked.
     */
    void push_back(T* node) noexcept {
        assert(node && !node->linked());
        link_before(&head_, node);
    }

    /**
     * @brief Insert node at the front of the list. O(1).
     * @param node Must not already be linked.
     */
    void push_front(T* node) noexcept {
        assert(node && !node->linked());
        link_before(head_.next, node);
    }

    /**
     * @brief Remove node from wherever it is currently linked. O(1).
     *
     * Safe to call even when `node` belongs to a different IntrusiveList
     * instance, as long as the node is properly linked.
     *
     * @param node Node to remove. Must be currently linked.
     */
    void remove(T* node) noexcept {
        assert(node && node->linked());
        unlink(node);
    }

    /** @brief Remove and return the front node. O(1). */
    [[nodiscard]] T* pop_front() noexcept {
        assert(!empty());
        T* n = front();
        unlink(n);
        return n;
    }

    /** @brief Remove and return the back node. O(1). */
    [[nodiscard]] T* pop_back() noexcept {
        assert(!empty());
        T* n = back();
        unlink(n);
        return n;
    }

    // ── Iterator ─────────────────────────────────────────────────────────────

    struct Iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T&;

        IntrusiveNode* node;

        T& operator*()  const noexcept { return *static_cast<T*>(node); }
        T* operator->() const noexcept { return  static_cast<T*>(node); }

        Iterator& operator++() noexcept { node = node->next; return *this; }
        Iterator  operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        Iterator& operator--() noexcept { node = node->prev; return *this; }
        Iterator  operator--(int) noexcept { auto tmp = *this; --*this; return tmp; }

        bool operator==(const Iterator&) const noexcept = default;
    };

    [[nodiscard]] Iterator begin() noexcept { return {head_.next}; }
    [[nodiscard]] Iterator end()   noexcept { return {&head_}; }

    struct ConstIterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = const T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const T*;
        using reference         = const T&;

        const IntrusiveNode* node;

        const T& operator*()  const noexcept { return *static_cast<const T*>(node); }
        const T* operator->() const noexcept { return  static_cast<const T*>(node); }

        ConstIterator& operator++() noexcept { node = node->next; return *this; }
        ConstIterator  operator++(int) noexcept { auto tmp = *this; ++*this; return tmp; }
        ConstIterator& operator--() noexcept { node = node->prev; return *this; }
        ConstIterator  operator--(int) noexcept { auto tmp = *this; --*this; return tmp; }

        bool operator==(const ConstIterator&) const noexcept = default;
    };

    [[nodiscard]] ConstIterator begin()  const noexcept { return {head_.next}; }
    [[nodiscard]] ConstIterator end()    const noexcept { return {&head_}; }
    [[nodiscard]] ConstIterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] ConstIterator cend()   const noexcept { return end(); }

private:
    // Insert `node` before `pos` in the list.
    static void link_before(IntrusiveNode* pos, IntrusiveNode* node) noexcept {
        node->next       = pos;
        node->prev       = pos->prev;
        pos->prev->next  = node;
        pos->prev        = node;
    }

    // Disconnect `node` from its neighbours.
    static void unlink(IntrusiveNode* node) noexcept {
        node->prev->next = node->next;
        node->next->prev = node->prev;
        node->prev = nullptr;
        node->next = nullptr;
    }

    IntrusiveNode head_; // sentinel (not a T — acts as list boundary)
};

} // namespace qbuem
