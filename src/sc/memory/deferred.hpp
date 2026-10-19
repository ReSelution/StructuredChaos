#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace sc::memory {

  // Calls functions some frames later, for what the GPU may still be using:
  // a VkImage whose last component goes in frame N may be read by the command
  // buffers of frame N until its fence has signalled.
  //
  //   DeletionQueue deletions(FRAMES_IN_FLIGHT);
  //   ~GpuTexture() { deletions.push([=] { vmaDestroyImage(allocator, image, allocation); }); }
  //
  //   // In the system that begins a frame, once the fence of this slot has signalled:
  //   deletions.beginFrame(frame);
  //
  // What is pushed in frame N lies in slot N % framesInFlight and is called
  // by beginFrame(N + framesInFlight), which the caller only reaches once the
  // fence of frame N has signalled, as the fence of a slot guards it. Pushed
  // before the frame used it or after, it waits as long; before, a frame
  // longer than needed.
  //
  // push is safe from any thread, without a lock, but not next to beginFrame:
  // a push that sees the new frame could land in the slot that is being
  // called. In a schedule, beginFrame is a system that runs alone.
  //
  // The functions of a slot are called in the order each thread pushed them,
  // so that an image view pushed before its image goes first. Every push
  // allocates the function with its captures. A function must not throw: one
  // that does ends the program.
  class DeletionQueue {
    struct Node {
      void (*call)(Node *) noexcept;
      Node *next;
    };

    template<typename F>
    struct Holder : Node {
      F function;
    };

  public:
    explicit DeletionQueue(uint32_t framesInFlight) :
        m_framesInFlight(framesInFlight), m_slots(std::make_unique<Slot[]>(framesInFlight)) {
      assert(framesInFlight != 0);
    }

    // Calls what is left: the caller has waited for the device to be idle.
    ~DeletionQueue() { flushAll(); }

    DeletionQueue(const DeletionQueue &) = delete;
    DeletionQueue &operator=(const DeletionQueue &) = delete;

    template<typename F>
      requires std::is_invocable_v<std::decay_t<F> &>
    void push(F &&function) {
      auto *const node =
          new Holder<std::decay_t<F>>{{.call = &call<std::decay_t<F>>, .next = nullptr}, std::forward<F>(function)};
      std::atomic<Node *> &head = m_slots[m_frame.load(std::memory_order_relaxed) % m_framesInFlight].head;
      node->next = head.load(std::memory_order_relaxed);
      while (!head.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
      }
    }

    // The frame starts: calls what was pushed framesInFlight frames ago, in
    // the slot this frame reuses. Only once the fence of that frame has
    // signalled, and not next to push.
    void beginFrame(uint64_t frame) {
      m_frame.store(frame, std::memory_order_relaxed);
      flush(m_slots[frame % m_framesInFlight]);
    }

    // Calls everything, for shutdown after vkDeviceWaitIdle.
    void flushAll() {
      for (uint32_t slot = 0; slot < m_framesInFlight; ++slot) {
        flush(m_slots[slot]);
      }
    }

    [[nodiscard]] uint32_t framesInFlight() const noexcept { return m_framesInFlight; }

  private:
    struct alignas(64) Slot {
      std::atomic<Node *> head{nullptr};
    };

    template<typename F>
    static void call(Node *node) noexcept {
      std::unique_ptr<Holder<F>> holder(static_cast<Holder<F> *>(node));
      holder->function();
    }

    // The list is newest first: turned around, so that each thread's
    // functions come in the order it pushed them. Again for what a function
    // pushed meanwhile into the same slot.
    static void flush(Slot &slot) {
      while (Node *node = slot.head.exchange(nullptr, std::memory_order_acquire)) {
        Node *oldest = nullptr;
        while (node != nullptr) {
          Node *const next = node->next;
          node->next = oldest;
          oldest = node;
          node = next;
        }
        while (oldest != nullptr) {
          // The function frees the node: next is read before.
          Node *const current = oldest;
          oldest = current->next;
          current->call(current);
        }
      }
    }

    uint32_t m_framesInFlight;
    std::unique_ptr<Slot[]> m_slots;
    std::atomic<uint64_t> m_frame{0};
  };

} // namespace sc::memory
