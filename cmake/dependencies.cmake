include(FetchContent)


# EnTT für das ECS-Backend
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
