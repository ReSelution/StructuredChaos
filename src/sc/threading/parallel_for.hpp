#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <type_traits>

#include "backend.hpp"
#include "types.hpp"

namespace sc::threading::internal {

  // onTicket is null or called with the ticket of the loop before the
  // calling thread starts on it, see sc::threading::parallelFor.
  template<typename F, typename OnTicket = std::nullptr_t>
  void parallelFor(size_t count, size_t grain, size_t helpers, F &function, OnTicket &&onTicket = nullptr) {
    constexpr bool wantsTicket = !std::is_same_v<std::decay_t<OnTicket>, std::nullptr_t>;
    grain = std::max<size_t>(grain, 1);
    if (count <= grain) {
      if (count != 0) {
        function(size_t{0}, count);
      }
      return;
    }
    if (helpers == 0 && !wantsTicket) {
      // Nothing to share: no job, no tickets, nobody is woken up.
      for (size_t first = 0; first < count; first += grain) {
        function(first, std::min(first + grain, count));
      }
      return;
    }

    struct Job : ParallelJob {
      // The first item nobody has taken yet.
      alignas(64) std::atomic<size_t> next{0};
      size_t count = 0;
      size_t grain = 0;
      F *function = nullptr;
      std::atomic_flag failed;
      std::exception_ptr error;
    };

    Job job;
    job.count = count;
    job.grain = grain;
    job.function = &function;
    job.run = [](ParallelJob &base, StopCheck stop, void *context) {
      Job &job = static_cast<Job &>(base);
      try {
        // A helper that stops early leaves nothing behind: the owner works on
        // the loop until the last chunk is taken.
        while (stop == nullptr || !stop(context)) {
          const size_t first = job.next.fetch_add(job.grain, std::memory_order_relaxed);
          if (first >= job.count) {
            break;
          }
          (*job.function)(first, std::min(first + job.grain, job.count));
        }
      } catch (...) {
        if (!job.failed.test_and_set(std::memory_order_acq_rel)) {
          job.error = std::current_exception();
        }
        // Nobody starts another chunk.
        job.next.store(job.count, std::memory_order_relaxed);
      }
    };

    // One thread per chunk at most, this one included.
    uint64_t ticket = 0;
    ParallelSlot *slot =
        shareParallel(job, std::min(helpers, ((count + grain - 1) / grain) - 1), wantsTicket ? &ticket : nullptr);
    if constexpr (wantsTicket) {
      if (slot != nullptr) {
        try {
          onTicket(LoopTicket{ticket});
        } catch (...) {
          // Nobody starts a chunk, the loop ends with the exception.
          job.next.store(job.count, std::memory_order_relaxed);
          finishParallel(*slot);
          throw;
        }
      }
    }
    job.run(job, nullptr, nullptr);
    if (slot != nullptr) {
      finishParallel(*slot);
    }

    if (job.error) {
      std::rethrow_exception(job.error);
    }
  }

} // namespace sc::threading::internal
