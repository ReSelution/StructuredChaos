#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "sc/io/file_reader.hpp"
#include "sc/threading/threading.hpp"

namespace {

  using sc::io::FileReader;
  using sc::io::FileResult;

  // A directory of its own under the temp directory, removed at the end.
  struct TempDir {
    std::filesystem::path path;

    TempDir() :
        path(std::filesystem::temp_directory_path() /
             ("sc_file_reader_" + std::to_string(reinterpret_cast<uintptr_t>(this)))) {
      std::filesystem::create_directories(path);
    }

    ~TempDir() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    // A file of the given size whose bytes say where they are.
    [[nodiscard]] std::string write(const std::string &name, size_t size) const {
      const std::filesystem::path file = path / name;
      std::ofstream stream(file, std::ios::binary);
      for (size_t i = 0; i < size; ++i) {
        stream.put(static_cast<char>(byteAt(i)));
      }
      return file.string();
    }

    static unsigned char byteAt(size_t i) { return static_cast<unsigned char>((i * 131) ^ (i >> 8)); }
  };

  bool sameAsWritten(const FileResult &result, size_t size) {
    if (!result || result->size != size) {
      return false;
    }
    for (size_t i = 0; i < size; ++i) {
      if (result->data[i] != std::byte{TempDir::byteAt(i)}) {
        return false;
      }
    }
    return true;
  }

  // Waits until every read has been given to its done.
  void waitForAll(FileReader &reader) {
    while (reader.pending() != 0) {
      (void) reader.wait();
    }
  }

} // namespace

TEST_CASE("IO File Reader", "[io]") {
  const TempDir dir;
  FileReader reader;

  SECTION("a file, whole, and done only in poll or wait") {
    const std::string path = dir.write("small.bin", 1000);
    FileResult result;
    bool called = false;
    reader.read(path, [&](FileResult file) {
      result = std::move(file);
      called = true;
    });
    CHECK(!called);
    CHECK(reader.pending() == 1);
    waitForAll(reader);
    CHECK(called);
    CHECK(sameAsWritten(result, 1000));
    CHECK(reader.pending() == 0);
  }

  SECTION("an empty file") {
    const std::string path = dir.write("empty.bin", 0);
    FileResult result;
    reader.read(path, [&](FileResult file) { result = std::move(file); });
    waitForAll(reader);
    REQUIRE(result.has_value());
    CHECK(result->size == 0);
  }

  SECTION("a missing file gives the error of the OS") {
    FileResult result;
    reader.read((dir.path / "missing.bin").string(), [&](FileResult file) { result = std::move(file); });
    waitForAll(reader);
    REQUIRE(!result.has_value());
    CHECK(result.error() == std::errc::no_such_file_or_directory);
  }

  SECTION("a directory gives an error, not bytes") {
    FileResult result;
    reader.read(dir.path.string(), [&](FileResult file) { result = std::move(file); });
    waitForAll(reader);
    CHECK(!result.has_value());
  }

  SECTION("a large file") {
    constexpr size_t SIZE = (size_t{1} << 24) + 123;
    const std::string path = dir.write("large.bin", SIZE);
    FileResult result;
    reader.read(path, [&](FileResult file) { result = std::move(file); });
    waitForAll(reader);
    CHECK(sameAsWritten(result, SIZE));
  }

  SECTION("more files than the ring has room for wait in the reader") {
    FileReader small(4);
    constexpr size_t FILES = 100;
    std::vector<std::string> paths;
    for (size_t i = 0; i < FILES; ++i) {
      paths.push_back(dir.write("many_" + std::to_string(i) + ".bin", 100 + i));
    }
    std::vector<size_t> sizes(FILES, 0);
    std::vector<int> order;
    for (size_t i = 0; i < FILES; ++i) {
      small.read(paths[i], [&, i](FileResult file) {
        CHECK(sameAsWritten(file, 100 + i));
        sizes[i] = file ? file->size : 0;
        order.push_back(static_cast<int>(i));
      });
    }
    waitForAll(small);
    CHECK(order.size() == FILES);
    for (size_t i = 0; i < FILES; ++i) {
      CHECK(sizes[i] == 100 + i);
    }
  }

  SECTION("read on many threads, poll on one") {
    sc::threading::init();
    constexpr size_t FILES = 300;
    std::vector<std::string> paths;
    for (size_t i = 0; i < FILES; ++i) {
      paths.push_back(dir.write("threads_" + std::to_string(i) + ".bin", 1 + ((i * 37) % 5000)));
    }
    std::atomic<size_t> right{0};
    sc::threading::parallelFor(FILES, 4, [&](size_t first, size_t last) {
      for (size_t i = first; i < last; ++i) {
        reader.read(paths[i], [&, i](FileResult file) {
          if (sameAsWritten(file, 1 + ((i * 37) % 5000))) {
            right.fetch_add(1, std::memory_order_relaxed);
          }
        });
        if (i % 16 == 0) {
          (void) reader.poll();
        }
      }
    });
    waitForAll(reader);
    CHECK(right.load() == FILES);
  }

  SECTION("a done that throws does not keep the others from theirs") {
    const std::string path = dir.write("throws.bin", 10);
    int called = 0;
    reader.read(path, [&](FileResult) {
      ++called;
      throw std::runtime_error("done");
    });
    reader.read(path, [&](FileResult) { ++called; });
    bool thrown = false;
    while (reader.pending() != 0) {
      try {
        (void) reader.wait();
      } catch (const std::runtime_error &) {
        thrown = true;
      }
    }
    CHECK(thrown);
    CHECK(called == 2);
  }

  SECTION("the destructor gives every read to its done") {
    const std::string path = dir.write("destructor.bin", 4096);
    int called = 0;
    {
      FileReader local;
      for (int i = 0; i < 10; ++i) {
        local.read(path, [&](FileResult file) { called += sameAsWritten(file, 4096) ? 1 : 0; });
      }
    }
    CHECK(called == 10);
  }
}
