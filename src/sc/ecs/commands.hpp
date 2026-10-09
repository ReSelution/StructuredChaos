#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <entt/entity/component.hpp>
#include <entt/entity/fwd.hpp>

#include "entity.hpp"
#include "registry.hpp"
#include "sc/threading/threading.hpp"

namespace sc::ecs::internal {

  // The changes a system recorded for one component type. On cache lines of
  // its own: the queues of different components are worked off by different
  // threads at the same time.
  struct alignas(64) CommandQueueBase {
    explicit CommandQueueBase(size_t componentIndex) : componentIndex(componentIndex) {}
    virtual ~CommandQueueBase() = default;

    // Where a removal was recorded after an addition: everything before these
    // positions of removed and added has to be done before the rest.
    struct Split {
      size_t removed;
      size_t added;
    };

    // Makes the changes in the registry, in the order they were recorded, and
    // forgets them. Writes the storage of the component and, if it is part of
    // a group, of its family.
    virtual void apply(Registry &registry) = 0;
    virtual void clear() noexcept = 0;

    [[nodiscard]] bool empty() const noexcept { return removed.empty() && added.empty(); }

    void remove(entt::entity e) {
      if (added.size() != (splits.empty() ? 0 : splits.back().added)) {
        // Something was added since the last removal.
        splits.push_back({.removed = removed.size(), .added = added.size()});
      }
      removed.push_back(e);
    }

    size_t componentIndex;
    std::vector<entt::entity> removed;
    std::vector<entt::entity> added;
    // Empty unless a system removes a component after adding one, so that
    // there is usually one range to remove and one to add.
    std::vector<Split> splits;
  };

  template <typename Component> struct CommandQueue final : CommandQueueBase {
    // EnTT keeps no instances of such a component, so there is nothing to
    // record besides the entity.
    static constexpr bool HAS_VALUE = entt::component_traits<Component, entt::entity>::page_size != 0u;

    CommandQueue() : CommandQueueBase(internal::componentIndex<Component>()) {}

    void apply(Registry &registry) override {
      auto &storage = registry.storage<Component>();
      Split done{.removed = 0, .added = 0};
      // Removes and then adds everything up to the given positions.
      const auto advance = [&](Split to) {
        using Offset = std::ptrdiff_t;
        storage.remove(removed.begin() + static_cast<Offset>(done.removed),
                       removed.begin() + static_cast<Offset>(to.removed));
        const auto first = added.begin() + static_cast<Offset>(done.added);
        const auto last = added.begin() + static_cast<Offset>(to.added);
        if constexpr (HAS_VALUE) {
          storage.insert(first, last, std::make_move_iterator(values.begin() + static_cast<Offset>(done.added)));
        } else {
          storage.insert(first, last);
        }
        done = to;
      };
      for (const Split split : splits) {
        advance(split);
      }
      advance({.removed = removed.size(), .added = added.size()});
      clear();
    }

    void clear() noexcept override {
      removed.clear();
      added.clear();
      splits.clear();
      values.clear();
    }

    // The components to add, one for every entity of added.
    std::vector<Component> values;
  };

  // The changes a system recorded through its Commands, until the scheduler
  // makes them: the part that is the same for every system. Every system has
  // its own buffer, so recording needs no locking.
  struct alignas(64) CommandBufferBase {
    CommandBufferBase() = default;
    virtual ~CommandBufferBase() { releaseSpares(); }

    CommandBufferBase(const CommandBufferBase &) = delete;
    CommandBufferBase &operator=(const CommandBufferBase &) = delete;

    // One queue for every component the system named in its Commands.
    [[nodiscard]] virtual std::span<CommandQueueBase *const> queues() noexcept = 0;

    void clear() noexcept {
      for (CommandQueueBase *queue : queues()) {
        queue->clear();
      }
      destroyed.clear();
    }

    // Gives the spare entities that are left back to their registry, if it
    // still exists.
    void releaseSpares() {
      if (spareNext != ENTITY_BLOCK_SIZE) {
        Registry::release(spareRegistry, spareEntities.begin() + static_cast<std::ptrdiff_t>(spareNext),
                          spareEntities.end());
        spareNext = ENTITY_BLOCK_SIZE;
      }
    }

    std::vector<entt::entity> destroyed;

    // Entities created in advance for Commands::create, so that a system goes
    // to the registry once per block instead of once per entity. The ones a
    // system does not get to use are kept for its next run and given back when
    // the buffer goes away or is used with another registry.
    static constexpr size_t ENTITY_BLOCK_SIZE = 256;
    std::array<Entity, ENTITY_BLOCK_SIZE> spareEntities{};
    size_t spareNext = ENTITY_BLOCK_SIZE;
    // Registry::id of the registry the spare entities belong to.
    uint64_t spareRegistry = 0;
  };

