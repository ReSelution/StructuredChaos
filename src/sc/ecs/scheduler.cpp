#include "scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <exception>
#include <span>

#include <entt/graph/flow.hpp>

#include "sc/threading/threading.hpp"

namespace {

  // Calls the function for every item, for more than one item on the thread
  // pool, and returns once all calls are done. Rethrows the first exception.
  template <typename Items, typename Function> void runAll(const Items &items, Function &&function) {
    if (items.empty()) [[unlikely]] {
      return;
    }
    if (items.size() == 1) {
      function(items.front());
      return;
    }

    // Caught here and not by parallelFor, which would not start the calls
    // that are left: every call is made, whatever the others do.
    std::atomic_flag failed;
    std::exception_ptr error;
    // The calling thread takes its share of the items and never sleeps while
    // it waits for the rest.
    sc::threading::parallelFor(items.size(), 1, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        try {
          function(items[i]);
        } catch (...) {
          if (!failed.test_and_set(std::memory_order_acq_rel)) {
            error = std::current_exception();
          }
        }
      }
    });

    if (error) {
      std::rethrow_exception(error);
    }
  }

} // namespace

namespace sc::ecs {

  const std::vector<std::vector<size_t>> &Scheduler::stages() {
    if (!m_stagesValid) {
      buildStages();
    }
    return m_stages;
  }

  void Scheduler::buildStages() {
    // Every system reads this resource and an exclusive one writes it, which
    // puts an exclusive system in conflict with all others.
    const entt::id_type registryResource = entt::type_hash<Registry>::value();

    entt::flow flow;
    for (size_t i = 0; i < m_systems.size(); ++i) {
      const internal::SystemAccess &access = m_systems[i].access;
      flow.bind(static_cast<entt::id_type>(i));
      flow.set(registryResource, access.exclusive);
      for (const auto &component : access.components) {
        flow.set(component.id, component.write);
      }
    }

    // An edge leads from a system to one that has to wait for it. A system
    // runs in the stage after the last one it waits for.
    const auto graph = flow.graph();
    std::vector<size_t> stageOf(m_systems.size(), 0);
    size_t stageCount = 0;
    for (size_t i = 0; i < m_systems.size(); ++i) {
      for (const auto [from, to] : graph.in_edges(i)) {
        // Systems only wait for ones added before them.
        assert(from < to);
        stageOf[i] = std::max(stageOf[i], stageOf[from] + 1);
      }
      stageCount = std::max(stageCount, stageOf[i] + 1);
    }

    m_stages.assign(stageCount, {});
    for (size_t i = 0; i < m_systems.size(); ++i) {
      m_stages[stageOf[i]].push_back(i);
    }
    m_stagesValid = true;
    // No registry has this id.
    m_planRegistry = 0;
  }

  void Scheduler::planCommands(const Registry &registry) {
    m_plans.assign(m_stages.size(), {});
    for (size_t stage = 0; stage < m_stages.size(); ++stage) {
      CommandPlan &plan = m_plans[stage];
      for (const size_t system : m_stages[stage]) {
        if (const auto &commands = m_systems[system].commands) {
          plan.queues.insert(plan.queues.end(), commands->queues().begin(), commands->queues().end());
        }
      }

      // The queues of one component stay in the order of their systems and are
      // worked off by a single task: a storage has one writer. A group ties its
      // components together into one family, which shares a task.
      const auto family = [&registry](const internal::CommandQueueBase *queue) {
        return registry.family(queue->componentIndex);
      };
      std::stable_sort(plan.queues.begin(), plan.queues.end(), [&family](const auto *lhs, const auto *rhs) {
        return family(lhs) != family(rhs) ? family(lhs) < family(rhs) : lhs->componentIndex < rhs->componentIndex;
      });
      for (size_t first = 0; first < plan.queues.size();) {
        size_t last = first + 1;
        while (last < plan.queues.size() && family(plan.queues[last]) == family(plan.queues[first])) {
          ++last;
        }
        plan.families.emplace_back(plan.queues.data() + first, last - first);
        first = last;
      }
    }
    m_planRegistry = registry.id();
    m_planFamilies = registry.familyVersion();
  }

  void Scheduler::run(Registry &registry) {
    // Creating a group reorders storages, so it has to happen before anything
    // runs. For a group that exists already this is only a lookup.
    for (const System &system : m_systems) {
      if (system.prepare != nullptr) {
        system.prepare(registry);
      }
    }

    const std::vector<std::vector<size_t>> &stages = this->stages();
    for (size_t stage = 0; stage < stages.size(); ++stage) {
      try {
        runAll(stages[stage], [this, &registry](size_t index) {
          System &system = m_systems[index];
          system.run({.registry = registry, .commands = system.commands.get()});
        });
      } catch (...) {
        dropCommands(stages[stage]);
        throw;
      }
      applyCommands(registry, stage);
    }
  }

  void Scheduler::applyCommands(Registry &registry, size_t stage) {
    // Checked for every stage: a system that takes the registry may have
    // created a group in the one before.
    if (m_planRegistry != registry.id() || m_planFamilies != registry.familyVersion()) {
      planCommands(registry);
    }

    // Different families do not touch each other, so their tasks run at the
    // same time.
    m_pending.clear();
    for (const std::span<internal::CommandQueueBase *const> family : m_plans[stage].families) {
      if (!std::ranges::all_of(family, &internal::CommandQueueBase::empty)) {
        m_pending.push_back(family);
      }
    }
    runAll(m_pending, [&registry](std::span<internal::CommandQueueBase *const> queues) {
      for (internal::CommandQueueBase *queue : queues) {
        queue->apply(registry);
      }
    });

    // Destroying an entity writes every component, so it comes after and has
    // nothing running next to it.
    for (const size_t system : m_stages[stage]) {
      if (const auto &commands = m_systems[system].commands) {
        if (!commands->destroyed.empty()) {
          registry.destroy(commands->destroyed.begin(), commands->destroyed.end());
          commands->destroyed.clear();
        }
      }
    }
  }

  void Scheduler::dropCommands(const std::vector<size_t> &stage) noexcept {
    for (const size_t system : stage) {
      if (const auto &commands = m_systems[system].commands) {
        commands->clear();
      }
    }
  }

} // namespace sc::ecs
