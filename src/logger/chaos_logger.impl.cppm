module;

#include "spdlog/common.h"
#include "spdlog/logger.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module SC.Logger:Impl;
import SC.Util;
namespace SC {

export constexpr FixedString NoCat = "";
export using LogLevel = spdlog::level::level_enum;

export template <FixedString M, FixedString C = NoCat> class ChaosLogger {
public:
  static void init(LogLevel level = spdlog::level::info);
  static void shutdown() { spdlog::shutdown(); }
  // Logging
  template <typename... Args>
  static void log(LogLevel level, spdlog::format_string_t<Args...> fmt,
                  Args &&...args) {
    get()->log(level, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void info(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->info(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void trace(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->trace(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void debug(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->debug(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void warn(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->warn(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void err(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->error(fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void critical(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    get()->critical(fmt, std::forward<Args>(args)...);
  }
  // Stats
  template <typename... StatsTypes, typename... Args>
  static void stats(spdlog::format_string_t<Args...> fmt, Args &&...args) {
    stats<StatsTypes...>(LogLevel::info, fmt, std::forward<Args>(args)...);
  }

  template <typename... StatsTypes, typename... Args>
  static void stats(LogLevel level, spdlog::format_string_t<Args...> fmt,
                    Args &&...args) {
    if constexpr (sizeof...(StatsTypes) == 0)
      return;

    std::vector<std::pair<std::string_view, std::string>> collected;
    collected.reserve(sizeof...(StatsTypes));

    (collected.emplace_back(StatsTypes::name(), StatsTypes::str()), ...);

    std::string user_msg = fmt::format(fmt, std::forward<Args>(args)...);

    log_stats_impl(level, user_msg, collected);
  }

private:
  static spdlog::logger *get();
  static inline std::shared_ptr<spdlog::logger> logger = nullptr;
  static inline std::atomic<spdlog::logger *> rawLogger{nullptr};
  static inline std::mutex initMutex;

  static void
  log_stats_impl(LogLevel level, std::string_view user_msg,
                 const std::vector<std::pair<std::string_view, std::string>>
                     &collected_stats) {
    if (collected_stats.empty() && user_msg.empty())
      return;

    std::string stats_msg;
    if (!collected_stats.empty()) {
      stats_msg.reserve(collected_stats.size() * 64);
      bool first = true;
      for (const auto &[name, value_str] : collected_stats) {
        if (!first)
          stats_msg += " | ";
        stats_msg += std::format("{}: {}", name, value_str);
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

template <FixedString M, FixedString C>
spdlog::logger *ChaosLogger<M, C>::get() {
  auto *ptr = rawLogger.load(std::memory_order_acquire);
  if (!ptr) [[unlikely]] {
    init();
    ptr = rawLogger.load(std::memory_order_acquire);
  }
  return ptr;
}

template <FixedString M, FixedString C>
void ChaosLogger<M, C>::init(spdlog::level::level_enum level) {
  if (rawLogger.load(std::memory_order_acquire))
    return;

  std::lock_guard<std::mutex> lock(initMutex);
  if (logger)
    return;

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

  std::error_code ec;
  std::filesystem::create_directories("logs", ec);

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

  std::string filePath;
  filePath.reserve(5 + M.text().size() + 4);
  filePath += "logs/";
  filePath += M.text();
  filePath += ".log";

  auto fileSink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(filePath, true);
  fileSink->set_level(spdlog::level::trace);

  std::string filePattern;
  filePattern.reserve(20 + (hasCat ? C.text().size() : 0));
  filePattern += "[%T] ";
  if constexpr (hasCat) {
    filePattern += "[";
    filePattern += C.text();
    filePattern += "] ";
  }
  filePattern += "[%l]: %v";
  fileSink->set_pattern(filePattern);

  std::array<spdlog::sink_ptr, 2> sinks{consoleSink, fileSink};
  logger =
      std::make_shared<spdlog::logger>(logerName, sinks.begin(), sinks.end());

  spdlog::register_logger(logger);
  rawLogger.store(logger.get(), std::memory_order_release);
  logger->flush_on(spdlog::level::warn);
}

} // namespace SC
