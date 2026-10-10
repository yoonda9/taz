// Shared test fixture for handlers/file.c unit tests (FILE_* and DIR_*):
// drives a REQUEST through taz_dispatch_frame with a real uv_loop_t and a
// fake dispatch (counting conn_ref/conn_unref, a closing flag), against a
// scratch directory removed in TearDown. Included by test_file.cpp and
// test_dir.cpp, each linked into the same taz_tests binary - every free
// function here is `static` (internal linkage) so the two TUs never clash;
// the class definitions themselves are identical in both TUs, which the
// ODR explicitly allows.

#ifndef TAZ_TESTS_FILE_TEST_SUPPORT_H
#define TAZ_TESTS_FILE_TEST_SUPPORT_H

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <uv.h>

#ifndef _WIN32
#include <csignal>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "taz/dispatch.h"
#include "taz/frame.h"
#include "taz/fsutil.h"
#include "taz/v1/common.pb.h"
#include "taz/work.h"

// ---------------------------------------------------------------------------
// Fake dispatch (mirrors test_work.cpp's FakeConn): counts conn_ref/
// conn_unref, exposes a closing flag, stands in for connection.c.
// ---------------------------------------------------------------------------

struct FakeConn
{
    int ref_count = 0;
    int unref_count = 0;
    int closing = 0;
    int pause_calls = 0;
    int resume_calls = 0;
    size_t queue_size = 0;
};

static void CountRef(void *ctx)
{
    static_cast<FakeConn *>(ctx)->ref_count++;
}

static void CountUnref(void *ctx)
{
    static_cast<FakeConn *>(ctx)->unref_count++;
}

static int IsClosing(void *ctx)
{
    return static_cast<FakeConn *>(ctx)->closing;
}

static void CountPauseReads(void *ctx)
{
    static_cast<FakeConn *>(ctx)->pause_calls++;
}

static void CountResumeReads(void *ctx)
{
    static_cast<FakeConn *>(ctx)->resume_calls++;
}

static size_t FakeQueueSize(void *ctx)
{
    return static_cast<FakeConn *>(ctx)->queue_size;
}

struct WriteCtx
{
    std::vector<std::vector<uint8_t>> frames;
};

static void capture_write(const uint8_t *data, size_t len, void *ctx)
{
    auto *wctx = static_cast<WriteCtx *>(ctx);
    wctx->frames.emplace_back(data, data + len);
}

static taz_frame_header_t unpack_header(const std::vector<uint8_t> &frame)
{
    taz_frame_header_t h{};
    if (frame.size() >= static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        taz_frame_unpack_header(frame.data(), &h);
    }
    return h;
}

static std::vector<uint8_t> frame_payload(const std::vector<uint8_t> &frame)
{
    if (frame.size() < static_cast<size_t>(TAZ_FRAME_HEADER_SIZE))
    {
        return {};
    }
    return {frame.begin() + TAZ_FRAME_HEADER_SIZE, frame.end()};
}

#ifndef _WIN32
// Exact-mode assertions on files created via uv_fs_open depend on the
// process umask (e.g. 'umask 077' turns a requested 0644 into 0600); pin a
// known umask for the test's duration and restore the caller's on exit.
class ScopedUmask
{
  public:
    explicit ScopedUmask(mode_t mask) : prev_(umask(mask))
    {
    }
    ~ScopedUmask()
    {
        (void)umask(prev_);
    }
    ScopedUmask(const ScopedUmask &) = delete;
    ScopedUmask &operator=(const ScopedUmask &) = delete;

  private:
    mode_t prev_;
};

