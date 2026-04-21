//
// Created by oleub on 07.04.26.
//#include <iostream>

#include <span>

#include "ecs/chaos_registry.hpp"
#include "ecs/chaos_resource.hpp"
#include "threading/chaos_threading.hpp"
#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"

LOG_ALIAS(RegLog, "Chaos", "Registry");

DEFINE_CHAOS_CORE_STAT(RegistryThroughput, "Registry emplace", SC::ChaosThroughput<SC::MetricUnits>);

DEFINE_CHAOS_CORE_STAT(RegisteryManipulate, "Registry manipulate", SC::ChaosThroughput<SC::MetricUnits>);

// Konfiguration
constexpr size_t TOTAL_ENTITIES = 100000;
constexpr size_t BATCH_SIZE = 5000; // Tasks pro Enqueue-Welle

struct MeshComponent {
  SC::ChaosResource<std::span<const float> > vertices;
  uint32_t meshId;
};

struct TransformComponent {
  // 4x4 Matrix für Rotation, Translation und Skalierung
  glm::mat4 transform{1.0f};
};

struct PositionComponent {
  glm::vec3 pos{0.0f};
};

struct PhysicsComponent {
  // AABB (Bounding Box) und Bewegungsdaten
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
  glm::vec3 velocity{0.0f};
  float mass{1.0f};
};

struct TagComponent {
  // Hier bleiben wir bei char-Arrays oder std::string,
  // da GLM nur für Mathe da ist.
  std::array<char, 32> name;
};

struct AIStateComponent {
  int currentState;
  glm::vec3 targetPos{0.0f};
  float stamina{100.0f};
};

struct MaterialComponent {
  uint64_t albedoId;
  uint64_t normalId;
  float roughness;
  float metallic;
  uint32_t shaderFlags;
};

struct RenderStateComponent {
  enum class RenderLayer : uint8_t { Opaque, Transparent, UI, PostProcess };

  RenderLayer layer{RenderLayer::Opaque};
  uint32_t sortOrder{0};
  bool castShadows{true};
  bool receiveShadows{true};
  bool isVisible{true};
  uint8_t stencilMask{0xFF};
};

struct AudioComponent {
  SC::ChaosResource<std::span<const int16_t> > audioSamples;
  float volume{1.0f};
  float pitch{1.0f};
  bool loop{false};
  bool is3D{true};
  float minDistance{1.0f};
  float maxDistance{500.0f};
};

struct NetworkComponent {
  uint64_t networkId;
  uint64_t lastStateHash{0};
  uint32_t ownerPeerId;
  bool isDirty{false};
  uint8_t replicationPriority{100}; // 0-255
};

struct RelationshipComponent {
  SC::ChaosEntity parent{};
  std::array<SC::ChaosEntity, 4> children{};
  uint32_t childCount{0};
};

struct ScriptComponent {
  uint32_t scriptId;
  float updateInterval{0.016f}; // 60 FPS
  float timeSinceLastUpdate{0.0f};
  bool isActive{true};
  // Ein Handle auf eine Script-Instanz oder Bytecode
  SC::ChaosResource<std::span<uint8_t> > scriptBytecode;
};

struct MoveCopySpy {
  static inline std::atomic<size_t> copyCount{0};
  static inline std::atomic<size_t> moveCount{0};
  static inline std::atomic<size_t> ctorCount{0};

  float data[4]{0.0f}; // Simuliert einen vec4

  MoveCopySpy() { ctorCount++; }

  // Kopier-Konstruktor
  MoveCopySpy(const MoveCopySpy &other) {
    copyCount++;
    for (int i = 0; i < 4; ++i) data[i] = other.data[i];
  }

  // Move-Konstruktor
  MoveCopySpy(MoveCopySpy &&other) noexcept {
    moveCount++;
    for (int i = 0; i < 4; ++i) data[i] = other.data[i];
  }

  static void reset() {
    copyCount = 0;
    moveCount = 0;
    ctorCount = 0;
  }
};


