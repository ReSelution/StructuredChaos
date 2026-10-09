#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <entt/core/fwd.hpp>
#include <entt/core/type_info.hpp>
#include <entt/core/type_traits.hpp>
#include <entt/entity/component.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>

#include "commands.hpp"
#include "group.hpp"
#include "registry.hpp"
#include "sc/threading/threading.hpp"
#include "view.hpp"

namespace sc::ecs::internal {

  // The components of an entity in the storage with the given index. Those of
  // the leading storage are at hand and need no lookup.
  template <size_t Leading, size_t Index, typename View, typename Components>
  decltype(auto) componentsOf(const View &view, entt::entity entity, const Components &leading) {
    if constexpr (Leading == Index) {
      return leading;
    } else {
      return view.template storage<Index>()->get_as_tuple(entity);
    }
  }

  // eachInRange for a view whose smallest storage is the one with the index
  // Leading.
  template <size_t Leading, typename... Get, typename... Exclude, typename Function, size_t... Index>
  void eachOf(const entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> &view, size_t first,
              size_t last, Function &function, std::index_sequence<Index...>) {
    auto *storage = view.template storage<Leading>();
    using Storage = std::remove_pointer_t<decltype(storage)>;
    const typename Storage::base_type &entities = *storage;
    const auto offset = static_cast<std::ptrdiff_t>(first);

    const auto visit = [&](entt::entity entity, const auto &leading) {
      const bool excluded = [&]<size_t... Excluded>(std::index_sequence<Excluded...>) {
        return (view.template storage<sizeof...(Get) + Excluded>()->contains(entity) || ...);
      }(std::index_sequence_for<Exclude...>{});
      // A storage that keeps its order leaves a tombstone where an entity was.
      if ((Storage::storage_policy != entt::deletion_policy::in_place || entity != entt::tombstone) &&
          ((Leading == Index || view.template storage<Index>()->contains(entity)) && ...) && !excluded) {
        std::apply(function, std::tuple_cat(componentsOf<Leading, Index>(view, entity, leading)...));
      }
    };

    auto entity = entities.begin() + offset;
    const auto end = entities.begin() + static_cast<std::ptrdiff_t>(last);
    // EnTT keeps no instances of a component without members.
    if constexpr (entt::component_traits<typename Storage::element_type, entt::entity>::page_size != 0u) {
      for (auto component = storage->begin() + offset; entity != end; ++entity, ++component) {
        visit(*entity, std::forward_as_tuple(*component));
      }
    } else {
      for (; entity != end; ++entity) {
        visit(*entity, std::tuple<>{});
      }
    }
  }

  // Calls the function with the components of the entities of a view that are
  // at the given positions of its smallest storage, from first up to the one
  // before last. Like the each of the view that takes a function, this walks
  // that storage directly and only looks up the other components.
  template <typename... Get, typename... Exclude, typename Function>
  void eachInRange(const entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> &view, size_t first,
                   size_t last, Function &function) {
    [&]<size_t... Index>(std::index_sequence<Index...> all) {
      ((view.handle() == view.template storage<Index>() ? eachOf<Index>(view, first, last, function, all) : void()),
       ...);
    }(std::index_sequence_for<Get...>{});
  }

  // The component an owned storage of a group has at a position.
  template <typename Storage> auto componentAt(Storage &storage, std::ptrdiff_t position) {
    // EnTT keeps no instances of a component without members.
    if constexpr (std::is_void_v<typename Storage::value_type>) {
      return std::make_tuple();
    } else {
      return std::forward_as_tuple(storage.rbegin()[position]);
    }
  }

