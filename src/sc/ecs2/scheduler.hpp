#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "entity.hpp"
#include "sc/threading/threading.hpp"
#include "world.hpp"

namespace sc::ecs2::internal {

  // What one thread wants done once its stage is over. On cache lines of its
  // own, the threads next to it write theirs at the same time.
  struct alignas(64) CommandList {
    // A component to remove: the entity and the column of the component.
    struct Erased {
      Entity entity;
      ColumnBase *column;
    };

    std::vector<Erased> erased;
    // The entities to destroy.
    std::vector<Entity> destroyed;

    void clear() noexcept {
      erased.clear();
      destroyed.clear();
    }
  };

} // namespace sc::ecs2::internal

namespace sc::ecs2 {

  // The entities that have the given components, as a parameter of a system:
  //
  //   void move(Query<Position, const Velocity> query) {
  //     query.each([](Position &pos, const Velocity &vel) { ... });
  //   }
  //
  // A const component is only read, Exclude names components the entities must
  // not have. This is what tells the scheduler which systems may run at the
  // same time.
  template <typename... Terms> class Query {
    template <typename Component> static constexpr bool named = (std::same_as<Component, Terms> || ...);

  public:
    explicit Query(World &world) : m_world(&world), m_columns(columnsOf(world, typename Lists::GetList{})) {}

    // See World::each.
    template <typename Function> void each(Function &&function) const { m_world->template each<Terms...>(function); }

    // See World::parallelEach.
    template <typename Function> void parallelEach(Function &&function, size_t grain = 1) const {
      m_world->template parallelEach<Terms...>(function, grain);
    }

    // The component of any entity that exists and has it, named the way the
    // query names it.
    template <typename Component>
      requires named<Component>
    [[nodiscard]] Component &get(Entity entity) const {
      assert(m_world->valid(entity));
      return std::get<World::ColumnOf<Component> *>(m_columns)->get(indexOf(entity));
    }

    template <typename Component>
      requires named<Component> || named<const Component>
    [[nodiscard]] bool contains(Entity entity) const {
      return m_world->template contains<Component>(entity);
    }

  private:
    using Lists = internal::Split<internal::List<>, internal::List<>, Terms...>;

    template <typename... Get> static auto columnsOf(World &world, internal::List<Get...>) {
      return std::tuple<World::ColumnOf<Get> *...>{&world.column<Get>()...};
    }

    World *m_world;
    // Looked up once, so that get does not have to.
    decltype(columnsOf(std::declval<World &>(), typename Lists::GetList{})) m_columns;
  };

  // Lets a system add and remove the components it names, right away:
  //
  //   scheduler.addParallel([](Entity entity, Change<Frozen> change, const Health &health) {
  //     if (health.value < 10.0f) {
  //       change.emplace<Frozen>(entity);
  //     }
  //   });
  //
  // To the scheduler this is writing the components, so no other system uses
  // them at the same time. Within the system:
  //  - a loop must not change the components it goes through;
  //  - the function of a parallel loop uses emplace only for the entity it is
  //    called with and for entities it made with create, which no other
  //    thread knows of. Any other entity it gives a component with
  //    tryEmplace, as several threads may be doing at the same time, and
  //    takes it away with Commands::erase: erase would take it away from
  //    under a thread that is using it.
  template <typename... Components> class Change {
    template <typename Component> static constexpr bool named = (std::same_as<Component, Components> || ...);

  public:
    explicit Change(World &world) : m_world(&world) {}

    // An entity to give the named components to. Safe to call from the
    // function of a parallel loop, like World::create.
    [[nodiscard]] Entity create() const { return m_world->create(); }

    template <typename Component, typename... Args>
      requires named<Component>
    decltype(auto) emplace(Entity entity, Args &&...args) const {
      return m_world->template emplace<Component>(entity, std::forward<Args>(args)...);
    }

    // See World::tryEmplace.
    template <typename Component, typename... Args>
      requires named<Component>
    bool tryEmplace(Entity entity, Args &&...args) const {
      return m_world->template tryEmplace<Component>(entity, std::forward<Args>(args)...);
    }

    template <typename Component>
      requires named<Component>
    bool erase(Entity entity) const {
      return m_world->template erase<Component>(entity);
    }

    template <typename Component>
      requires named<Component>
    [[nodiscard]] bool contains(Entity entity) const {
      return m_world->template contains<Component>(entity);
    }

  private:
    World *m_world;
  };