void run_registry_stress_test() {

  RegLog::info("Starting High-Throughput Test: {} Entitäten... (Single Component)", TOTAL_ENTITIES);
  {
    SC::ChaosRegistry registry;

    registry.reserve<TransformComponent>(TOTAL_ENTITIES);
    registry.reserve<PhysicsComponent>(TOTAL_ENTITIES);
    registry.reserve<MaterialComponent>(TOTAL_ENTITIES);
    registry.reserve<TagComponent>(TOTAL_ENTITIES);
    registry.reserve<MeshComponent>(TOTAL_ENTITIES);
    std::vector<int> entitiesStart;
    entitiesStart.reserve(TOTAL_ENTITIES / BATCH_SIZE + 1);
    for (int i = 0; i < TOTAL_ENTITIES; i += BATCH_SIZE) {
      entitiesStart.emplace_back(i);
    }
    auto t = RegLog::time("Registry-Processing of {1} took {0}", TOTAL_ENTITIES);
    RegistryThroughput::reset();
    SC::ChaosThreading::detacheBatch(
      std::move(entitiesStart), [&registry](int thread_id, int start) {
        for (int i = 0; i < BATCH_SIZE; i++) {
          float rawData[200];
          auto e = registry.create();
          e.add<TransformComponent>();
          e.add<PhysicsComponent>();
          e.add<MaterialComponent>();
          e.add<TagComponent>();
          e.add<AIStateComponent>();
          e.add<MeshComponent>(std::span<const float>(rawData, 200),
                               static_cast<uint32_t>(start + i));
        }
        RegistryThroughput::record(BATCH_SIZE * 6);
      }, nullptr

    );
    SC::ChaosThreading::wait_until_finished();
    RegistryThroughput::stop();
    auto time1 = RegLog::time("Manipulating  of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    auto &&[lock, view] = registry.view<PhysicsComponent>();
    // RegisteryManipulate::reset();
    // RegisteryManipulate::start();
    // const auto &iter = view.each();
    // SC::ChaosThreading::parralelFor(iter, [](auto e, auto &phy) {
    //   for (int i = 0; i < 100; ++i) {
    //     phy.velocity = phy.velocity * 0.99f + glm::vec3(std::sin(i), std::cos(i), 0.0f);
    //   }
    // });
    // RegisteryManipulate::record(view.size());
    // RegisteryManipulate::stop();
  }

  RegLog::stats<RegistryThroughput, RegisteryManipulate>("");
}

void run_registry_stress_test_batch() {
  RegLog::info("Starting High-Throughput Test: {} Entitäten... (Batch Insert)", TOTAL_ENTITIES);
  {
    SC::ChaosRegistry registry;
    std::vector<int> entitiesStart;
    entitiesStart.reserve(TOTAL_ENTITIES / BATCH_SIZE + 1);
    for (int i = 0; i < TOTAL_ENTITIES; i += BATCH_SIZE) {
      entitiesStart.emplace_back(i);
    }
    RegistryThroughput::reset();
    auto t = RegLog::time("Registry-Processing of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    SC::ChaosThreading::detacheBatch(
      std::move(entitiesStart), [&registry](int thread_id, int start) {
        // 1. Entities erstellen
        std::vector<SC::ChaosEntity> entities{BATCH_SIZE};
        registry.create(entities.begin(), entities.end());

        // 2. Transform (Standard)
        std::vector<TransformComponent> transforms{BATCH_SIZE, TransformComponent{}};
        registry.insert<TransformComponent>(entities.begin(), entities.end(),
                                            std::make_move_iterator(transforms.begin()));
        RegistryThroughput::record(BATCH_SIZE);

        // 3. Physics (Data heavy)
        std::vector<PhysicsComponent> physics{BATCH_SIZE, PhysicsComponent{}};
        registry.insert<PhysicsComponent>(entities.begin(), entities.end(), std::make_move_iterator(physics.begin()));
        RegistryThroughput::record(BATCH_SIZE);

        // 4. Material (IDs & Flags)
        std::vector<MaterialComponent> materials{BATCH_SIZE, MaterialComponent{}};
        registry.insert<MaterialComponent>(entities.begin(), entities.end(),
                                           std::make_move_iterator(materials.begin()));
        RegistryThroughput::record(BATCH_SIZE);

        // 5. Tags (Strings/Char Arrays)
        std::vector<TagComponent> tags{BATCH_SIZE, TagComponent{}};
        registry.insert<TagComponent>(entities.begin(), entities.end(), std::make_move_iterator(tags.begin()));
        RegistryThroughput::record(BATCH_SIZE);

        std::vector<AIStateComponent> ai{BATCH_SIZE, AIStateComponent{}};
        registry.insert<AIStateComponent>(entities.begin(), entities.end(), std::make_move_iterator(ai.begin()));
        RegistryThroughput::record(BATCH_SIZE);

        // 6. Mesh (Die fette Payload mit ChaosResource)
        std::vector<MeshComponent> meshComponents;
        meshComponents.reserve(BATCH_SIZE);
        registry.setStorageAnchor<MeshComponent>();
        for (int j = 0; j < BATCH_SIZE; ++j) {
          float rawData[200]; // Simulierter Vertex-Buffer
          meshComponents.emplace_back(std::span<const float>(rawData, 200),
                                      static_cast<uint32_t>(start + j));
        }
        registry.insert<MeshComponent>(entities.begin(), entities.end(),
                                       std::make_move_iterator(meshComponents.begin()));
        RegistryThroughput::record(BATCH_SIZE);
        //
        // std::vector<RenderStateComponent> renderStates{BATCH_SIZE, RenderStateComponent{}};
        // registry.insert<RenderStateComponent>(entities.begin(), entities.end(),  std::make_move_iterator(renderStates.begin()));
        // RegistryThroughput::record(BATCH_SIZE);

        // // 8. Audio (Resource Simulation)
        // std::vector<AudioComponent> audioComponents;
        // registry.setStorageAnchor<AudioComponent>();
        // audioComponents.reserve(BATCH_SIZE);
        // for (int j = 0; j < BATCH_SIZE; ++j) {
        //   static int16_t dummySamples[1024]; // Simulierter Audio-Buffer
        //   audioComponents.emplace_back(std::span<const int16_t>(dummySamples, 1024), 1.0f, 1.0f);
        // }
        // registry.insert<AudioComponent>(entities.begin(), entities.end(),
        //                                 std::make_move_iterator(audioComponents.begin()));
        // RegistryThroughput::record(BATCH_SIZE);
        //
        // // 9. Network (ID & Hash Tracking)
        // std::vector<NetworkComponent> networkComponents;
        // networkComponents.reserve(BATCH_SIZE);
        // for (int j = 0; j < BATCH_SIZE; ++j) {
        //   networkComponents.emplace_back(static_cast<uint64_t>(start + j), 0ULL, 1u);
        // }
        // registry.insert<NetworkComponent>(entities.begin(), entities.end(),
        //                                   std::make_move_iterator(networkComponents.begin()));
        // RegistryThroughput::record(BATCH_SIZE);
        //
        // // 10. Relationship (Hierarchy Handling)
        // std::vector<RelationshipComponent> relationships{BATCH_SIZE, RelationshipComponent{}};
        // registry.insert<RelationshipComponent>(entities.begin(), entities.end(), relationships.begin());
        // RegistryThroughput::record(BATCH_SIZE);
        //
        // // 11. Scripts (Bytecode Payload)
        // std::vector<ScriptComponent> scriptComponents;
        // registry.setStorageAnchor<ScriptComponent>();
        // scriptComponents.reserve(BATCH_SIZE);
        // for (int j = 0; j < BATCH_SIZE; ++j) {
        //   static uint8_t dummyBytecode[64]; // Simulierter Script-Bytecode
        //   scriptComponents.emplace_back(
        //     static_cast<uint32_t>(j),
        //     0.016f, 0.0f, true,
        //     std::span<uint8_t>(dummyBytecode, 64)
        //   );
        // }
        // registry.insert<ScriptComponent>(entities.begin(), entities.end(),
        //                                  std::make_move_iterator(scriptComponents.begin()));
        // RegistryThroughput::record(BATCH_SIZE);

        //registry.erase<MeshComponent>(entities.front());
        //        auto &&[lock , comp, phy] = registry.cget<MeshComponent, PhysicsComponent>(entities.front());
      }, [timer = std::move(t)](int id) mutable {
        RegistryThroughput::stop();
        timer.stop();
      });

    SC::ChaosThreading::wait_until_finished();

    RegLog::stats<RegistryThroughput>("");
    auto time1 = RegLog::time("Manipulating  of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    auto &&[lock, view] = registry.view<PhysicsComponent>();

    RegisteryManipulate::reset();
    RegisteryManipulate::start();
    const auto &iter = view.each();
    // SC::ChaosThreading::parralelFor(iter, [](auto e, auto &phy) {
    //   for (int i = 0; i < 100; ++i) {
    //     phy.velocity = phy.velocity * 0.99f + glm::vec3(std::sin(i), std::cos(i), 0.0f);
    //   }
    // });

    // RegisteryManipulate::record(view.size());
    // RegisteryManipulate::stop();
    // RegLog::stats<RegisteryManipulate>("Parallel For");
  }
}

template<bool move>
void testMoveCopy() {
  SC::ChaosRegistry registry;
  MoveCopySpy::reset();

  std::vector<MoveCopySpy> spies{BATCH_SIZE};
  std::vector<SC::ChaosEntity> entities{BATCH_SIZE};
  registry.create(entities.begin(), entities.end());

  auto t = RegLog::time("Mode took {}");
  if constexpr (move) {
    registry.insert<MoveCopySpy>(entities.begin(), entities.end(),
                                 std::make_move_iterator(spies.begin()));
  } else {
    registry.insert<MoveCopySpy>(entities.begin(), entities.end(), spies.begin());
  }
  t.stop();

  RegLog::info("Insert: Move {}, Ctor: {}, Copies: {}, Moves: {}",
               move,
               MoveCopySpy::ctorCount.load(),
               MoveCopySpy::copyCount.load(),
               MoveCopySpy::moveCount.load());
}

int main() {
  SC::ChaosThreading::init();
  // run_registry_stress_test_batch();
  // testMoveCopy<false>();
  // testMoveCopy<true>();
  run_registry_stress_test();

  return 0;
}