  // The same for a group, whose entities are at the given positions of its
  // list. The owned components are at the same positions of their storages
  // and need no lookup, the other ones are looked up.
  template <typename... Owned, typename... Get, typename... Exclude, typename Function>
  void eachInRange(
      const entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> &group,
      size_t first, size_t last, Function &function) {
    [&]<size_t... O, size_t... G>(std::index_sequence<O...>, std::index_sequence<G...>) {
      const std::tuple owned{group.template storage<O>()...};
      const std::tuple others{group.template storage<sizeof...(Owned) + G>()...};
      // Reversed, an EnTT storage goes from its first entity to its last.
      const auto entities = group.handle().rbegin();
      for (auto position = static_cast<std::ptrdiff_t>(first); position < static_cast<std::ptrdiff_t>(last);
           ++position) {
        [[maybe_unused]] const entt::entity entity = entities[position];
        std::apply(function, std::tuple_cat(componentAt(*std::get<O>(owned), position)...,
                                            std::get<G>(others)->get_as_tuple(entity)...));
      }
    }(std::index_sequence_for<Owned...>{}, std::index_sequence_for<Get...>{});
  }

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Calls the function with the components of every entity of a view, like
  // the each of the view that takes a function, but on the calling thread and
  // on every pool thread that has nothing else to do:
  //
  //   void move(View<Position, const Velocity> view) {
  //     parallelEach(view, [](Position &pos, const Velocity &vel) { ... });
  //   }
  //
  // The function is called from several threads at once, each of them with
  // other entities, grain of them in a row.
  template <typename... Get, typename... Exclude, typename Function>
  void parallelEach(entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> view, Function &&function,
                    size_t grain = 512) {
    if (view.handle() == nullptr) [[unlikely]] {
      return;
    }
    threading::parallelFor(view.handle()->size(), grain, [&](size_t first, size_t last) {
      internal::eachInRange(view, first, last, function);
    });
  }

  // The same for the entities of a group.
  template <typename... Owned, typename... Get, typename... Exclude, typename Function>
  void parallelEach(entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> group,
                    Function &&function, size_t grain = 512) {
    // No entities in a group the registry does not have.
    threading::parallelFor(group.size(), grain, [&](size_t first, size_t last) {
      internal::eachInRange(group, first, last, function);
    });
  }

} // namespace sc::ecs

namespace sc::ecs::internal {

  // What a system reads and writes, collected from its parameters.
  struct SystemAccess {
    struct Component {
      entt::id_type id;
      bool write;
    };

    std::vector<Component> components;
    // The system takes the registry itself and may do anything with it.
    bool exclusive = false;
    // Makes the buffer for the changes the system records through its
    // Commands; null for a system that does not take any.
    std::unique_ptr<CommandBufferBase> (*makeCommands)() = nullptr;

    // Several parameters may name the same component; writing wins.
    void add(entt::id_type id, bool write) {
      for (Component &component : components) {
        if (component.id == id) {
          component.write = component.write || write;
          return;
        }
      }
      components.push_back({.id = id, .write = write});
    }
  };

  // What the parameters of a system are made from when it runs.
  struct SystemContext {
    Registry &registry;
    // Null for a system that does not take Commands.
    CommandBufferBase *commands;
  };

  // How one kind of system parameter is declared to the scheduler and how its
  // value is made when the system runs. Specialized for every kind of parameter
  // a system may have.
  template <typename Param> struct SystemParam;

  template <typename... Get, typename... Exclude>
  struct SystemParam<entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>>> {
    using ViewType = entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>>;

    static void declare(SystemAccess &access) {
      (access.add(entt::type_hash<typename Get::element_type>::value(), !std::is_const_v<Get>), ...);
      // An excluded component is only looked up, never changed.
      (access.add(entt::type_hash<typename Exclude::element_type>::value(), false), ...);
    }

    static ViewType fetch(const SystemContext &context) {
      return ViewType{context.registry.storage<entt::constness_as_t<typename Get::element_type, Get>>()...,
                      context.registry.storage<entt::constness_as_t<typename Exclude::element_type, Exclude>>()...};
    }
  };

  template <typename... Owned, typename... Get, typename... Exclude>
  struct SystemParam<entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>>> {
    using GroupType = entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>>;

    static void declare(SystemAccess &access) {
      (access.add(entt::type_hash<typename Owned::element_type>::value(), !std::is_const_v<Owned>), ...);
      (access.add(entt::type_hash<typename Get::element_type>::value(), !std::is_const_v<Get>), ...);
      (access.add(entt::type_hash<typename Exclude::element_type>::value(), false), ...);
    }

    // Creates the group if the registry does not have it yet.
    static void prepare(Registry &registry) { get(registry); }

    static GroupType fetch(const SystemContext &context) { return get(context.registry); }

