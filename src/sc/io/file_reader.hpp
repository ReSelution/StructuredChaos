#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <system_error>

// liburing is only included in file_reader.cpp, so that its macros and the
// kernel's headers stay out of everything that includes this.

namespace sc::io {

  // The bytes of a whole file. Not zeroed before the read, so a large file
  // is written once, by the kernel.
  struct FileBytes {
    std::unique_ptr<std::byte[]> data;
    size_t size = 0;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data.get(), size}; }
  };

  // What a read ends with: the bytes, or the error of the OS (ENOENT for a
  // missing file, and so on).
  using FileResult = std::expected<FileBytes, std::error_code>;

  // Reads whole files without a thread that waits for them. On Linux the
  // kernel reads through io_uring: read only hands the file to it and
  // returns, and no thread of ours is blocked meanwhile. Measured on btrfs,
  // Linux 7.2, with files dropped from the page cache: the open and the
  // reads need no iou-wrk thread of the kernel either; statx would, so the
  // size comes from fstat after the open. Elsewhere, read reads the file
  // right away, on the calling thread.
  //
  //   FileReader files;
  //   files.read("textures/rock.png", [](FileResult file) { ... });
  //   // once per frame, or wherever there is time:
  //   files.poll();
  //
  // done is called by poll or wait, on the thread that calls them, never in
  // read: who reads decides where the bytes are handled.
  //
  // read, poll and wait may be called on any thread, next to each other.
  // One read takes two trips to the kernel: the open, then the bytes.
  // Each is a poll, so a file that is called for once per frame is there
  // after two frames at the earliest.
  class FileReader {
  public:
    using Done = std::move_only_function<void(FileResult)>;

    // depth: how many reads may be under way in the kernel at once; the
    // others wait in the reader. Throws
    // std::system_error if the kernel has no io_uring for us (switched off
    // with the sysctl kernel.io_uring_disabled, or in a container that
    // forbids it).
    explicit FileReader(unsigned depth = 256);

    // Waits for every read under way and calls its done: the kernel writes
    // into the buffers until then.
    ~FileReader();

    FileReader(const FileReader &) = delete;
    FileReader &operator=(const FileReader &) = delete;

    // Starts reading the whole file at path.
    void read(std::string path, Done done);

    // Calls done for every read that is over, and returns how many.
    size_t poll();

    // As poll, but if no read is over and one is under way, waits for the
    // next first.
    size_t wait();

    // Reads that have not been given to their done yet.
    [[nodiscard]] size_t pending() const;

  private:
    struct Ring;
    struct Request;

    std::unique_ptr<Ring> m_ring;
  };

} // namespace sc::io
