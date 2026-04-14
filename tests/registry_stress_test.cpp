//
// Created by oleub on 07.04.26.
//#include <iostream>

#include <span>

#include "ecs/chaos_registry.hpp"
#include "ecs/chaos_resource.hpp"
#include "threading/chaos_threading.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

LOG_ALIAS(RegLog, "Chaos", "Registry");

DEFINE_CHAOS_CORE_STAT(RegistryThroughput, "Registry emplace", SC::ChaosThroughput<SC::MetricUnits>);

DEFINE_CHAOS_CORE_STAT(RegisteryManipulate, "Registry manipulate", SC::ChaosThroughput<SC::MetricUnits>);

struct MeshComponent {
  SC::ChaosResource<std::span<const float>> vertices;
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


void run_registry_stress_test() {
  constexpr int TOTAL_ENTITIES = 1000000;
  constexpr int BATCH_SIZE_SIZE = 5000; // Tasks pro Enqueue-Welle
  RegLog::info("Starting High-Throughput Test: {} Entitäten...", TOTAL_ENTITIES);
  {
    auto t = RegLog::time("Registry-Processing of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    SC::ChaosRegistry registry;
    RegistryThroughput::reset();


    std::vector<int> entitiesStart;
    entitiesStart.reserve(TOTAL_ENTITIES / BATCH_SIZE_SIZE + 1);
    for (int i = 0; i < TOTAL_ENTITIES; i += BATCH_SIZE_SIZE) {
      entitiesStart.emplace_back(i);
    }
    SC::ChaosThreading::enqueueBatch(
        std::move(entitiesStart), [&registry](int thread_id, int start) {
          for (int i = 0; i < BATCH_SIZE_SIZE; i++) {
            float rawData[200];
            auto e = registry.create();
            e.add<TransformComponent>();
            e.add<PhysicsComponent>();
            e.add<MaterialComponent>();
            e.add<TagComponent>();
            e.add<MeshComponent>(std::span<const float>(rawData, 200),
                                 static_cast<uint32_t>(start + i));
          }
          RegistryThroughput::record(BATCH_SIZE_SIZE * 5);

        }

    );
    SC::ChaosThreading::wait_until_finished();
    RegistryThroughput::stop();
    auto time1 = RegLog::time("Manipulating  of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    auto &&[lock, view] = registry.view<PhysicsComponent>();


    RegisteryManipulate::reset();
    RegisteryManipulate::start();
    const auto &iter = view.each();
    SC::ChaosThreading::parralelFor(iter, [](auto e, auto &phy) {
      for (int i = 0; i < 100; ++i) {
        phy.velocity = phy.velocity * 0.99f + glm::vec3(std::sin(i), std::cos(i), 0.0f);
      }
    });
    RegisteryManipulate::record(view.size());
    RegisteryManipulate::stop();
  }

  RegLog::stats<RegistryThroughput, RegisteryManipulate>("");
}

void run_registry_stress_testBATCH_SIZE() {
  // Konfiguration
  constexpr size_t TOTAL_ENTITIES = 1000000;
  constexpr size_t BATCH_SIZE = 5000; // Tasks pro Enqueue-Welle

  RegLog::info("Starting High-Throughput Test: {} Entitäten... (Move Vector)", TOTAL_ENTITIES);
  {
    auto t = RegLog::time("Registry-Processing of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    SC::ChaosRegistry registry;
    RegistryThroughput::reset();


    std::vector<int> entitiesStart;
    entitiesStart.reserve(TOTAL_ENTITIES / BATCH_SIZE + 1);
    for (int i = 0; i < TOTAL_ENTITIES; i += BATCH_SIZE) {
      entitiesStart.emplace_back(i);
    }
    SC::ChaosThreading::enqueueBatch(
        std::move(entitiesStart), [reg = &registry, BATCH_SIZE](int thread_id, int start) {

          // 1. Entities erstellen
          std::vector<SC::ChaosEntity> entities{BATCH_SIZE};
          reg->create(entities.begin(), entities.end());

          // 2. Transform (Standard)
          std::vector<TransformComponent> transforms{BATCH_SIZE, TransformComponent{}};
          reg->template insert<TransformComponent>(entities.begin(), entities.end(), transforms.begin());
          RegistryThroughput::record(BATCH_SIZE);

          // 3. Physics (Data heavy)
          std::vector<PhysicsComponent> physics{BATCH_SIZE, PhysicsComponent{}};
          reg->template insert<PhysicsComponent>(entities.begin(), entities.end(), physics.begin());
          RegistryThroughput::record(BATCH_SIZE);

          // 4. Material (IDs & Flags)
          std::vector<MaterialComponent> materials{BATCH_SIZE, MaterialComponent{}};
          reg->template insert<MaterialComponent>(entities.begin(), entities.end(), materials.begin());
          RegistryThroughput::record(BATCH_SIZE);

          // 5. Tags (Strings/Char Arrays)
          std::vector<TagComponent> tags{BATCH_SIZE, TagComponent{}};
          reg->template insert<TagComponent>(entities.begin(), entities.end(), tags.begin());
          RegistryThroughput::record(BATCH_SIZE);

          // 6. Mesh (Die fette Payload mit ChaosResource)
          std::vector<MeshComponent> meshComponents;
          meshComponents.reserve(BATCH_SIZE);
          reg->template setStorageAnchor<MeshComponent>();
          for (int j = 0; j < BATCH_SIZE; ++j) {
            float rawData[200]; // Simulierter Vertex-Buffer
            meshComponents.emplace_back(std::span<const float>(rawData, 200),
                                        static_cast<uint32_t>(start + j));
          }
          reg->template insert<MeshComponent>(entities.begin(), entities.end(),
                                              std::make_move_iterator(meshComponents.begin()));
          RegistryThroughput::record(BATCH_SIZE);

          //registry.erase<MeshComponent>(entities.front());
//        auto &&[lock , comp, phy] = registry.cget<MeshComponent, PhysicsComponent>(entities.front());

        });


    SC::ChaosThreading::wait_until_finished();
    RegistryThroughput::stop();
    auto time1 = RegLog::time("Manipulating  of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
    auto &&[lock, view] = registry.view<PhysicsComponent>();

    RegisteryManipulate::reset();
    RegisteryManipulate::start();
    const auto &iter = view.each();
    SC::ChaosThreading::parralelFor(iter, [](auto e, auto &phy) {
      for (int i = 0; i < 100; ++i) {
        phy.velocity = phy.velocity * 0.99f + glm::vec3(std::sin(i), std::cos(i), 0.0f);
      }
    });

    RegisteryManipulate::record(view.size());
    RegisteryManipulate::stop();
    RegLog::stats<RegisteryManipulate>("Parallel For");
//    RegisteryManipulate::reset();
//    RegisteryManipulate::start();
//
//    for (const auto &[e, phy]: iter) {
//      for (int i = 0; i < 100; ++i) {
//        phy.velocity = phy.velocity * 0.99f + glm::vec3(std::sin(i), std::cos(i), 0.0f);
//      }
//    }
//    RegisteryManipulate::record(view.size());
//    RegisteryManipulate::stop();
//    RegLog::stats<RegisteryManipulate>("Single Threaded");
  }

  RegLog::stats<RegistryThroughput>("");
}

int main() {
  SC::ChaosThreading::init();
  run_registry_stress_testBATCH_SIZE();
  run_registry_stress_testBATCH_SIZE();
  run_registry_stress_test();

  return 0;
}
