include(FetchContent)

FetchContent_Declare(
        EnTT
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG        v3.16.0
)
FetchContent_MakeAvailable(EnTT)

FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG       v3.2.8
)
FetchContent_MakeAvailable(mimalloc)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.17.0
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
        pfr
        GIT_REPOSITORY https://github.com/apolukhin/pfr_non_boost.git
        GIT_TAG        2.3.2
)
FetchContent_MakeAvailable(pfr)

FetchContent_Declare(
        atomic_queue
        GIT_REPOSITORY https://github.com/max0x7ba/atomic_queue.git
        GIT_TAG        v1.7.3
)
FetchContent_MakeAvailable(atomic_queue)

FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.3
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
        hwy
        GIT_REPOSITORY https://github.com/google/highway.git
        GIT_TAG        1.3.0
)
FetchContent_MakeAvailable(hwy)

FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG       v0.13.1
)
FetchContent_MakeAvailable(tracy)
