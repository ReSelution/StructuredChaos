#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "entity.hpp"
#include "scheduler.hpp"
#include "world.hpp"

namespace sc::ecs {

  // An entity that stands in a relation of the kind to another one, its
  // target: a child to its parent, a weapon to its owner. One target per
  // kind, as an entity has a component once. The target must exist when the
  // relation is made.
  template<typename Kind>
  struct Relation {
    Entity target;
  };

  // The entities that stand in a relation of the kind to this one, in the
  // order they got it. Kept by the hooks of relate; not to be changed past
  // them.
  template<typename Kind>
  struct Related {
    std::vector<Entity> sources;
  };

  // What happens to the sources when their target goes, see relate.
  enum class Cascade : uint8_t {
    // They go with it.
    Destroy,
    // They lose the relation and stay.
    Orphan,
  };

  // The relation of a child to its parent, see relate.
  struct ChildOf {};
  using Parent = Relation<ChildOf>;
  using Children = Related<ChildOf>;

} // namespace sc::ecs

namespace sc::ecs::internal {

  // Commands where a hook runs in a system or after the changes of a stage;
  // none (empty lists) outside one, where the hooks of relate change the
  // world right away instead, as nothing runs next to them there.
  struct CommandsIfAny {
    World *world;
    std::span<CommandList> lists;

    [[nodiscard]] bool recording() const noexcept { return !lists.empty(); }
    [[nodiscard]] Commands commands() const { return Commands{*world, lists}; }
  };

  template<>
  struct SystemParam<CommandsIfAny> {
    static void declare(SystemAccess &access) { access.commands = true; }

    static CommandsIfAny fetch(const SystemContext &context) {
      return {.world = &context.world, .lists = context.commands};
    }
  };

  // The target whose sources the onRemove hook of its Related takes away on
  // this thread right now, without Commands: their own onRemove hooks leave
  // its list alone then, whose making bit this thread holds.
  template<typename Kind>
  inline thread_local Entity goingTarget = null;

  // Marks a world that relate was called for with the kind.
  template<typename Kind>
  struct Relating {};

} // namespace sc::ecs::internal

namespace sc::ecs {

  // Keeps Related<Kind> of every target in step with the Relation<Kind> of
  // its sources, through hooks of the world: a source that gets the relation
  // is added to the list of its target, one that loses it, or goes, is taken
  // out, and when the target goes, its sources go with it or lose the
  // relation, as cascade says. Through Commands where the hook runs in a
  // system or after the changes of a stage, so in rounds, right away
  // outside one.
  //
  //   relate<ChildOf>(world, Cascade::Destroy);
  //   commands.emplace<Parent>(arm, body);   // body gets Children{arm}
  //   commands.destroy(body);                // arm goes with it
  //
  // The list is changed through Edit::emplaceOrModify, under its making bit,
  // so many threads may give one target its sources at the same time. To
  // give a source another target, erase the relation and emplace the new
  // one, in this order; emplaceOrReplace would get past the hooks and
  // asserts in debug builds, and modify must not change the target either.
  // std::logic_error for a kind related twice. Not while systems run.
  template<typename Kind>
  World &relate(World &world, Cascade cascade) {
    using Source = Relation<Kind>;
    using Target = Related<Kind>;
    if (world.tryGlobal<internal::Relating<Kind>>() != nullptr) {
      throw std::logic_error("sc::ecs: relate for a kind it was called for already");
    }
    world.setGlobal<internal::Relating<Kind>>();
    world.column<Source>().forbidReplace();

    world.onAdd<Source>([](Entity source, Source &relation, Edit<Target> targets) {
      targets.emplaceOrModify(
          relation.target, [&](Target &target) { target.sources.push_back(source); }, std::vector<Entity>{source});
    });

    world.onRemove<Source>([](Entity source, Source &relation, Edit<Target> targets) {
      // Its list goes with it, see goingTarget; one that is gone already has
      // none.
      if (relation.target == internal::goingTarget<Kind> || !targets.template contains<Target>(relation.target)) {
        return;
      }
      targets.modify(relation.target, [&](Target &target) { std::erase(target.sources, source); });
    });

    world.onRemove<Target>([&world, cascade](Entity target, Target &related, internal::CommandsIfAny recorder) {
      if (recorder.recording()) {
        const Commands commands = recorder.commands();
        for (const Entity source: related.sources) {
          if (cascade == Cascade::Destroy) {
            commands.destroy(source);
          } else {
            commands.erase<Source>(source);
          }
        }
        return;
      }
      const auto before = std::exchange(internal::goingTarget<Kind>, target);
      for (const Entity source: related.sources) {
        if (!world.valid(source)) {
          continue;
        }
        if (cascade == Cascade::Destroy) {
          world.destroy(source);
        } else {
          world.erase<Source>(source);
        }
      }
      internal::goingTarget<Kind> = before;
    });
    return world;
  }

  // Calls the function with every source of the target, in their order.
  // From is the world or a query that takes Related<Kind>, which reads the
  // list as its get does: not next to what changes the sources of the
  // target.
  template<typename Kind, typename From, typename Function>
  void forEachRelated(const From &from, Entity target, Function &&function) {
    if (const auto *const related = from.template tryGet<const Related<Kind>>(target); related != nullptr) {
      for (const Entity source: related->sources) {
        function(source);
      }
    }
  }

  // The same for the sources of the sources and so on, depth first: every
  // entity before those that stand in the relation to it, a parent before
  // its children. Not the root itself.
  template<typename Kind, typename From, typename Function>
  void forEachDescendant(const From &from, Entity root, Function &&function) {
    std::vector<Entity> stack;
    forEachRelated<Kind>(from, root, [&](Entity source) { stack.push_back(source); });
    // In their order: the last pushed is the next taken.
    std::ranges::reverse(stack);
    while (!stack.empty()) {
      const Entity entity = stack.back();
      stack.pop_back();
      function(entity);
      const size_t before = stack.size();
      forEachRelated<Kind>(from, entity, [&](Entity source) { stack.push_back(source); });
      std::reverse(stack.begin() + static_cast<std::ptrdiff_t>(before), stack.end());
    }
  }

} // namespace sc::ecs
