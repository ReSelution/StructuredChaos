#include "sc/io/file_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <mutex>
#include <utility>
#include <vector>

#if __has_include(<liburing.h>)
#define SC_IO_URING 1
#include <deque>

#include <fcntl.h>
#include <liburing.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define SC_IO_URING 0
#include <filesystem>
#include <fstream>
#endif

namespace sc::io {

  namespace {

    // Calls every done with its result, also when one throws, and rethrows
    // the first exception once all of them have been called.
    template<typename Finished>
    size_t deliver(std::vector<Finished> &finished) {
      std::exception_ptr first;
      for (Finished &read: finished) {
        try {
          read.call();
        } catch (...) {
          if (!first) {
            first = std::current_exception();
          }
        }
      }
      const size_t count = finished.size();
      finished.clear();
      if (first) {
        std::rethrow_exception(first);
      }
      return count;
    }

  } // namespace

#if SC_IO_URING

  // A read under way, the user_data of its request to the kernel: one at a
  // time, first the open, then the reads.
  struct FileReader::Request {
    std::string path;
    Done done;
    int fd = -1;
    // As errno.
    int error = 0;
    // file.size is the size the file had; got how much of it is read.
    FileBytes file;
    size_t got = 0;

    void call() {
      done(error != 0 ? FileResult(std::unexpected(std::error_code(error, std::system_category())))
                      : FileResult(std::move(file)));
    }
  };

  struct FileReader::Ring {
    // A read asks the kernel for at most this much at once; Linux reads at
    // most a little less than 2 GiB per call anyway.
    static constexpr size_t MOST_PER_READ = size_t{1} << 30;

    io_uring ring{};
    unsigned depth;
    // The ring and everything below. read, poll and wait take it only for
    // as long as they change something, never while they wait.
    std::mutex mutex;
    // Requests given to the kernel and not answered yet: at most depth, so
    // that neither queue of the ring can overflow.
    unsigned underWay = 0;
    // Reads that wait for room, in the order read was called.
    std::deque<Request *> backlog;
    size_t pending = 0;
    // Prepared since the last submit.
    unsigned prepared = 0;

    struct Finished {
      Request *request;

      void call() const {
        const std::unique_ptr<Request> owned(request);
        owned->call();
      }
    };

    explicit Ring(unsigned depth) : depth(depth) {
      if (const int result = io_uring_queue_init(depth, &ring, 0); result < 0) {
        throw std::system_error(-result, std::system_category(), "FileReader: io_uring_queue_init");
      }
      // wait waits with a timeout and without the lock: without EXT_ARG
      // (Linux 5.11), liburing would put the timeout into the submission
      // queue for it, next to a read that fills it.
      if ((ring.features & IORING_FEAT_EXT_ARG) == 0) {
        io_uring_queue_exit(&ring);
        throw std::system_error(ENOSYS, std::system_category(), "FileReader: io_uring without IORING_FEAT_EXT_ARG");
      }
    }

    ~Ring() { io_uring_queue_exit(&ring); }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    // An entry of the submission queue. There is always one: no more than
    // depth requests are under way or prepared.
    io_uring_sqe *entry(Request *request) {
      io_uring_sqe *const sqe = io_uring_get_sqe(&ring);
      io_uring_sqe_set_data(sqe, request);
      ++underWay;
      ++prepared;
      return sqe;
    }

    // Opens through the ring. Not statx by path: io_uring hands every statx
    // to iou-wrk threads of the kernel (measured: 8 to 9 of them for 500
    // files, none for openat and read).
    void start(Request *request) {
      io_uring_prep_openat(entry(request), AT_FDCWD, request->path.c_str(), O_RDONLY | O_CLOEXEC, 0);
    }

    // The rest of the file, from where the bytes read so far end.
    void readOn(Request *request) {
      const size_t count = std::min(request->file.size - request->got, MOST_PER_READ);
      io_uring_prep_read(entry(request), request->fd, request->file.data.get() + request->got,
                         static_cast<unsigned>(count), request->got);
    }

    // After the open: the size by fstat, which waits for no disk, since
    // the open has brought the inode in already.
    void opened(Request *request, int result, std::vector<Finished> &finished) {
      if (result < 0) {
        request->error = -result;
        finish(request, finished);
        return;
      }
      request->fd = result;
      struct stat status{};
      if (::fstat(request->fd, &status) != 0) {
        request->error = errno;
      } else if (!S_ISREG(status.st_mode)) {
        request->error = S_ISDIR(status.st_mode) ? EISDIR : EINVAL;
      } else if (status.st_size != 0) {
        request->file.size = static_cast<size_t>(status.st_size);
        request->file.data = std::make_unique_for_overwrite<std::byte[]>(request->file.size);
        readOn(request);
        return;
      }
      finish(request, finished);
    }

    void answer(io_uring_cqe *cqe, std::vector<Finished> &finished) {
      auto *const request = static_cast<Request *>(io_uring_cqe_get_data(cqe));
      --underWay;
      const int result = cqe->res;
      if (request->fd < 0) {
        opened(request, result, finished);
        return;
      }
      if (result < 0) {
        request->error = -result;
      } else if (result == 0) {
        // The file has become shorter since fstat: what there is.
        request->file.size = request->got;
      } else {
        // A read may give fewer bytes than asked for.
        request->got += static_cast<size_t>(result);
        if (request->got < request->file.size) {
          readOn(request);
          return;
        }
      }
      finish(request, finished);
    }

