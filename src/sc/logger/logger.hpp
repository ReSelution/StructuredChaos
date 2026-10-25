#pragma once

#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <spdlog/logger.h>

#include "sc/stats/timer.hpp"
#include "sc/util/fixed_string.hpp"

namespace sc {

  using LogLevel = spdlog::level::level_enum;

  // Directory the log files are written to, "logs" below the working directory
  // by default. Applies to every module that has not logged yet.
  void setLogDirectory(std::filesystem::path directory);

  [[nodiscard]] std::filesystem::path logDirectory();

  // Makes the state that every logger shares, spdlog's included, if it is
  // not there yet, without a logger or a file. Made on first use otherwise,
  // and a static that is made after an atexit handler is registered is
  // destroyed before the handler runs: a thread that such a handler stops
  // calls this before it registers the handler, so that it may log until it
  // is stopped.
  void initLogState();

  // Flushes and drops every logger of the process. Logging afterwards still
  // works through the loggers that already exist. A logger that is used for
  // the first time afterwards is set up anew and writes to the same file as
  // the other loggers of its module.
  void shutdownLogging();

  namespace internal {
    // The logger "module" or "module:category", created with a console sink of
    // the given level and the file sink of the module, or the one that is
    // registered with spdlog under that name already.
    [[nodiscard]] std::shared_ptr<spdlog::logger> makeLogger(std::string_view module, std::string_view category,
                                                             LogLevel consoleLevel);
  } // namespace internal

  // Logger of a module, optionally with a category:
  //   using NetLog = sc::Logger<"Net">;
  //   using NetIoLog = sc::Logger<"Net", "IO">;
  //
  // Every module has one log file, <directory>/<module>.log, that receives all
  // messages of all its categories. The console only shows messages from the
  // level given to init() upwards, info by default.
  constexpr FixedString NoCat = "";
  template<FixedString M, FixedString C = NoCat>
  class Logger {

  public:
    using LogLevel = sc::LogLevel;

    // Sets the logger up with the given console level. Only the first call has
    // an effect; logging without it uses the default level.
    static void init(LogLevel level = spdlog::level::info);
    // Writes everything logged so far to the file. Happens on its own for
    // warnings and above.
    static void flush() { get()->flush(); }
    // Logging
    template<typename... Args>
    static void log(LogLevel level, spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->log(level, fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void info(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->info(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void trace(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->trace(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void debug(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->debug(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void warn(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->warn(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void err(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->error(fmt, std::forward<Args>(args)...);
    }

    template<typename... Args>
    static void critical(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->critical(fmt, std::forward<Args>(args)...);
    }
    // Stats
    template<typename... StatsTypes, typename... Args>
    static void stats(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      stats<StatsTypes...>(LogLevel::info, fmt, std::forward<Args>(args)...);
    }

    // Logs the message together with the current values of the stats. Without
    // any stat there is nothing to report, and nothing is logged.
    template<typename... StatsTypes, typename... Args>
    static void stats(LogLevel level, spdlog::format_string_t<Args...> fmt, Args &&...args) {
      if constexpr (sizeof...(StatsTypes) != 0) {
        std::vector<std::pair<std::string_view, std::string>> collected;
        collected.reserve(sizeof...(StatsTypes));
        (collected.emplace_back(StatsTypes::name(), StatsTypes::str()), ...);

        const std::string userMessage = spdlog::fmt_lib::format(fmt, std::forward<Args>(args)...);
        logStats(level, userMessage, collected);
      }
    }

    // Timer
    template<typename... Args>
    [[nodiscard]] static auto time(std::string_view fmt, Args &&...args) {
      return time(spdlog::level::info, fmt, std::forward<Args>(args)...);
    }

    // The timer logs when it is destroyed, which is after this call returned.
    // The first placeholder receives the duration. Arguments passed as lvalues
    // are kept as references and read only then, so the line shows the values
    // they have at the end of the measurement; they have to outlive the timer.
    // Temporaries and the format string are copied.
    //
    // The format string may be built at run time, so it is only checked then.
    // A broken one logs an error instead of the line.
    template<typename... Args>
    [[nodiscard]] static auto time(LogLevel level, std::string_view fmt, Args &&...args) {
      return sc::stats::Timer([level, format = std::string(fmt),
                               kept = std::tuple<Args...>(std::forward<Args>(args)...)](sc::stats::TimeResult res) {
        const std::string duration = spdlog::fmt_lib::format("{:.2f}{}", res.value, res.suffix);
        try {
          std::apply(
              [&](const auto &...values) { get()->log(level, spdlog::fmt_lib::runtime(format), duration, values...); },
              kept);
        } catch (const std::exception &e) {
          get()->error("Cannot format the timer line \"{}\": {}", format, e.what());
        }
      });
    }

  private:
    static spdlog::logger *get();

    // s_owner keeps the logger alive, also after shutdownLogging() dropped it
    // from spdlog's registry. s_logger is the same logger for the fast path,
    // which only needs an atomic load instead of a shared_ptr copy.
    static inline std::shared_ptr<spdlog::logger> s_owner = nullptr;
    static inline std::atomic<spdlog::logger *> s_logger{nullptr};
    static inline std::mutex s_initMutex;

    static void logStats(LogLevel level, std::string_view userMessage,
                         const std::vector<std::pair<std::string_view, std::string>> &collected) {
      std::string statsMessage;
      for (const auto &[name, value]: collected) {
        if (!statsMessage.empty()) {
          statsMessage += " | ";
        }
        statsMessage += name;
        statsMessage += ": ";
        statsMessage += value;
      }

      const bool hasUser = !userMessage.empty();
      const bool hasStats = !statsMessage.empty();
      if (hasUser && hasStats) {
        get()->log(level, "{} -> [{}]", userMessage, statsMessage);
      } else if (hasStats) {
        get()->log(level, "[{}]", statsMessage);
      } else if (hasUser) {
        get()->log(level, "{}", userMessage);
      }
    }
  };

  template<FixedString M, FixedString C>
  spdlog::logger *Logger<M, C>::get() {
    auto *ptr = s_logger.load(std::memory_order_acquire);
    if (ptr == nullptr) [[unlikely]] {
      init();
      ptr = s_logger.load(std::memory_order_acquire);
    }
    return ptr;
  }

  template<FixedString M, FixedString C>
  void Logger<M, C>::init(LogLevel level) {
    if (s_logger.load(std::memory_order_acquire) != nullptr) {
      return;
    }

    std::scoped_lock lock(s_initMutex);
    if (s_owner) {
      return;
    }
    s_owner = internal::makeLogger(M.text(), C.text(), level);
    s_logger.store(s_owner.get(), std::memory_order_release);
  }

} // namespace sc
