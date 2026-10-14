#include "logger.hpp"

#include <system_error>
#include <unordered_map>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace sc {
  namespace {

    struct LogState {
      std::mutex mutex;
      std::filesystem::path directory{"logs"};
      // File sink per module, null where the file could not be opened.
      std::unordered_map<std::string, spdlog::sink_ptr> fileSinks;
    };

    LogState &logState() {
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
    spdlog::sink_ptr logFileSink(const std::string &module) {
      LogState &state = logState();
      std::scoped_lock lock(state.mutex);

      if (const auto it = state.fileSinks.find(module); it != state.fileSinks.end()) {
        return it->second;
      }

      spdlog::sink_ptr sink;
      try {
        // A failure shows up when the file is opened below.
        std::error_code ec;
        std::filesystem::create_directories(state.directory, ec);

        auto fileSink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>((state.directory / (module + ".log")).string(), true);
        fileSink->set_level(spdlog::level::trace);

        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<LogCategoryFlag>('*').set_pattern("[%T] %*[%l]: %v");
        fileSink->set_formatter(std::move(formatter));

        sink = std::move(fileSink);
      } catch (const spdlog::spdlog_ex &) { // NOLINT(bugprone-empty-catch)
        // Logging must not take the program down because a directory is missing
        // or read-only.
      }

      state.fileSinks.emplace(module, sink);
      return sink;
    }

  } // namespace

  void setLogDirectory(std::filesystem::path directory) {
    LogState &state = logState();
    std::scoped_lock lock(state.mutex);
    state.directory = std::move(directory);
  }

  std::filesystem::path logDirectory() {
    LogState &state = logState();
    std::scoped_lock lock(state.mutex);
    return state.directory;
  }

  void shutdownLogging() { spdlog::shutdown(); }

  namespace internal {
    std::shared_ptr<spdlog::logger> makeLogger(std::string_view module, std::string_view category,
                                               LogLevel consoleLevel) {
      std::string name(module);
      if (!category.empty()) {
        name += ':';
        name += category;
      }

      if (auto existing = spdlog::get(name)) {
        return existing;
      }

      // The console sinks of spdlog share one global mutex, so lines of
      // different loggers do not run into each other even with a sink each.
      auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
      consoleSink->set_level(consoleLevel);

      std::string pattern = "[";
      pattern += module;
      pattern += "]";
      if (!category.empty()) {
        pattern += " [";
        pattern += category;
        pattern += "]";
      }
      // The message in the color of its level.
      pattern += ": %^%v%$";
      consoleSink->set_pattern(pattern);

      std::vector<spdlog::sink_ptr> sinks{consoleSink};
      if (auto fileSink = logFileSink(std::string(module))) {
        sinks.push_back(std::move(fileSink));
      }

      auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
      // The sinks decide what they show. The logger itself has to let everything
      // through, or the file would never see debug and trace messages.
      logger->set_level(spdlog::level::trace);
      logger->flush_on(spdlog::level::warn);

      spdlog::register_logger(logger);
      return logger;
    }
  } // namespace internal

} // namespace sc