// Pins a path's POSIX mode bits for the duration of the test, restoring the
// mode observed at construction time on scope exit (via lstat, not a
// hardcoded value) so TearDown's RemoveTree can still traverse a directory
// a test deliberately made unsearchable. Uses EXPECT_ rather than ASSERT_
// in the constructor/destructor since the latter would return out of a
// function with no return type.
class ScopedChmod
{
  public:
    ScopedChmod(std::string path, int mode) : path_(std::move(path))
    {
        uv_fs_t stat_req;
        EXPECT_EQ(uv_fs_lstat(nullptr, &stat_req, path_.c_str(), nullptr), 0);
        prev_mode_ = static_cast<int>(stat_req.statbuf.st_mode & 07777U);
        uv_fs_req_cleanup(&stat_req);

        uv_fs_t chmod_req;
        EXPECT_EQ(
            uv_fs_chmod(nullptr, &chmod_req, path_.c_str(), mode, nullptr), 0);
        uv_fs_req_cleanup(&chmod_req);
    }
    ~ScopedChmod()
    {
        uv_fs_t chmod_req;
        EXPECT_EQ(uv_fs_chmod(nullptr, &chmod_req, path_.c_str(), prev_mode_,
                              nullptr),
                  0);
        uv_fs_req_cleanup(&chmod_req);
    }
    ScopedChmod(const ScopedChmod &) = delete;
    ScopedChmod &operator=(const ScopedChmod &) = delete;

  private:
    std::string path_;
    int prev_mode_ = 0;
};

// Pins RLIMIT_FSIZE for the duration of the test (e.g. to force EFBIG on a
// write past a small cap), restoring the previous limit on scope exit.
class ScopedFsizeLimit
{
  public:
    explicit ScopedFsizeLimit(rlim_t bytes)
    {
        EXPECT_EQ(getrlimit(RLIMIT_FSIZE, &prev_), 0);
        const struct rlimit lim{bytes, prev_.rlim_max};
        EXPECT_EQ(setrlimit(RLIMIT_FSIZE, &lim), 0);
    }
    ~ScopedFsizeLimit()
    {
        (void)setrlimit(RLIMIT_FSIZE, &prev_);
    }
    ScopedFsizeLimit(const ScopedFsizeLimit &) = delete;
    ScopedFsizeLimit &operator=(const ScopedFsizeLimit &) = delete;

  private:
    struct rlimit prev_{};
};

// Ignores a signal for the duration of the test (RLIMIT_FSIZE's default
// SIGXFSZ would otherwise kill the process on the first over-limit write),
// restoring the previous disposition on scope exit.
class ScopedSignalIgnore
{
  public:
    explicit ScopedSignalIgnore(int sig)
        : sig_(sig), prev_(signal(sig, SIG_IGN))
    {
        EXPECT_NE(prev_, SIG_ERR);
    }
    ~ScopedSignalIgnore()
    {
        (void)signal(sig_, prev_);
    }
    ScopedSignalIgnore(const ScopedSignalIgnore &) = delete;
    ScopedSignalIgnore &operator=(const ScopedSignalIgnore &) = delete;

  private:
    int sig_;
    void (*prev_)(int);
};
#endif

// ---------------------------------------------------------------------------
// Fixture: real loop, fake dispatch, a scratch directory removed in
// TearDown.
// ---------------------------------------------------------------------------

class FileHandlerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_EQ(uv_loop_init(&loop_), 0);

        taz_dispatch_init(&d_);
        d_.loop = &loop_;
        d_.conn_ref = CountRef;
        d_.conn_unref = CountUnref;
        d_.conn_ctx = &conn_;
        d_.conn_closing = IsClosing;
        d_.conn_write_queue_size = FakeQueueSize;

        char tmpdir[1024];
        size_t tmpdir_len = sizeof(tmpdir) - 1U;
        ASSERT_EQ(uv_os_tmpdir(tmpdir, &tmpdir_len), 0);
        const std::string tpl_str =
            std::string(tmpdir, tmpdir_len) + "/taz_file_test_XXXXXX";
        std::vector<char> tpl(tpl_str.begin(), tpl_str.end());
        tpl.push_back('\0');

        uv_fs_t req;
        ASSERT_EQ(uv_fs_mkdtemp(nullptr, &req, tpl.data(), nullptr), 0);
        dir_ = req.path;
        uv_fs_req_cleanup(&req);
    }

    void TearDown() override
    {
        RemoveTree(dir_);
        ASSERT_EQ(uv_loop_close(&loop_), 0);
    }

    // Removes path (file, directory, or symlink - never following a
    // symlink into another tree). Best-effort cleanup of whatever a test
    // created under dir_, including nested directories (e.g. DIR_MAKE's
    // mkdir -p tests). Iterative (not recursive) to keep a single,
    // boring call stack regardless of tree depth: a depth-first scan
    // pushes every path onto a stack and records directories in
    // discovery order (a directory is always recorded before any of its
    // descendants), then removes directories in reverse of that order,
    // so every descendant is rmdir'd before its ancestor.
    static void RemoveTree(const std::string &root)
    {
        std::vector<std::string> stack{root};
        std::vector<std::string> dirs;

        while (!stack.empty())
        {
            const std::string path = stack.back();
            stack.pop_back();

            uv_fs_t lstat_req;
            if (uv_fs_lstat(nullptr, &lstat_req, path.c_str(), nullptr) < 0)
            {
                uv_fs_req_cleanup(&lstat_req);
                continue;
            }
            const bool is_dir = (lstat_req.statbuf.st_mode & S_IFMT) == S_IFDIR;
            uv_fs_req_cleanup(&lstat_req);

            if (!is_dir)
            {
                uv_fs_t unlink_req;
                (void)uv_fs_unlink(nullptr, &unlink_req, path.c_str(), nullptr);
                uv_fs_req_cleanup(&unlink_req);
                continue;
            }

            dirs.push_back(path);

            uv_fs_t scan_req;
            if (uv_fs_scandir(nullptr, &scan_req, path.c_str(), 0, nullptr) >=
                0)
            {
                uv_dirent_t ent;
                while (uv_fs_scandir_next(&scan_req, &ent) != UV_EOF)
                {
                    stack.push_back(path + "/" + ent.name);
                }
            }
            uv_fs_req_cleanup(&scan_req);
        }

        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
        {
            uv_fs_t rmdir_req;
            (void)uv_fs_rmdir(nullptr, &rmdir_req, it->c_str(), nullptr);
            uv_fs_req_cleanup(&rmdir_req);
        }
    }

    std::string JoinDir(const std::string &name) const
    {
        return dir_ + "/" + name;
    }

    const std::string &Dir() const
    {
        return dir_;
    }

    static void WriteFile(const std::string &path, const std::string &contents)
    {
        uv_fs_t open_req;
        const uv_file fd = uv_fs_open(
            nullptr, &open_req, path.c_str(),
            UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0644, nullptr);
        uv_fs_req_cleanup(&open_req);
        ASSERT_GE(fd, 0);

        if (!contents.empty())
        {
            const uv_buf_t buf =
                uv_buf_init(const_cast<char *>(contents.data()),
                            static_cast<unsigned int>(contents.size()));
            uv_fs_t write_req;
            const int n =
                uv_fs_write(nullptr, &write_req, fd, &buf, 1, 0, nullptr);
            uv_fs_req_cleanup(&write_req);
            ASSERT_EQ(n, static_cast<int>(contents.size()));
        }

        uv_fs_t close_req;
        (void)uv_fs_close(nullptr, &close_req, fd, nullptr);
        uv_fs_req_cleanup(&close_req);
    }

    // The umask applies to uv_fs_open's mode argument, so a freshly written
    // file's permissions are not reliably 0644 (e.g. 'umask 077' => 0600).
    // Tests that assert an exact mode must pin it explicitly first.
    static void ChmodFile(const std::string &path, int mode)
    {
        uv_fs_t chmod_req;
        ASSERT_EQ(uv_fs_chmod(nullptr, &chmod_req, path.c_str(), mode, nullptr),
                  0);
        uv_fs_req_cleanup(&chmod_req);
    }

    // Drives one REQUEST of the given opcode through taz_dispatch_frame
    // without running the loop afterwards - for tests that need a REQUEST
    // registered (and its stream_ops set, for FILE_PUT) before any FILE_CHUNK
    // frames are dispatched.
    void DispatchRequestNoRun(taz_v1_Opcode opcode,
                              const std::vector<uint8_t> &payload,
                              uint32_t stream_id)
    {
        taz_frame_header_t header{};
        header.type = static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_REQUEST);
        header.flags = static_cast<uint8_t>(taz_v1_FrameFlag_FRAME_FLAG_NONE);
        header.opcode = static_cast<uint16_t>(opcode);
        header.length = static_cast<uint32_t>(payload.size());
        header.stream_id = stream_id;

        taz_dispatch_frame(&d_, &header,
                           payload.empty() ? nullptr : payload.data(),
                           TAZ_FRAME_OK, capture_write, &wctx_);
    }

    // Drives one REQUEST of the given opcode through taz_dispatch_frame. If
    // check_in_flight is set, it runs right after dispatch returns but
    // before the loop runs, to observe state while the work is still
    // (ostensibly) in flight.
    void DispatchRequest(taz_v1_Opcode opcode,
                         const std::vector<uint8_t> &payload,
                         uint32_t stream_id,
                         const std::function<void()> &check_in_flight = nullptr)
    {
        DispatchRequestNoRun(opcode, payload, stream_id);
        if (check_in_flight)
        {
            check_in_flight();
        }
        ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
    }

    // Convenience wrapper for the FILE_STAT tests (written before FILE_CREATE/
    // FILE_DELETE existed; kept so those tests read the same as before).
    void
    DispatchStatRequest(const std::vector<uint8_t> &payload, uint32_t stream_id,
                        const std::function<void()> &check_in_flight = nullptr)
    {
        DispatchRequest(taz_v1_Opcode_OPCODE_FILE_STAT, payload, stream_id,
                        check_in_flight);
    }

    // Drives one FILE_CHUNK frame for stream_id through taz_dispatch_frame
    // without running the loop afterwards - for tests that need several
    // chunks queued before anything is processed (e.g. ingress backpressure).
    void DispatchChunkNoRun(uint32_t stream_id,
                            const std::vector<uint8_t> &bytes, bool last)
    {
        taz_frame_header_t header{};
        header.type =
            static_cast<uint8_t>(taz_v1_FrameType_FRAME_TYPE_FILE_CHUNK);
        header.flags = last ? 0U
                            : static_cast<uint8_t>(
                                  taz_v1_FrameFlag_FRAME_FLAG_CONTINUATION);
        header.opcode = 0U;
        header.length = static_cast<uint32_t>(bytes.size());
        header.stream_id = stream_id;

        taz_dispatch_frame(&d_, &header, bytes.empty() ? nullptr : bytes.data(),
                           TAZ_FRAME_OK, capture_write, &wctx_);
    }

    // Drives one FILE_CHUNK frame then runs the loop to idle - the usual
    // case, where each chunk's write step (if any) is expected to complete
    // before the next chunk is dispatched.
    void DispatchChunk(uint32_t stream_id, const std::vector<uint8_t> &bytes,
                       bool last)
    {
        DispatchChunkNoRun(stream_id, bytes, last);
        RunLoop();
    }

    void RunLoop()
    {
        ASSERT_EQ(uv_run(&loop_, UV_RUN_DEFAULT), 0);
    }

    // Runs a single loop iteration (one pool completion's worth of
    // callbacks) - for tests that need to observe a multi-step transfer one
    // step at a time, e.g. stopping after exactly N chunks. Returns
    // uv_run's result (non-zero while handles/requests remain).
    int RunLoopOnce()
    {
        return uv_run(&loop_, UV_RUN_ONCE);
    }

    // Installs counting fake pause/resume hooks (conn_write_queue_size stays
    // unset: PUT's ingress backpressure never reads it). Counts are exposed
    // via PauseCalls()/ResumeCalls().
    void EnablePauseResumeCounting()
    {
        d_.conn_pause_reads = CountPauseReads;
        d_.conn_resume_reads = CountResumeReads;
    }

    int PauseCalls() const
    {
        return conn_.pause_calls;
    }

    int ResumeCalls() const
    {
        return conn_.resume_calls;
    }

    // Settable fake for the connection's outbound write-queue depth, read by
    // FILE_GET's egress backpressure (taz_dispatch_conn_write_queue_size).
    void SetQueueSize(size_t n)
    {
        conn_.queue_size = n;
    }

    static bool PathExists(const std::string &path)
    {
        uv_fs_t req;
        const int rc = uv_fs_lstat(nullptr, &req, path.c_str(), nullptr);
        uv_fs_req_cleanup(&req);
        return rc == 0;
    }

    // Kind check via the same taz_fsutil_kind_from_mode the handlers use,
    // so this reflects the daemon's own notion of "directory" (including
    // the Windows FILE_ATTRIBUTE_DIRECTORY-derived mode bits), not just a
    // raw S_ISDIR on a platform where libuv's st_mode may be synthesized.
    static bool PathIsDir(const std::string &path)
    {
        uv_fs_t req;
        const int rc = uv_fs_lstat(nullptr, &req, path.c_str(), nullptr);
        const bool is_dir =
            (rc == 0) && (taz_fsutil_kind_from_mode(req.statbuf.st_mode) ==
                          taz_v1_Kind_KIND_DIR);
        uv_fs_req_cleanup(&req);
        return is_dir;
    }

    // On Windows this is never POSIX-exact (libuv derives it from the
    // READONLY attribute), but the 0222/0200 write bits this file's chmod
    // tests check do reflect that attribute faithfully.
    static uint64_t FileMode(const std::string &path)
    {
        uv_fs_t req;
        const int rc = uv_fs_lstat(nullptr, &req, path.c_str(), nullptr);
        const uint64_t mode = (rc == 0) ? (req.statbuf.st_mode & 07777U) : 0U;
        uv_fs_req_cleanup(&req);
        return mode;
    }

    static std::string ReadFileBytes(const std::string &path)
    {
        uv_fs_t open_req;
        const uv_file fd = uv_fs_open(nullptr, &open_req, path.c_str(),
                                      UV_FS_O_RDONLY, 0, nullptr);
        uv_fs_req_cleanup(&open_req);
        if (fd < 0)
        {
            return std::string();
        }

        std::string data;
        char chunk[4096];
        for (;;)
        {
            const uv_buf_t buf = uv_buf_init(chunk, sizeof(chunk));
            uv_fs_t read_req;
            const int n =
                uv_fs_read(nullptr, &read_req, fd, &buf, 1,
                           static_cast<int64_t>(data.size()), nullptr);
            uv_fs_req_cleanup(&read_req);
            if (n <= 0)
            {
                break;
            }
            data.append(chunk, static_cast<size_t>(n));
        }

        uv_fs_t close_req;
        (void)uv_fs_close(nullptr, &close_req, fd, nullptr);
        uv_fs_req_cleanup(&close_req);
        return data;
    }

    const std::vector<std::vector<uint8_t>> &Frames() const
    {
        return wctx_.frames;
    }

    size_t ActiveStreamCount() const
    {
        return d_.active_count;
    }

    int RefCount() const
    {
        return conn_.ref_count;
    }

    int UnrefCount() const
    {
        return conn_.unref_count;
    }

    void SetConnClosing(int closing)
    {
        conn_.closing = closing;
    }

    // Mirrors conn_close(): mark the connection closing, then abort every
    // active stream. handlers/file_transfer.c's abort op frees a DRAINING
    // stream synchronously (taz_dispatch_stream_done included) but defers
    // every other stream's release to a pool cleanup step, so a RunLoop()
    // afterwards is still needed to observe the rest of the cleanup
    // complete.
    void CloseConnectionAndCancelAll()
    {
        conn_.closing = 1;
        taz_dispatch_cancel_all(&d_);
    }

    // Mirrors a SIGTERM/SIGINT handler: requests the deferred shutdown that
    // makes outstanding taz_work_submit_step work finish (closing == 1 in
    // its done callback) before the loop is allowed to stop. Sticky across
    // the whole test binary process - callers must pair this with
    // taz_work_reset_for_tests() before returning (mem: a forgotten reset
    // broke 49/288 later FileHandlerTest cases under just test-valgrind).
    void RequestShutdown()
    {
        taz_work_request_shutdown(&loop_);
    }

    // Notifies every stream registered with on_writable that a write just
    // completed - FILE_GET's egress backpressure resumes here.
    void NotifyWritable()
    {
        taz_dispatch_notify_writable(&d_);
    }

    // The real uv_loop_t backing this fixture, for tests that need to
    // uv_spawn a real child alongside a dispatched request (e.g.
    // PROCESS_KILL against a live sleeper).
    uv_loop_t *Loop()
    {
        return &loop_;
    }

    // Direct access to the fake taz_dispatch_t, for tests that need to seed
    // or inspect per-connection state a handler reads/writes (e.g. RUN_AS's
    // run_as_active/uid/gid/user, TIMEOUT_SET's conn_timeout_ms) beyond
    // what the frame-level assertions above cover.
    taz_dispatch_t &Dispatch()
    {
        return d_;
    }

  private:
    uv_loop_t loop_{};
    taz_dispatch_t d_{};
    FakeConn conn_{};
    WriteCtx wctx_{};
    std::string dir_;
};

#endif // TAZ_TESTS_FILE_TEST_SUPPORT_H
