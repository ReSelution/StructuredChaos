#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "sc/stats/timer.hpp"
#include "sc/util/fixed_string.hpp"

namespace sc {

  namespace internal {

    struct LogState {
      std::mutex mutex;
      std::filesystem::path directory{"logs"};
      // File sink per module, null where the file could not be opened.
      std::unordered_map<std::string, spdlog::sink_ptr> fileSinks;
    };

    inline LogState &logState() {
      static LogState state;
      return state;
    }

    // Pattern flag that prints "[category] " for a logger named
    // "module:category" and nothing for a logger without category. It lets all
    // loggers of a module write to one file sink with one pattern.
    class LogCategoryFlag final : public spdlog::custom_flag_formatter {
    public:
      void format(const spdlog::details::log_msg &msg, const std::tm &, spdlog::memory_buf_t &dest) override {
        const std::string_view name{msg.logger_name.data(), msg.logger_name.size()};
        const size_t separator = name.find(':');
        if (separator == std::string_view::npos) {
          return;
        }
        const std::string_view category = name.substr(separator + 1);
        dest.push_back('[');
        dest.append(category.data(), category.data() + category.size());
        dest.push_back(']');
        dest.push_back(' ');
      }

      [[nodiscard]] std::unique_ptr<spdlog::custom_flag_formatter> clone() const override {
        return std::make_unique<LogCategoryFlag>();
      }
    };

    // The file sink of a module, shared by all of its loggers: two sinks on the
    // same file would each truncate it and write over one another. Returns null
    // if the file cannot be opened; the module then only logs to the console.
    inline spdlog::sink_ptr logFileSink(const std::string &module) {
      LogState &state = logState();
      std::scoped_lock lock(state.mutex);

      if (const auto it = state.fileSinks.find(module); it != state.fileSinks.end()) {
        return it->second;
      }

      spdlog::sink_ptr sink;
      try {
        std::error_code ec;
        std::filesystem::create_directories(state.directory, ec);

        auto fileSink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>((state.directory / (module + ".log")).string(), true);
        fileSink->set_level(spdlog::level::trace);

        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<LogCategoryFlag>('*').set_pattern("[%T] %*[%l]: %v");
        fileSink->set_formatter(std::move(formatter));

        sink = std::move(fileSink);
      } catch (const spdlog::spdlog_ex &) {
        // Logging must not take the program down because a directory is missing
        // or read-only.
      }

      state.fileSinks.emplace(module, sink);
      return sink;
    }

  } // namespace internal

  // Directory the log files are written to, "logs" below the working directory
  // by default. Applies to every module that has not logged yet.
  inline void setLogDirectory(std::filesystem::path directory) {
    internal::LogState &state = internal::logState();
    std::scoped_lock lock(state.mutex);
    state.directory = std::move(directory);
  }

  inline std::filesystem::path logDirectory() {
    internal::LogState &state = internal::logState();
    std::scoped_lock lock(state.mutex);
    return state.directory;
  }

