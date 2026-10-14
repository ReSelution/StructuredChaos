#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <new>

namespace sc::internal {

  // The objects of a resource whose destructors have to run when the resource
  // is reset, newest first. Every object brings the node that remembers it:
  // the node lies in the same block, right behind the object, so that the
  // object itself starts the block and takes its alignment from there.
  //
  // remember() may be called from several threads at the same time.
  class CleanupList {
    struct Node {
      void (*destroy)(Node *) noexcept;
      Node *next;
    };

    // Distance from the object to its node.
    template<typename T>
    static constexpr size_t nodeOffset = (sizeof(T) + alignof(Node) - 1) & ~(alignof(Node) - 1);

  public:
    // Size and alignment of a block for an object and its node.
    template<typename T>
    static constexpr size_t blockSize = nodeOffset<T> + sizeof(Node);

    template<typename T>
    static constexpr size_t blockAlignment = std::max(alignof(T), alignof(Node));

    // Puts the object at the start of the block on the list. Only an object
    // that exists is remembered.
    template<typename T>
    void remember(void *block) noexcept {
      auto *node = new (static_cast<std::byte *>(block) + nodeOffset<T>)
          Node{.destroy =
                   [](Node *n) noexcept {
                     void *start = reinterpret_cast<std::byte *>(n) - nodeOffset<T>;
                     std::launder(static_cast<T *>(start))->~T();
                   },
               .next = m_head.load(std::memory_order_relaxed)};
      while (!m_head.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
      }
    }

    // Newest first. Runs again for what a destructor created meanwhile.
    void runAll() noexcept {
      while (Node *node = m_head.exchange(nullptr, std::memory_order_acq_rel)) {
        while (node != nullptr) {
          // The destructor may not touch the node, but read it first anyway.
          Node *next = node->next;
          node->destroy(node);
          node = next;
        }
      }
    }

  private:
    std::atomic<Node *> m_head{nullptr};
  };

} // namespace sc::internal