    void finish(Request *request, std::vector<Finished> &finished) {
      if (request->fd >= 0) {
        // Not through the ring: closing a file that is only read waits for
        // nothing.
        ::close(request->fd);
        request->fd = -1;
      }
      --pending;
      finished.push_back({request});
    }

    // Answers what the kernel has answered, starts what waits for room and
    // hands everything prepared to the kernel. Under the lock.
    void reap(std::vector<Finished> &finished) {
      unsigned head;
      unsigned seen = 0;
      io_uring_cqe *cqe;
      io_uring_for_each_cqe(&ring, head, cqe) {
        answer(cqe, finished);
        ++seen;
      }
      io_uring_cq_advance(&ring, seen);
      while (!backlog.empty() && underWay < depth) {
        start(backlog.front());
        backlog.pop_front();
      }
      submit();
    }

    void submit() {
      while (prepared != 0) {
        const int result = io_uring_submit(&ring);
        if (result < 0) {
          throw std::system_error(-result, std::system_category(), "FileReader: io_uring_submit");
        }
        prepared -= static_cast<unsigned>(result);
      }
    }
  };

  FileReader::FileReader(unsigned depth) : m_ring(std::make_unique<Ring>(std::max(depth, 1U))) {}

  FileReader::~FileReader() {
    while (pending() != 0) {
      try {
        (void) wait();
        // NOLINTNEXTLINE(bugprone-empty-catch): a done that throws must not stop the others
      } catch (...) {
      }
    }
  }

  void FileReader::read(std::string path, Done done) {
    auto request = std::make_unique<Request>();
    request->path = std::move(path);
    request->done = std::move(done);
    const std::scoped_lock lock(m_ring->mutex);
    ++m_ring->pending;
    if (m_ring->underWay >= m_ring->depth || !m_ring->backlog.empty()) {
      m_ring->backlog.push_back(request.release());
      return;
    }
    m_ring->start(request.release());
    m_ring->submit();
  }

  size_t FileReader::poll() {
    std::vector<Ring::Finished> finished;
    {
      const std::scoped_lock lock(m_ring->mutex);
      m_ring->reap(finished);
    }
    return deliver(finished);
  }

  size_t FileReader::wait() {
    while (true) {
      std::vector<Ring::Finished> finished;
      bool underWay;
      {
        const std::scoped_lock lock(m_ring->mutex);
        m_ring->reap(finished);
        underWay = m_ring->underWay != 0;
      }
      if (!finished.empty() || !underWay) {
        return deliver(finished);
      }
      // Without the lock, so that read and poll go on meanwhile, and only
      // waits: the answer is taken by whoever reaps next. With a timeout,
      // because another thread may take the last answer between the reap
      // above and this.
      io_uring_cqe *cqe;
      __kernel_timespec timeout{.tv_sec = 0, .tv_nsec = 10'000'000};
      (void) io_uring_wait_cqe_timeout(&m_ring->ring, &cqe, &timeout);
    }
  }

  size_t FileReader::pending() const {
    const std::scoped_lock lock(m_ring->mutex);
    return m_ring->pending;
  }

#else

  // Without io_uring, read reads right away.
  struct FileReader::Request {
    Done done;
    FileResult result;

    void call() { done(std::move(result)); }
  };

  struct FileReader::Ring {
    std::mutex mutex;
    std::vector<Request> finished;
  };

  namespace {

    FileResult readNow(const std::string &path) {
      std::error_code error;
      const auto size = std::filesystem::file_size(path, error);
      if (error) {
        return std::unexpected(error);
      }
      FileBytes file{.data = std::make_unique_for_overwrite<std::byte[]>(size), .size = size};
      std::ifstream stream(path, std::ios::binary);
      if (!stream.read(reinterpret_cast<char *>(file.data.get()), static_cast<std::streamsize>(size))) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
      }
      return file;
    }

  } // namespace

  FileReader::FileReader(unsigned /*depth*/) : m_ring(std::make_unique<Ring>()) {}

  FileReader::~FileReader() {
    try {
      (void) poll();
      // NOLINTNEXTLINE(bugprone-empty-catch): a done that throws must not stop the others
    } catch (...) {
    }
  }

  void FileReader::read(std::string path, Done done) {
    Request request{.done = std::move(done), .result = readNow(path)};
    const std::scoped_lock lock(m_ring->mutex);
    m_ring->finished.push_back(std::move(request));
  }

  size_t FileReader::poll() {
    std::vector<Request> finished;
    {
      const std::scoped_lock lock(m_ring->mutex);
      finished.swap(m_ring->finished);
    }
    return deliver(finished);
  }

  size_t FileReader::wait() { return poll(); }

  size_t FileReader::pending() const {
    const std::scoped_lock lock(m_ring->mutex);
    return m_ring->finished.size();
  }

#endif

} // namespace sc::io