  // Logger of a module, optionally with a category:
  //   using NetLog = sc::Logger<"Net">;
  //   using NetIoLog = sc::Logger<"Net", "IO">;
  //
  // Every module has one log file, <directory>/<module>.log, that receives all
  // messages of all its categories. The console only shows messages from the
  // level given to init() upwards, info by default.
  constexpr FixedString NoCat = "";
  template <FixedString M, FixedString C = NoCat> class Logger {

  public:
    using LogLevel = spdlog::level::level_enum;

    // Sets the logger up with the given console level. Only the first call has
    // an effect; logging without it uses the default level.
    static void init(LogLevel level = spdlog::level::info);
    static void shutdown() { spdlog::shutdown(); }
    // Writes everything logged so far to the file. Happens on its own for
    // warnings and above.
    static void flush() { get()->flush(); }
    // Logging
    template <typename... Args> static void log(LogLevel level, spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->log(level, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void info(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->info(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void trace(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->trace(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void debug(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->debug(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void warn(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->warn(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void err(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->error(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args> static void critical(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      get()->critical(fmt, std::forward<Args>(args)...);
    }
    // Stats
    template <typename... StatsTypes, typename... Args>
    static void stats(spdlog::format_string_t<Args...> fmt, Args &&...args) {
      stats<StatsTypes...>(LogLevel::info, fmt, std::forward<Args>(args)...);
    }

    template <typename... StatsTypes, typename... Args>
    static void stats(LogLevel level, spdlog::format_string_t<Args...> fmt, Args &&...args) {
      if constexpr (sizeof...(StatsTypes) == 0) {
        return;
      }

      std::vector<std::pair<std::string_view, std::string>> collected;
      collected.reserve(sizeof...(StatsTypes));

      (collected.emplace_back(StatsTypes::name(), StatsTypes::str()), ...);

      std::string user_msg = spdlog::fmt_lib::format(fmt, std::forward<Args>(args)...);

      log_stats_impl(level, user_msg, collected);
    }

    // Timer
    template <typename... Args> [[nodiscard]] static auto time(std::string_view fmt_str, Args &&...args) {
      return time(spdlog::level::info, fmt_str, std::forward<Args>(args)...);
    }

    // The timer logs when it is destroyed, which is after this call returned.
    // It therefore keeps copies of the format string and the arguments.
    template <typename... Args>
    [[nodiscard]] static auto time(spdlog::level::level_enum level, std::string_view fmt_str, Args &&...args) {
      return sc::stats::Timer([level, format = std::string(fmt_str),
                               kept = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)](
                                  sc::stats::TimeResult res) mutable {
        std::string timeStr = spdlog::fmt_lib::format("{:.2f}{}", res.value, res.suffix);
        std::apply([&](auto &...values) { get()->log(level, spdlog::fmt_lib::runtime(format), timeStr, values...); },
                   kept);
      });
    }

  private:
    static spdlog::logger *get();
    static inline std::shared_ptr<spdlog::logger> logger = nullptr;
    static inline std::atomic<spdlog::logger *> rawLogger{nullptr};
    static inline std::mutex initMutex;

    static void log_stats_impl(LogLevel level, std::string_view user_msg,
                               const std::vector<std::pair<std::string_view, std::string>> &collected_stats) {
      if (collected_stats.empty() && user_msg.empty()) {
        return;
      }

      std::string stats_msg;
      if (!collected_stats.empty()) {
        stats_msg.reserve(collected_stats.size() * 64);
        bool first = true;
        for (const auto &[name, value_str] : collected_stats) {
          if (!first) {
            stats_msg += " | ";
          }
          stats_msg += name;
          stats_msg += ": ";
          stats_msg += value_str;
          first = false;
        }
      }

      bool has_user = !user_msg.empty();
      bool has_stats = !stats_msg.empty();

      if (has_user && has_stats) {
        get()->log(level, "{} -> [{}]", user_msg, stats_msg);
      } else if (has_stats) {
        get()->log(level, "[{}]", stats_msg);
      } else if (has_user) {
        get()->log(level, "{}", user_msg);
      }
    }
  };

  template <FixedString M, FixedString C> spdlog::logger *Logger<M, C>::get() {
    auto *ptr = rawLogger.load(std::memory_order_acquire);
    if (ptr == nullptr) [[unlikely]] {
      init();
      ptr = rawLogger.load(std::memory_order_acquire);
    }
    return ptr;
  }

  template <FixedString M, FixedString C> void Logger<M, C>::init(spdlog::level::level_enum level) {
    if (rawLogger.load(std::memory_order_acquire) != nullptr) {
      return;
    }

    std::scoped_lock lock(initMutex);
    if (logger) {
      return;
    }

    constexpr bool hasCat = (C.text() != NoCat.text());

    std::string logerName;
    size_t nameReserve = M.text().size();
    if constexpr (hasCat) {
      nameReserve += C.text().size() + 1;
    }

    logerName.reserve(nameReserve);
    logerName += M.text();
    if constexpr (hasCat) {
      logerName += ":";
      logerName += C.text();
    }

    auto spdLogger = spdlog::get(logerName);
    if (spdLogger) {
      logger = spdLogger;
      rawLogger.store(logger.get(), std::memory_order_release);
      return;
    }

    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_level(level);

    std::string conPattern;
    size_t conReserve = M.text().size() + 16;
    if constexpr (hasCat) {
      conReserve += C.text().size() + 3;
    }

    conPattern.reserve(conReserve);
    conPattern += "[";
    conPattern += M.text();
    conPattern += "]";
    if constexpr (hasCat) {
      conPattern += " [";
      conPattern += C.text();
      conPattern += "]";
    }
    conPattern += ": %v%$";
    consoleSink->set_pattern(conPattern);

    std::vector<spdlog::sink_ptr> sinks{consoleSink};
    if (auto fileSink = internal::logFileSink(std::string(M.text()))) {
      sinks.push_back(std::move(fileSink));
    }

    logger = std::make_shared<spdlog::logger>(logerName, sinks.begin(), sinks.end());
    // The sinks decide what they show. The logger itself has to let everything
    // through, or the file would never see debug and trace messages.
    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(logger);
    rawLogger.store(logger.get(), std::memory_order_release);
  }

} // namespace sc