  private:
    static GroupType get(Registry &registry) {
      return registry.group<entt::constness_as_t<typename Owned::element_type, Owned>...>(
          entt::get<entt::constness_as_t<typename Get::element_type, Get>...>,
          entt::exclude<entt::constness_as_t<typename Exclude::element_type, Exclude>...>);
    }
  };

  template <> struct SystemParam<Registry &> {
    static void declare(SystemAccess &access) { access.exclusive = true; }

    static Registry &fetch(const SystemContext &context) { return context.registry; }
  };

  template <typename... Components> struct SystemParam<Commands<Components...>> {
    using Buffer = CommandBuffer<Components...>;

    static void declare(SystemAccess &access) {
      access.makeCommands = +[]() -> std::unique_ptr<CommandBufferBase> { return std::make_unique<Buffer>(); };
    }

    static Commands<Components...> fetch(const SystemContext &context) {
      return Commands<Components...>{context.registry, static_cast<Buffer &>(*context.commands)};
    }
  };

  template <typename Param> inline constexpr bool isCommands = false;
  template <typename... Components> inline constexpr bool isCommands<Commands<Components...>> = true;

  // Views, groups and Commands are taken by value or by const reference, the
  // registry only by reference.
  template <typename Param>
  using SystemParamFor =
      SystemParam<std::conditional_t<std::is_same_v<Param, Registry &>, Registry &, std::remove_cvref_t<Param>>>;

  // Does what a parameter needs done on a registry before any system runs.
  template <typename Param> void prepareSystemParam([[maybe_unused]] Registry &registry) {
    if constexpr (requires { SystemParamFor<Param>::prepare(registry); }) {
      SystemParamFor<Param>::prepare(registry);
    }
  }

  // The parameter types of a system: a function pointer or an object with a
  // single, non-template operator().
  template <typename F> struct SystemSignature : SystemSignature<decltype(&F::operator())> {};

  template <typename R, typename... Args> struct SystemSignature<R (*)(Args...)> {
    using Params = entt::type_list<Args...>;
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

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Runs systems on a registry, as many of them at the same time as their
  // parameters allow.
  //
  // A system is a function whose parameters say what it uses:
  //
  //   void move(View<Position, const Velocity> view);   // writes Position, reads Velocity
  //   void spawn(View<const Spawner> view, Commands<Position> commands);   // records changes
  //   void collide(Group<entt::owned_t<Position, const Shape>> group);   // like a view
  //   void load(Registry &registry);                    // may do anything, runs alone
  //
  // Two systems run at the same time if neither writes a component the other
  // one uses. Systems that cannot run together keep the order in which they
  // were added. A system without parameters says nothing about what it uses
  // and therefore runs alone, like one that takes the registry.
  //
  // A system must not add or remove components and must not destroy entities
  // on its own. It either records that through Commands, which does not keep
  // it from running next to other systems, or takes the registry.
  class Scheduler {
  public:
    template <typename F> Scheduler &add(F &&system, std::string name = {}) {
      using Function = std::decay_t<F>;
      return addSystem(std::forward<F>(system), std::move(name), typename internal::SystemSignature<Function>::Params{});
    }

    // Adds a system that is the body of a loop: the function is called with
    // the components of every entity that has all of them, on several threads
    // at once, see parallelEach.
    //
    //   scheduler.addParallel([](Position &pos, const Velocity &vel) { ... });
    //
    // A component taken by reference to non-const is written, any other one is
    // only read.
    template <typename F> Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 512) {
      using Function = std::decay_t<F>;
      return addEach(std::forward<F>(function), std::move(name), grain,
                     typename internal::SystemSignature<Function>::Params{});
    }

    // The same with the view or group to go through named, for what the
    // parameters of the function cannot say: excluded components, components
    // the entities need but the function does not take, and groups.
    //
    //   scheduler.addParallel<View<Position, const Velocity, Exclude<Frozen>>>(
    //       [](Position &pos, const Velocity &vel) { ... });
    //   scheduler.addParallel<Group<entt::owned_t<Position, const Velocity>>>(
    //       [](Position &pos, const Velocity &vel) { ... });
    //
    // The function takes the components in the order the view or group names
    // them, without the ones that have no members.
    template <typename Source, typename F>
    Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 512) {
      return addSystem(
          [function = std::decay_t<F>(std::forward<F>(function)), grain](Source source) mutable {
            parallelEach(source, function, grain);
          },
          std::move(name), entt::type_list<Source>{});
    }

