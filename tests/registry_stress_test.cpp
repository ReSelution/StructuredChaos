//
// Created by oleub on 07.04.26.
//#include <iostream>

#include <span>

#include "ecs/chaos_registry.hpp"
#include "ecs/chaos_resource.hpp"
#include "threading/chaos_threading.hpp"

LOG_ALIAS(RegLog, "Chaos", "Registery");

DEFINE_CHAOS_CORE_STAT(RegistryThroughput, "Registery", SC::ChaosThroughput<SC::MetricUnits>);


// Test-Komponente
struct MeshComponent {
    SC::ChaosResource<std::span<const float>> vertices;
    uint32_t meshId;
};

struct TransformComponent {
    float matrix[16]{0.0f};
};

void run_registry_stress_test() {


    // Konfiguration
    constexpr  int TOTAL_ENTITIES = 500000;
    constexpr  int BATCH_SIZE = 5000; // Tasks pro Enqueue-Welle

    RegLog::info("Starting High-Throughput Test: {} Entitäten...", TOTAL_ENTITIES);
    {
        auto t = RegLog::time("Registry-Processing of {1} Entitäten dauerte {0}", TOTAL_ENTITIES);
        SC::ChaosRegistry registry;
        RegistryThroughput::reset();


        for (int i = 0; i < TOTAL_ENTITIES; i += BATCH_SIZE) {
            SC::ChaosThreading::enqueue([&registry, i, BATCH_SIZE](int thread_id) {
                std::vector<SC::ChaosEntity>entities {BATCH_SIZE};
                registry.create(entities.begin(), entities.end());
                std::vector<TransformComponent>transformComponents {BATCH_SIZE};
                registry.insert<TransformComponent>(entities.begin(), entities.end(), transformComponents.begin());
                RegistryThroughput::record(BATCH_SIZE);


                std::vector<MeshComponent>meshComponents;
                meshComponents.reserve(BATCH_SIZE);
                registry.setStorageAnchor<MeshComponent>();
                for (int j = 0; j < BATCH_SIZE; ++j) {
                    float rawData[200];
                    meshComponents.emplace_back(  std::span<const float>(rawData, 200),
                        static_cast<uint32_t>(i + j));

                }
                registry.insert<MeshComponent>(entities.begin(), entities.end(), std::make_move_iterator(meshComponents.begin()));
                RegistryThroughput::record(BATCH_SIZE);
            });
        }

        // Warten, bis alle Worker fertig sind
        SC::ChaosThreading::wait_until_finished();

    }

    RegLog::stats<RegistryThroughput>("");
}

int main() {
    // Initialisierung deines Systems
    SC::ChaosThreading::init();

    run_registry_stress_test();

    return 0;
}