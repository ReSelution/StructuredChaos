#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
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

  // EnTT keeps the components of a storage in pages, in the order of the
  // storage's entities. A loop that goes through a page as an array is one the
  // compiler turns into SIMD code, unlike one that goes through the iterators
  // of the storage, which look up the page of every single component.

  // How many components are left on the page of a storage from the given
  // position on.
  template <typename Storage> size_t leftOnPage(size_t position) {
    // EnTT keeps no instances of a component without members.
    if constexpr (std::is_void_v<typename Storage::value_type>) {
      return std::numeric_limits<size_t>::max();
    } else {
      constexpr size_t SIZE =
          entt::component_traits<typename Storage::element_type, typename Storage::entity_type>::page_size;
      return SIZE - position % SIZE;
    }
  }

  // The component a storage has at the given position, followed by the rest of
  // its page. Null for a component without members.
  template <typename Storage> auto pageAt(Storage &storage, size_t position) {
    if constexpr (std::is_void_v<typename Storage::value_type>) {
      return nullptr;
    } else {
      constexpr size_t SIZE =
          entt::component_traits<typename Storage::element_type, typename Storage::entity_type>::page_size;
      return storage.raw()[position / SIZE] + position % SIZE;
    }
  }

  // The component at an index of what pageAt returned.
  template <typename Page> auto componentOn(Page page, size_t index) {
    if constexpr (std::is_null_pointer_v<Page>) {
      return std::tuple<>{};
    } else {
      return std::forward_as_tuple(page[index]);
    }
  }

  // Whether a storage has the given entities at the same positions as the
  // storage they are from.
  template <typename Storage>
  bool sameEntities(const Storage &storage, const entt::entity *entities, size_t position, size_t count) {
    const typename Storage::base_type &set = storage;
    // Not std::equal, which compares enumerations one by one.
    return position + count <= set.size() &&
           std::memcmp(entities, set.data() + position, count * sizeof(entt::entity)) == 0;
  }

  // Whether a function takes the entity before the components of a tuple.
  template <typename Function, typename Components> inline constexpr bool takesEntity = false;
  template <typename Function, typename... Components>
  inline constexpr bool takesEntity<Function, std::tuple<Components...>> =
      std::is_invocable_v<Function &, entt::entity, Components...>;

  // Calls the function with the components of an entity and, if it takes one,
  // with the entity before them.
  template <typename Function, typename Components>
  void callWith(Function &function, [[maybe_unused]] entt::entity entity, Components &&components) {
    if constexpr (takesEntity<Function, std::remove_cvref_t<Components>>) {
      std::apply(function, std::tuple_cat(std::tuple{entity}, std::forward<Components>(components)));
    } else {
      std::apply(function, std::forward<Components>(components));
    }
  }

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
  void eachOf(const entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> &view, size_t first, size_t last,
              Function &function, std::index_sequence<Index...>) {
    const std::tuple storages{view.template storage<Index>()...};
    auto &storage = *std::get<Leading>(storages);
    using Storage = std::remove_reference_t<decltype(storage)>;
    const typename Storage::base_type &set = storage;
    const entt::entity *entities = set.data();

    // Storages that got their components for the same entities in the same
    // order, which is what happens to components that are added together,
    // have them at the same positions. Where that holds, no component has to
    // be looked up. Not with a storage that keeps its order, which has gaps,
    // and not with excluded components, which are looked up in any case.
    constexpr bool PACKED =
        sizeof...(Exclude) == 0 && ((Get::storage_policy == entt::deletion_policy::swap_and_pop) && ...);

    const auto visit = [&](entt::entity entity, const auto &leading) {
      const bool excluded = [&]<size_t... Excluded>(std::index_sequence<Excluded...>) {
        return (view.template storage<sizeof...(Get) + Excluded>()->contains(entity) || ...);
      }(std::index_sequence_for<Exclude...>{});
      // A storage that keeps its order leaves a tombstone where an entity was.
      if ((Storage::storage_policy != entt::deletion_policy::in_place || entity != entt::tombstone) &&
          ((Leading == Index || std::get<Index>(storages)->contains(entity)) && ...) && !excluded) {
        callWith(function, entity, std::tuple_cat(componentsOf<Leading, Index>(view, entity, leading)...));
      }
    };

    for (size_t position = first; position < last;) {
      // Up to where every storage has its components in one piece.
      const size_t count = std::min({last - position, leftOnPage<Get>(position)...});
      const entt::entity *chunk = entities + position;

      bool aligned = false;
      if constexpr (PACKED) {
        aligned = ((Leading == Index || sameEntities(*std::get<Index>(storages), chunk, position, count)) && ...);
      }

      if (aligned) {
        const std::tuple pages{pageAt(*std::get<Index>(storages), position)...};
        for (size_t i = 0; i < count; ++i) {
          callWith(function, chunk[i], std::tuple_cat(componentOn(std::get<Index>(pages), i)...));
        }
      } else {
        const auto leading = pageAt(storage, position);
        for (size_t i = 0; i < count; ++i) {
          visit(chunk[i], componentOn(leading, i));
        }
      }
      position += count;
    }
  }

  // Calls the function with the components of the entities of a view that are
  // at the given positions of its smallest storage, from first up to the one
  // before last, and with the entity before them if the function takes one. Like the each of the view that takes a
  // function, this walks that storage directly and only looks up the other components, and not even those where the
  // storages have the same entities at the same positions.
  template <typename... Get, typename... Exclude, typename Function>
  void eachInRange(const entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> &view, size_t first,
                   size_t last, Function &function) {
    [&]<size_t... Index>(std::index_sequence<Index...> all) {
      ((view.handle() == view.template storage<Index>() ? eachOf<Index>(view, first, last, function, all) : void()),
       ...);
    }(std::index_sequence_for<Get...>{});
  }

  // The same for a group, whose entities are at the given positions of its
  // list. The owned components are at the same positions of their storages
  // and need no lookup, the other ones are looked up.
  template <typename... Owned, typename... Get, typename... Exclude, typename Function>
  void
  eachInRange(const entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> &group,
              size_t first, size_t last, Function &function) {
    [&]<size_t... O, size_t... G>(std::index_sequence<O...>, std::index_sequence<G...>) {
      const std::tuple owned{group.template storage<O>()...};
      const std::tuple others{group.template storage<sizeof...(Owned) + G>()...};
      const entt::entity *entities = group.handle().data();

      for (size_t position = first; position < last;) {
        // Up to where every owned storage has its components in one piece.
        const size_t count = std::min({last - position, leftOnPage<Owned>(position)...});
        const std::tuple pages{pageAt(*std::get<O>(owned), position)...};
        const entt::entity *chunk = entities + position;
        for (size_t i = 0; i < count; ++i) {
          callWith(
              function, chunk[i],
              std::tuple_cat(componentOn(std::get<O>(pages), i)..., std::get<G>(others)->get_as_tuple(chunk[i])...));
        }
        position += count;
      }
    }(std::index_sequence_for<Owned...>{}, std::index_sequence_for<Get...>{});
  }

  // How many positions eachInRange has to go through for all entities of a
  // view or group.
  template <typename... Get, typename... Exclude>
  size_t extent(const entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> &view) {
    // Null for a view of a component the registry has no storage for.
    return view.handle() != nullptr ? view.handle()->size() : 0;
  }

  template <typename... Owned, typename... Get, typename... Exclude>
  size_t
  extent(const entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> &group) {
    return group.size();
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
  // other entities, grain of them in a row. It gets the entity as well if it
  // takes an entt::entity before the components.
  template <typename... Get, typename... Exclude, typename Function>
  void parallelEach(entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> view, Function &&function,
                    size_t grain = 512) {
    threading::parallelFor(internal::extent(view), grain,
                           [&](size_t first, size_t last) { internal::eachInRange(view, first, last, function); });
  }

  // The same for the entities of a group.
  template <typename... Owned, typename... Get, typename... Exclude, typename Function>
  void parallelEach(entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> group,
                    Function &&function, size_t grain = 512) {
    threading::parallelFor(internal::extent(group), grain,
                           [&](size_t first, size_t last) { internal::eachInRange(group, first, last, function); });
  }

  // The same on the calling thread alone. Other than the each of an EnTT view
  // or group, this goes through the components page by page, see eachInRange,
  // which is several times faster for a function that does little.
  //
  // The function must not add or remove components of the view or group.
  template <typename... Get, typename... Exclude, typename Function>
  void each(entt::basic_view<entt::get_t<Get...>, entt::exclude_t<Exclude...>> view, Function &&function) {
    internal::eachInRange(view, 0, internal::extent(view), function);
  }

  template <typename... Owned, typename... Get, typename... Exclude, typename Function>
  void each(entt::basic_group<entt::owned_t<Owned...>, entt::get_t<Get...>, entt::exclude_t<Exclude...>> group,
            Function &&function) {
    internal::eachInRange(group, 0, internal::extent(group), function);
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
    // The system records them from several threads and needs a buffer for
    // every thread, see ParallelCommands.
    bool commandsPerThread = false;

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
    // Empty for a system that does not take Commands, one buffer for every
    // thread for a system that takes ParallelCommands, else a single one.
    std::span<const std::unique_ptr<CommandBufferBase>> commands;
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
      return Commands<Components...>{context.registry, static_cast<Buffer &>(*context.commands.front())};
    }
  };

  template <typename... Components> struct SystemParam<ParallelCommands<Components...>> {
    static void declare(SystemAccess &access) {
      SystemParam<Commands<Components...>>::declare(access);
      access.commandsPerThread = true;
    }

    static ParallelCommands<Components...> fetch(const SystemContext &context) {
      return ParallelCommands<Components...>{context.registry, context.commands};
    }
  };

  template <typename Param> inline constexpr bool isCommands = false;
  template <typename... Components> inline constexpr bool isCommands<Commands<Components...>> = true;
  template <typename... Components> inline constexpr bool isCommands<ParallelCommands<Components...>> = true;

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

  // Not a generic lambda, whose parameters depend on what it is called with.
  template <typename F>
  concept HasSignature = std::is_pointer_v<F> || requires { &F::operator(); };

  // The parameters of a function for Scheduler::addParallel: the entity and
  // Commands, both optional, and then the components.
  template <typename Params> struct EachSignature;

  template <typename... Components> struct EachSignature<entt::type_list<Components...>> {
    // Void for a function that does not take Commands.
    using CommandsType = void;
    using ComponentList = entt::type_list<Components...>;
  };

  template <typename... Rest>
  struct EachSignature<entt::type_list<entt::entity, Rest...>> : EachSignature<entt::type_list<Rest...>> {};

  template <typename First, typename... Components>
    requires isCommands<std::remove_cvref_t<First>>
  struct EachSignature<entt::type_list<First, Components...>> {
    using CommandsType = std::remove_cvref_t<First>;
    using ComponentList = entt::type_list<Components...>;
  };

  // The Commands a function for Scheduler::addParallel takes, or void.
  template <typename F> struct EachCommands {
    using type = void;
  };
  template <HasSignature F> struct EachCommands<F> {
    using type = typename EachSignature<typename SystemSignature<F>::Params>::CommandsType;
  };

  // The ParallelCommands for the same components as the given Commands.
  template <typename CommandsType> struct ParallelOf;
  template <typename... Components> struct ParallelOf<Commands<Components...>> {
    using type = ParallelCommands<Components...>;
  };

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
      return addSystem(std::forward<F>(system), std::move(name),
                       typename internal::SystemSignature<Function>::Params{});
    }

    // Adds a system that is the body of a loop: the function is called with
    // the components of every entity that has all of them, on several threads
    // at once, see parallelEach.
    //
    //   scheduler.addParallel([](Position &pos, const Velocity &vel) { ... });
    //
    // A component taken by reference to non-const is written, any other one is
    // only read.
    //
    // Before the components, the function may take the entity and Commands, to
    // record changes like any other system:
    //
    //   scheduler.addParallel([](entt::entity entity, Commands<Frozen> commands, const Health &health) {
    //     if (health.value <= 0.0f) {
    //       commands.destroy(entity);
    //     }
    //   });
    //
    // Every thread records into a buffer of its own. The changes of a thread
    // are made in the order it recorded them, those of different threads in no
    // particular order.
    template <typename F> Scheduler &addParallel(F &&function, std::string name = {}, size_t grain = 512) {
      using Function = std::decay_t<F>;
      using Signature = internal::EachSignature<typename internal::SystemSignature<Function>::Params>;
      return addEach(std::forward<F>(function), std::move(name), grain, typename Signature::ComponentList{});
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
      using Function = std::decay_t<F>;
      using CommandsType = typename internal::EachCommands<Function>::type;

      if constexpr (std::is_void_v<CommandsType>) {
        return addSystem([function = Function(std::forward<F>(function)),
                          grain](Source source) mutable { parallelEach(source, function, grain); },
                         std::move(name), entt::type_list<Source>{});
      } else {
        using Parallel = typename internal::ParallelOf<CommandsType>::type;
        return addSystem(
            [function = Function(std::forward<F>(function)), grain](Source source, Parallel commands) mutable {
              threading::parallelFor(internal::extent(source), grain, [&](size_t first, size_t last) {
                // Once for the entities of a thread's share, not for every one.
                CommandsType local = commands.local();
                auto call = [&](entt::entity entity, auto &&...components) {
                  if constexpr (std::is_invocable_v<Function &, entt::entity, CommandsType &,
                                                    decltype(components)...>) {
                    function(entity, local, components...);
                  } else {
                    function(local, components...);
                  }
                };
                internal::eachInRange(source, first, last, call);
              });
            },
            std::move(name), entt::type_list<Source, Parallel>{});
      }
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
      // What the system recorded through its Commands: no buffer for a system
      // that does not take any, one for every thread for a system that takes
      // ParallelCommands, which run makes, else a single one.
      std::vector<std::unique_ptr<internal::CommandBufferBase>> commands;
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
          .commands = {},
          .prepare = (requires(Registry &registry) {
            internal::SystemParamFor<Params>::prepare(registry);
          } || ...) ? +[](Registry &registry) { (internal::prepareSystemParam<Params>(registry), ...); } : nullptr,
      });
      if (access.makeCommands != nullptr) {
        m_systems.back().commands.push_back(access.makeCommands());
      }
      m_stagesValid = false;
      return *this;
    }

    // addParallel for the view of the components the function is called with.
    template <typename F, typename... Components>
    Scheduler &addEach(F &&function, std::string name, size_t grain, entt::type_list<Components...>) {
      static_assert(sizeof...(Components) != 0, "the function takes the components of an entity");
      static_assert((!std::is_rvalue_reference_v<Components> && ...), "a component is taken by value or reference");
      // EnTT keeps no instances of such a component, so there is none to pass.
      static_assert(((entt::component_traits<std::remove_cvref_t<Components>, entt::entity>::page_size != 0u) && ...),
                    "a component without members cannot be a parameter");

      // Only a reference to non-const lets the function write the component.
      using ViewType = View<std::conditional_t<std::is_reference_v<Components>, std::remove_reference_t<Components>,
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
