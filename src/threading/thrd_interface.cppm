module;

#include <cstdint>
#include <thread>

export module sc.threading:interface;

import :backend;

namespace sc::threading {

  export void init(uint32_t threads = std::thread::hardware_concurrency() - 1) { impl::init_impl(threads); }

  export void wait_until_finished() { impl::wait_until_finished(); }

} // namespace sc::threading