  // Records what a system cannot do right away: destroying entities, which
  // touches every component, and removing a component another thread may be
  // using. It is done once all systems that run at the same time are done, so
  // nothing an entity has goes away while they run. Safe to use from the
  // function of a parallel loop, for any entity and from several threads for
  // the same one.
  class Commands {
  public:
    Commands(World &world, std::span<internal::CommandList> lists) : m_world(&world), m_lists(lists) {}

    // Does nothing if the entity does not exist any more by then.
    void destroy(Entity entity) const {
      m_lists[static_cast<size_t>(threading::thread_id())].destroyed.push_back(entity);
    }

    // Does nothing if the entity does not exist any more by then or does not
    // have the component. A system that does this does not count as writing
    // the component: the systems that use it run at the same time and still
    // find it.
    template <typename Component> void erase(Entity entity) const {
      m_lists[static_cast<size_t>(threading::thread_id())].erased.push_back(
          {.entity = entity, .column = &m_world->template column<Component>()});
    }

  private:
    World *m_world;
    // One for every sc::threading::thread_id.
    std::span<internal::CommandList> m_lists;
  };

} // namespace sc::ecs2

namespace sc::ecs2::internal {

  // What a system reads and writes, collected from its parameters.
  struct SystemAccess {
    struct Component {
      size_t index;
      bool write;
    };

    std::vector<Component> components;
    // The system takes the world itself and may do anything with it.
    bool exclusive = false;
    bool commands = false;

    // Several parameters may name the same component; writing wins.
    void add(size_t index, bool write) {
      for (Component &component : components) {
        if (component.index == index) {
          component.write = component.write || write;
          return;
        }
      }
      components.push_back({.index = index, .write = write});
    }

    // Whether the two must not run at the same time.
    [[nodiscard]] bool conflictsWith(const SystemAccess &other) const noexcept {
      if (exclusive || other.exclusive) {
        return true;
      }
      for (const Component &mine : components) {
        for (const Component &theirs : other.components) {
          if (mine.index == theirs.index && (mine.write || theirs.write)) {
            return true;
          }
        }
      }
      return false;
    }
  };

  // What the parameters of a system are made from when it runs.
  struct SystemContext {
    World &world;
    std::span<CommandList> commands;
  };

  // How one kind of system parameter is declared to the scheduler and how its
  // value is made when the system runs.
  template <typename Param> struct SystemParam;

  template <typename... Terms> struct SystemParam<Query<Terms...>> {
    static void declare(SystemAccess &access) {
      using Lists = Split<List<>, List<>, Terms...>;
      declare(access, typename Lists::GetList{}, typename Lists::ExcludedList{});
    }

    static Query<Terms...> fetch(const SystemContext &context) { return Query<Terms...>{context.world}; }

  private:
    template <typename... Get, typename... Excluded>
    static void declare(SystemAccess &access, List<Get...>, List<Excluded...>) {
      (access.add(componentIndex<std::remove_const_t<Get>>(), !std::is_const_v<Get>), ...);
      // An excluded component is only looked up, never changed.
      (access.add(componentIndex<std::remove_const_t<Excluded>>(), false), ...);
    }
  };

  template <typename... Components> struct SystemParam<Change<Components...>> {
    static void declare(SystemAccess &access) { (access.add(componentIndex<Components>(), true), ...); }

    static Change<Components...> fetch(const SystemContext &context) { return Change<Components...>{context.world}; }
  };

