#include "threading.hpp"

#include <cstdint>
void sc::threading::init(uint32_t threads) { impl::init_impl(threads); }

void sc::threading::wait_until_finished() { impl::wait_until_finished(); }

size_t sc::threading::thread_slots() noexcept { return 1 + impl::longRunningThreads.size() + impl::threads.size(); }
