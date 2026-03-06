#pragma once

#include <filesystem>
#include <spdlog/spdlog.h>
#include <string_view>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../stats/chaos_timer.hpp"
#include "spdlog/sinks/basic_file_sink.h"

namespace SC {
  struct NoCat {
    static constexpr std::string_view name() { return ""; }
  };

  template<typename Module, typename Cat = NoCat>
  class ChaosLogger {
  public:
    static void init(spdlog::level::level_enum level = spdlog::level::info);


    template<typename... StatsTypes, typename... Args>
    static void stats(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      stats<StatsTypes...>(spdlog::level::info, fmt, std::forward<Args>(args)...);
    }

    template<typename... StatsTypes, typename... Args>
    static void stats(spdlog::level::level_enum level, spdlog::format_string_t<Args...> fmt, Args &&... args) {
      if constexpr (sizeof...(StatsTypes) == 0) return;
      std::string stats_msg;
      stats_msg.reserve(sizeof...(StatsTypes) * 64);

      bool first = true;
      auto append_stat = [&](auto name, auto value_str) {
        if (!first) {
          stats_msg += " | ";
        }
        stats_msg += std::format("{}: {}", name, value_str);
        first = false;
      };

      (append_stat(StatsTypes::name(), StatsTypes::str()), ...);
      std::string user_msg = fmt::format(fmt, std::forward<Args>(args)...);
      if (!stats_msg.empty()) {
        get()->log(level, "{} -> [{}]", user_msg, stats_msg);
      } else {
        get()->log(level, "{}", user_msg);
      }
    }

    template<typename... Args>
    [[nodiscard]] static auto time(std::string_view fmt_str, Args &&... args) {
      return time(spdlog::level::info, fmt_str, std::forward<Args>(args)...);
    }


    template<typename... Args>
    [[nodiscard]] static auto time(spdlog::level::level_enum level, std::string_view fmt_str, Args &&... args) {
      return make_timer([level,fmt_str, ...captured_args = std::forward<Args>(args)](TimeResult res) mutable {
        std::string timeStr = fmt::format("{:.2f}{}", res.value, res.suffix);
        get()->log(level, fmt::runtime(fmt_str), timeStr, res.value);
      });
    }

    template<typename... Args>
    static void info(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->info(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void trace(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->trace(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->debug(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->warn(fmt, std::forward<Args>(args)...);
    }


    template<typename... Args>
    static void err(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->error(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void critical(spdlog::format_string_t<Args...> fmt, Args &&... args) {
      get()->critical(fmt, std::forward<Args>(args)...);
    }

  private:
    static spdlog::logger *get() {
      spdlog::logger *ptr = rawLogger.load(std::memory_order_acquire);
      if (!ptr)[[unlikely]] {
        init();
        ptr = rawLogger.load(std::memory_order_acquire);
      }
      return ptr;
    }

    static inline std::shared_ptr<spdlog::logger> logger = nullptr;
    static inline std::atomic<spdlog::logger *> rawLogger{nullptr};
    static inline std::mutex initMutex;
  };

  template<typename Module, typename Cat>
  void ChaosLogger<Module, Cat>::init(spdlog::level::level_enum level) {
    if (rawLogger.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(initMutex);
    if (logger) return;

    constexpr bool hasCat = !std::is_same_v<Cat, NoCat>;

    std::string logerName;
    size_t nameReserve = Module::name().size();
    if constexpr (hasCat) nameReserve += Cat::name().size() + 1;


    logerName.reserve(nameReserve);
    logerName += Module::name();
    if constexpr (hasCat) {
      logerName += ":";
      logerName += Cat::name();
    }
    auto spdLogger = spdlog::get(logerName);
    if (spdLogger) {
      logger = spdLogger;
      rawLogger.store(logger.get(), std::memory_order_release);
      return;
    }

    std::error_code ec;
    std::filesystem::create_directories("logs", ec);

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(level);
    std::string conPattern;
    size_t conReserve = Module::name().size() + 16;
    if constexpr (hasCat) conReserve += Cat::name().size() + 3;

    conPattern.reserve(conReserve);
    conPattern += "[";
    conPattern += Module::name();
    conPattern += "]";
    if constexpr (hasCat) {
      conPattern += " [";
      conPattern += Cat::name();
      conPattern += "]";
    }
    conPattern += ": %v%$";
    consoleSink->set_pattern(conPattern);

    std::string filePath;
    filePath.reserve(5 + Module::name().size() + 4); // "logs/" + name + ".log"
    filePath += "logs/";
    filePath += Module::name();
    filePath += ".log";

    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filePath, true);
    fileSink->set_level(spdlog::level::trace);

    std::string filePattern;
    filePattern.reserve(20 + (hasCat ? Cat::name().size() : 0));
    filePattern += "[%T] ";
    if constexpr (hasCat) {
      filePattern += "[";
      filePattern += Cat::name();
      filePattern += "] ";
    }
    filePattern += "[%l]: %v";
    fileSink->set_pattern(filePattern);

    std::array<spdlog::sink_ptr, 2> sinks{consoleSink, fileSink};
    logger = std::make_shared<spdlog::logger>(logerName, sinks.begin(), sinks.end());
    spdlog::register_logger(logger);
    rawLogger.store(logger.get(), std::memory_order_release);
    logger->flush_on(spdlog::level::warn);
  }


  // Helfer zum Zählen der Argumente (VA_ARGS Trick)
#define _GET_LOG_MACRO(_1, _2, _3, NAME, ...) NAME

  // Variante 1: AliasName + ModulString
#define _LOG_ALIAS_2(AliasName, ModStr) \
struct _Mod_##AliasName { static constexpr std::string_view name() { return ModStr; } }; \
using AliasName = SC::ChaosLogger<_Mod_##AliasName>; \
namespace { inline static bool _init_##AliasName = (AliasName::init(), true); }

  // Variante 2: AliasName + ModulString + CatString
#define _LOG_ALIAS_3(AliasName, ModStr, CatStr) \
struct _Mod_##AliasName { static constexpr std::string_view name() { return ModStr; } }; \
struct _Cat_##AliasName { static constexpr std::string_view name() { return CatStr; } }; \
using AliasName = SC::ChaosLogger<_Mod_##AliasName, _Cat_##AliasName>; \
namespace { inline static bool _init_##AliasName = (AliasName::init(), true); }

  // Das Haupt-Makro, das entscheidet
#define LOG_ALIAS(...) _GET_LOG_MACRO(__VA_ARGS__, _LOG_ALIAS_3, _LOG_ALIAS_2)(__VA_ARGS__)
} // namespace SC