    // Runs every system once. Stages with more than one system and systems
    // added with addParallel run on the thread pool, so sc::threading::init
    // must have been called before, and run must not be called from a pool
    // thread.
    //
    // If a system throws, the systems running at the same time still finish,
    // what they recorded through Commands is dropped, later systems are
    // skipped and the first exception is rethrown.
    void run(Registry &registry);

    [[nodiscard]] size_t size() const noexcept { return m_systems.size(); }

    [[nodiscard]] const std::string &name(size_t system) const { return m_systems[system].name; }

    // The systems in the order they run, by index in the order they were
    // added: one stage after the other, the systems of a stage at the same
    // time.
    [[nodiscard]] const std::vector<std::vector<size_t>> &stages();

  private:
    struct System {
      std::move_only_function<void(const internal::SystemContext &)> run;
      std::string name;
      internal::SystemAccess access;
      // Null for a system that does not take Commands.
      std::unique_ptr<internal::CommandBufferBase> commands;
      // Only for a system with parameters that need it, see prepareSystemParam.
      void (*prepare)(Registry &) = nullptr;
    };

    template <typename F, typename... Params>
    Scheduler &addSystem(F &&system, std::string name, entt::type_list<Params...>) {
      // The buffer of a system has the type of its one Commands parameter.
      static_assert((size_t{internal::isCommands<std::remove_cvref_t<Params>>} + ... + 0) <= 1,
                    "a system takes at most one Commands parameter");

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
          .access = access,
          .commands = access.makeCommands != nullptr ? access.makeCommands() : nullptr,
          .prepare = (requires(Registry &registry) {
            internal::SystemParamFor<Params>::prepare(registry);
          } || ...) ? +[](Registry &registry) { (internal::prepareSystemParam<Params>(registry), ...); } : nullptr,
      });
      m_stagesValid = false;
      return *this;
    }

    // addParallel for the view of the components the function is called with.
    template <typename F, typename... Components>
    Scheduler &addEach(F &&function, std::string name, size_t grain, entt::type_list<Components...>) {
      static_assert(sizeof...(Components) != 0, "the function takes the components of an entity");
      static_assert((!std::is_rvalue_reference_v<Components> && ...), "a component is taken by value or reference");
      // EnTT keeps no instances of such a component, so there is none to pass.
      static_assert(
          ((entt::component_traits<std::remove_cvref_t<Components>, entt::entity>::page_size != 0u) && ...),
          "a component without members cannot be a parameter");

      // Only a reference to non-const lets the function write the component.
      using ViewType = View<
          std::conditional_t<std::is_reference_v<Components>, std::remove_reference_t<Components>,
                             const Components>...>;
      return addParallel<ViewType>(std::forward<F>(function), std::move(name), grain);
    }

    // How the changes the systems of a stage recorded are made.
    struct CommandPlan {
      // The queues of the stage: one family after the other, in a family one
      // component after the other, for a component in the order of the systems.
      std::vector<internal::CommandQueueBase *> queues;
      // The queues of every family, which one task works off.
      std::vector<std::span<internal::CommandQueueBase *const>> families;
    };

    void buildStages();

    // Which components make up a family depends on the groups of the registry.
    void planCommands(const Registry &registry);

    // Makes the changes the systems of a stage recorded.
    void applyCommands(Registry &registry, size_t stage);

    void dropCommands(const std::vector<size_t> &stage) noexcept;

    std::vector<System> m_systems;
    std::vector<std::vector<size_t>> m_stages;
    // One for every stage. Made for the registry with the id m_planRegistry at
    // its Registry::familyVersion m_planFamilies, and made anew for another.
    std::vector<CommandPlan> m_plans;
    uint64_t m_planRegistry = 0;
    uint64_t m_planFamilies = 0;
    // Scratch space of applyCommands: the families with changes.
    std::vector<std::span<internal::CommandQueueBase *const>> m_pending;
    bool m_stagesValid = true;
  };

} // namespace sc::ecs
