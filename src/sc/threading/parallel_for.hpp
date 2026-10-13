#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <type_traits>

#include "backend.hpp"

namespace sc::threading::impl {

  template<typename F>
  void parallelFor(size_t count, size_t grain, size_t helpers, F &function) {
    grain = std::max<size_t>(grain, 1);
    if (count <= grain) {
      if (count != 0) {
        function(size_t{0}, count);
      }
      return;
    }
    if (helpers == 0) {
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
    job.run = [](ParallelJob &base) {
      Job &job = static_cast<Job &>(base);
      try {
        while (true) {
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
    ParallelSlot *slot = shareParallel(job, std::min(helpers, ((count + grain - 1) / grain) - 1));
    job.run(job);
    if (slot != nullptr) {
      finishParallel(*slot);
    }

    if (job.error) {
      std::rethrow_exception(job.error);
    }
  }

} // namespace sc::threading::impl