  template <> struct SystemParam<Commands> {
    static void declare(SystemAccess &access) { access.commands = true; }

    static Commands fetch(const SystemContext &context) { return Commands{context.world, context.commands}; }
  };

  template <> struct SystemParam<World &> {
    static void declare(SystemAccess &access) { access.exclusive = true; }

    static World &fetch(const SystemContext &context) { return context.world; }
  };

  // Queries, Change and Commands are taken by value or by const reference, the
  // world only by reference.
  template <typename Param>
  using SystemParamFor =
      SystemParam<std::conditional_t<std::is_same_v<Param, World &>, World &, std::remove_cvref_t<Param>>>;

  // The parameter types of a system: a function pointer or an object with a
  // single, non-template operator().
  template <typename F> struct SystemSignature : SystemSignature<decltype(&F::operator())> {};

  template <typename R, typename... Args> struct SystemSignature<R (*)(Args...)> {
    using Params = List<Args...>;
  };
  template <typename R, typename... Args>
  struct SystemSignature<R (*)(Args...) noexcept> : SystemSignature<R (*)(Args...)> {};
  template <typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...)> : SystemSignature<R (*)(Args...)> {};
  template <typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) const> : SystemSignature<R (*)(Args...)> {};
  template <typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) noexcept> : SystemSignature<R (*)(Args...)> {};
  template <typename C, typename R, typename... Args>
  struct SystemSignature<R (C::*)(Args...) const noexcept> : SystemSignature<R (*)(Args...)> {};

  // A parameter of a loop function that is neither the entity nor a component.
  template <typename Param> inline constexpr bool isLoopExtra = false;
  template <typename... Components> inline constexpr bool isLoopExtra<Change<Components...>> = true;
  template <> inline constexpr bool isLoopExtra<Commands> = true;
  template <typename... Terms> inline constexpr bool isLoopExtra<Query<Terms...>> = true;

  // The parameters of a function for Scheduler::addParallel: the entity, then
  // Change, Commands and queries, all of them optional, and then the
  // components.
  template <bool WithEntity, typename Extras, typename... Rest> struct LoopRest;

  template <bool WithEntity, typename... Extras, typename... Components>
  struct LoopRest<WithEntity, List<Extras...>, Components...> {
    static constexpr bool ENTITY = WithEntity;
    using ExtraList = List<Extras...>;
    using ComponentList = List<Components...>;
  };

  template <bool WithEntity, typename... Extras, typename First, typename... Rest>
    requires isLoopExtra<std::remove_cvref_t<First>>
  struct LoopRest<WithEntity, List<Extras...>, First, Rest...>
      : LoopRest<WithEntity, List<Extras..., std::remove_cvref_t<First>>, Rest...> {};

  template <typename Params> struct LoopSignature;
  template <typename... Params> struct LoopSignature<List<Params...>> : LoopRest<false, List<>, Params...> {};
  template <typename... Params> struct LoopSignature<List<Entity, Params...>> : LoopRest<true, List<>, Params...> {};

  // The term of a query for a component a loop function takes: only a
  // reference to non-const lets the function write it.
  template <typename Param>
  using TermOf =
      std::conditional_t<std::is_lvalue_reference_v<Param> && !std::is_const_v<std::remove_reference_t<Param>>,
                         std::remove_reference_t<Param>, const std::remove_cvref_t<Param>>;

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
    threading::parallelFor(items.size(), 1, [&](size_t first, size_t last) {
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

} // namespace sc::ecs2::internal

namespace sc::ecs2 {

  // Runs systems on a world, as many of them at the same time as their
  // parameters allow.
  //
  // A system is a function whose parameters say what it uses:
  //
  //   void move(Query<Position, const Velocity> query);   // writes Position, reads Velocity
  //   void freeze(Query<const Health> query, Change<Frozen> change);   // adds and removes Frozen
  //   void reap(Query<const Lifetime> query, Commands commands);   // destroys entities, later
  //   void load(World &world);                             // may do anything, runs alone
  //
  // Two systems run at the same time if neither writes a component the other
  // one uses. Systems that cannot run together keep the order in which they
  // were added. A system without parameters says nothing about what it uses
  // and therefore runs alone, like one that takes the world.
  class Scheduler {
  public:
    template <typename F> Scheduler &add(F &&system, std::string name = {}) {
      using Function = std::decay_t<F>;
      return addSystem(std::forward<F>(system), std::move(name),
                       typename internal::SystemSignature<Function>::Params{});
    }

    // Adds a system that is the body of a loop: the function is called with
    // the components of every entity that has all of them, on several threads
    // at once.
    //
    //   scheduler.addParallel([](Position &pos, const Velocity &vel) { ... });
    //
    // A component taken by reference to non-const is written, any other one is
    // only read. Before the components, the function may take the entity and,
    // after it, Change, Commands and queries, the latter to look at entities
    // other than its own:
    //
    //   scheduler.addParallel([](Entity entity, Commands commands, const Health &health) { ... });
    //   scheduler.addParallel([](Query<const Health> others, const Target &target) { ... });
    //
    // grain is the number of pages a thread takes at a time.
    template <typename F> Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 1) {
      using Signature = internal::LoopSignature<typename internal::SystemSignature<std::decay_t<F>>::Params>;
      return addLoop(std::forward<F>(function), std::move(name), grain, typename Signature::ExtraList{},
                     typename Signature::ComponentList{});
    }

    // The same with the query to go through named, for what the parameters of
    // the function cannot say: excluded components and components the entities
    // need but the function does not take.
    //
    //   scheduler.addParallel<Query<Position, const Velocity, Exclude<Frozen>>>(
    //       [](Position &pos, const Velocity &vel) { ... });
    //
    // The function takes the components in the order the query names them,
    // without the ones that have no members.
    template <typename QueryType, typename F>
    Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 1) {
      using Signature = internal::LoopSignature<typename internal::SystemSignature<std::decay_t<F>>::Params>;
      return addLoopOver(std::type_identity<QueryType>{}, std::forward<F>(function), std::move(name), grain,
                         typename Signature::ExtraList{});
    }

    // Runs every system once. Stages with more than one system and systems
    // added with addParallel run on the thread pool, so sc::threading::init
    // must have been called before, and run must not be called from a pool
    // thread.
    //
    // If a system throws, the systems running at the same time still finish,
    // what they recorded through Commands is not done, later systems are
    // skipped and the first exception is rethrown.
    void run(World &world) {
      const std::vector<std::vector<size_t>> &stages = this->stages();

      // Here and not when the system is added: the pool may be set up later.
      const size_t threads = threading::thread_slots();
      for (System &system : m_systems) {
        if (system.access.commands && system.commands.size() < threads) {
          system.commands = std::vector<internal::CommandList>(threads);
        }
      }

      for (const std::vector<size_t> &stage : stages) {
        try {
          internal::runAll(stage, [this, &world](size_t index) {
            System &system = m_systems[index];
            system.run({.world = world, .commands = system.commands});
          });
        } catch (...) {
          for (const size_t index : stage) {
            for (internal::CommandList &list : m_systems[index].commands) {
              list.clear();
            }
          }
          throw;
        }

        // Now that nothing is running: nobody is using what goes away.
        for (const size_t index : stage) {
          for (internal::CommandList &list : m_systems[index].commands) {
            for (const internal::CommandList::Erased &erased : list.erased) {
              // Not that of an entity that got the index since.
              if (world.valid(erased.entity)) {
                erased.column->erase(indexOf(erased.entity));
              }
            }
            for (const Entity entity : list.destroyed) {
              world.destroy(entity);
            }
            list.clear();
          }
        }
      }
    }

    [[nodiscard]] size_t size() const noexcept { return m_systems.size(); }

    [[nodiscard]] const std::string &name(size_t system) const { return m_systems[system].name; }

    // The systems in the order they run, by index in the order they were
    // added: one stage after the other, the systems of a stage at the same
    // time.
    [[nodiscard]] const std::vector<std::vector<size_t>> &stages() {
      if (!m_stagesValid) {
        // A system runs in the stage after the last earlier one it conflicts
        // with.
        std::vector<size_t> stageOf(m_systems.size(), 0);
        size_t count = 0;
        for (size_t i = 0; i < m_systems.size(); ++i) {
          for (size_t earlier = 0; earlier < i; ++earlier) {
            if (m_systems[i].access.conflictsWith(m_systems[earlier].access)) {
              stageOf[i] = std::max(stageOf[i], stageOf[earlier] + 1);
            }
          }
          count = std::max(count, stageOf[i] + 1);
        }
        m_stages.assign(count, {});
        for (size_t i = 0; i < m_systems.size(); ++i) {
          m_stages[stageOf[i]].push_back(i);
        }
        m_stagesValid = true;
      }
      return m_stages;
    }

  private:
    struct System {
      std::move_only_function<void(const internal::SystemContext &)> run;
      std::string name;
      internal::SystemAccess access;
      // What the threads recorded through Commands, one list for every
      // thread; empty for a system that does not take Commands.
      std::vector<internal::CommandList> commands;
    };

    template <typename F, typename... Params>
    Scheduler &addSystem(F &&system, std::string name, internal::List<Params...>) {
      internal::SystemAccess access;
      (internal::SystemParamFor<Params>::declare(access), ...);
      if (sizeof...(Params) == 0) {
        access.exclusive = true;
      }

      m_systems.push_back(System{
          .run =
              [function = std::decay_t<F>(std::forward<F>(system))](const internal::SystemContext &context) mutable {
                function(internal::SystemParamFor<Params>::fetch(context)...);
              },
          .name = std::move(name),
          .access = std::move(access),
          .commands = {},
      });
      m_stagesValid = false;
      return *this;
    }

    // addParallel for the query of the components the function is called with.
    template <typename F, typename... Extras, typename... Components>
    Scheduler &addLoop(F &&function, std::string name, size_t grain, internal::List<Extras...> extras,
                       internal::List<Components...>) {
      static_assert(sizeof...(Components) != 0, "the function takes the components of an entity");
      static_assert((!std::is_rvalue_reference_v<Components> && ...), "a component is taken by value or reference");
      static_assert((!std::is_empty_v<std::remove_cvref_t<Components>> && ...),
                    "a component without members cannot be a parameter");
      return addLoopOver(std::type_identity<Query<internal::TermOf<Components>...>>{}, std::forward<F>(function),
                         std::move(name), grain, extras);
    }

    template <typename... Terms, typename F, typename... Extras>
    Scheduler &addLoopOver(std::type_identity<Query<Terms...>>, F &&function, std::string name, size_t grain,
                           internal::List<Extras...>) {
      using Function = std::decay_t<F>;
      using Signature = internal::LoopSignature<typename internal::SystemSignature<Function>::Params>;
      return addSystem(
          [function = Function(std::forward<F>(function)), grain](Query<Terms...> query, Extras... extras) mutable {
            if constexpr (Signature::ENTITY) {
              query.parallelEach(
                  [&](Entity entity, auto &...components) { function(entity, extras..., components...); }, grain);
            } else {
              query.parallelEach([&](auto &...components) { function(extras..., components...); }, grain);
            }
          },
          std::move(name), internal::List<Query<Terms...>, Extras...>{});
    }

    std::vector<System> m_systems;
    std::vector<std::vector<size_t>> m_stages;
    bool m_stagesValid = true;
  };

} // namespace sc::ecs2
