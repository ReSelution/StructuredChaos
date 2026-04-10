//
// Created by oleub on 07.04.26.
//#include <iostream>

#include <span>

#include "ecs/chaos_registry.hpp"
#include "ecs/chaos_resource.hpp"
#include "threading/chaos_threading.hpp"

LOG_ALIAS(RegLog, "Chaos", "Registry");

DEFINE_CHAOS_CORE_STAT(RegistryThroughput, "egistry emplace", SC::ChaosThroughput<SC::MetricUnits>);
DEFINE_CHAOS_CORE_STAT(RegisteryManipulate, "Registry manipulate", SC::ChaosThroughput<SC::MetricUnits>);


// Test-Komponente
struct MeshComponent {
    SC::ChaosResource<std::span<const float>> vertices;
    uint32_t meshId;
};

struct TransformComponent {
    std::array<float, 16> translation;
};

struct PositionComponent {
    std::array<float, 3> pos{0.0f};
};

struct PhysicsComponent {
    std::array<float, 3> min, max;
    std::array<float, 3> velocity;
    float mass;
};

struct TagComponent {
    std::array<char, 32> name;
};

struct AIStateComponent {
    int currentState;
    std::array<float, 3> targetPos;
    float stamina;
};

struct MaterialComponent {
    uint64_t albedoId;
    uint64_t normalId;
    float roughness;
    float metallic;
    uint32_t shaderFlags;
};


void run_registry_stress_test() {
    // Konfiguration
    constexpr int TOTAL_ENTITIES = 1000000;
    constexpr int BATCH_SIZE = 5000; // Tasks pro Enqueue-Welle

    RegLog::info("Starting High-Throughput Test: {} Entitäten...", TOTAL_ENTITIES);
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
                SC::ChaosThreading::Priority::Normal,
                entitiesStart.begin(), entitiesStart.end(), [&registry, BATCH_SIZE](int thread_id, int start) {
                    // 1. Entities erstellen
                    std::vector<SC::ChaosEntity> entities{BATCH_SIZE};
                    registry.create(entities.begin(), entities.end());

                    // 2. Transform (Standard)
                    std::vector<TransformComponent> transforms{BATCH_SIZE, TransformComponent{}};
                    registry.insert<TransformComponent>(entities.begin(), entities.end(), transforms.begin());
                    RegistryThroughput::record(BATCH_SIZE);

                    // 3. Physics (Data heavy)
                    std::vector<PhysicsComponent> physics{BATCH_SIZE, PhysicsComponent{}};
                    registry.insert<PhysicsComponent>(entities.begin(), entities.end(), physics.begin());
                    RegistryThroughput::record(BATCH_SIZE);

                    // 4. Material (IDs & Flags)
                    std::vector<MaterialComponent> materials{BATCH_SIZE, MaterialComponent{}};
                    registry.insert<MaterialComponent>(entities.begin(), entities.end(), materials.begin());
                    RegistryThroughput::record(BATCH_SIZE);

                    // 5. Tags (Strings/Char Arrays)
                    std::vector<TagComponent> tags{BATCH_SIZE, TagComponent{}};
                    registry.insert<TagComponent>(entities.begin(), entities.end(), tags.begin());
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

                    //registry.erase<MeshComponent>(entities.front());
//        auto &&[lock , comp, phy] = registry.cget<MeshComponent, PhysicsComponent>(entities.front());

                }

        );
        SC::ChaosThreading::wait_until_finished();
        RegistryThroughput::stop();
        auto time1 = RegLog::time("Manipulating  of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
        auto &&[lock, view] = registry.view<PhysicsComponent>();
//    for (auto &&[e, phy] : view.each() ){
//      auto &&[x,y,z ] = phy.velocity;
//      x+=0.1f;
//      y -= 0.3f;
//      z += 1.0f;
//    }

        RegisteryManipulate::reset();
        RegisteryManipulate::start();
        const auto &iter = view.each();
        SC::ChaosThreading::parralelFor(iter.begin(), iter.end(), [](auto e, auto &phy) {
            auto &&[x, y, z] = phy.velocity;
            x += 0.1f;
            y -= 0.3f;
            z += 1.0f;
        });
        RegisteryManipulate::record(view.size());
        RegisteryManipulate::stop();
    }

    RegLog::stats<RegistryThroughput, RegisteryManipulate>("");
}

int main() {
    SC::ChaosThreading::init();

    run_registry_stress_test();

    return 0;
}