  template <typename... Components> struct CommandBuffer final : CommandBufferBase {
    template <typename Component> CommandQueue<Component> &queue() noexcept {
      return std::get<CommandQueue<Component>>(m_queues);
    }

    [[nodiscard]] std::span<CommandQueueBase *const> queues() noexcept override { return m_all; }

  private:
    std::tuple<CommandQueue<Components>...> m_queues;
    std::array<CommandQueueBase *, sizeof...(Components)> m_all{&std::get<CommandQueue<Components>>(m_queues)...};
  };

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Lets a system change the structure of the registry: create and destroy
  // entities, and add and remove the components it names. Taken by value as a
  // parameter of a system:
  //
  //   void spawn(View<const Spawner> spawners, Commands<Position, Velocity> commands);
  //   void reap(View<const Lifetime> lifetimes, Commands<> commands);   // only destroys
  //
  // Nothing but create happens right away. The changes are recorded and made
  // once all systems that run at the same time are done, so the system itself
  // and the ones running next to it do not see them, the ones after do.
  //
  // They are made in the order the systems were added and, for one system and
  // one component, in the order it recorded them; entities are destroyed last.
  //
  // The components of different types are written at the same time, except
  // for the components of a group, which are written one after the other.
  // Belongs to one system and must not be used from several threads.
  template <typename... Components> class Commands {
    template <typename Component> static constexpr bool named = (std::same_as<Component, Components> || ...);

  public:
    using Buffer = internal::CommandBuffer<Components...>;

    Commands(Registry &registry, Buffer &buffer) : m_registry(&registry), m_buffer(&buffer) {
      if (buffer.spareRegistry != registry.id()) {
        // Left over from another registry.
        buffer.releaseSpares();
        buffer.spareRegistry = registry.id();
      }
    }

    // The entity exists right away; safe next to everything else.
    Entity create() {
      if (m_buffer->spareNext == Buffer::ENTITY_BLOCK_SIZE) [[unlikely]] {
        m_registry->create(m_buffer->spareEntities.begin(), m_buffer->spareEntities.end());
        m_buffer->spareNext = 0;
      }
      return m_buffer->spareEntities[m_buffer->spareNext++];
    }

    // The same for a range of entities, which goes to the registry once.
    template <typename It>
      requires IsEntity<typename std::iterator_traits<It>::value_type>
    void create(It first, It last) {
      m_registry->create(first, last);
    }

    // The component is built now and moved into the registry later. The entity
    // must not have the component by then.
    template <typename Component, typename... Args>
      requires named<Component>
    void emplace(entt::entity e, Args &&...args) {
      auto &queue = m_buffer->template queue<Component>();
      if constexpr (internal::CommandQueue<Component>::HAS_VALUE) {
        [[maybe_unused]] const auto anchor = m_registry->anchor<Component>();
        queue.values.emplace_back(std::forward<Args>(args)...);
      }
      queue.added.push_back(e);
    }

    // Does nothing if the entity does not have the component by then.
    template <typename Component>
      requires named<Component>
    void remove(entt::entity e) {
      m_buffer->template queue<Component>().remove(e);
    }

    // Removes the entity with all of its components, including the ones that
    // were added in the meantime.
    void destroy(entt::entity e) { m_buffer->destroyed.push_back(e); }

  private:
    Registry *m_registry;
    Buffer *m_buffer;
  };

  // Commands for a system that records changes from several threads at once,
  // as the function of a parallelEach does. Every thread has a buffer of its
  // own, so recording needs no locking here either:
  //
  //   void reap(View<const Lifetime> view, ParallelCommands<> commands) {
  //     parallelEach(view, [&](entt::entity entity, const Lifetime &lifetime) {
  //       if (lifetime.left <= 0) {
  //         commands.local().destroy(entity);
  //       }
  //     });
  //   }
  //
  // The changes of a thread are made in the order it recorded them, those of
  // different threads in no particular order: which thread gets which entities
  // differs from run to run.
  template <typename... Components> class ParallelCommands {
  public:
    using Buffer = internal::CommandBuffer<Components...>;
    using Buffers = std::span<const std::unique_ptr<internal::CommandBufferBase>>;

    // One buffer for every sc::threading::thread_id.
    ParallelCommands(Registry &registry, Buffers buffers) : m_registry(&registry), m_buffers(buffers) {}

    // The Commands of the calling thread, which must not be handed to another
    // one. Best taken once for many entities.
    [[nodiscard]] Commands<Components...> local() const {
      const auto thread = static_cast<size_t>(threading::thread_id());
      assert(thread < m_buffers.size());
      return Commands<Components...>{*m_registry, static_cast<Buffer &>(*m_buffers[thread])};
    }

  private:
    Registry *m_registry;
    Buffers m_buffers;
  };

} // namespace sc::ecs
