/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * Design write requests through the daemon's own connection and request code.
 *
 * The test runs on_event_new_connection(), on_client_io() and
 * on_event_deferred_work() on an sd_event loop in a thread of this process,
 * against a mock device and a private socket, and talks to it with libvrtd.
 * It never connects to a running vrtd and never touches a card:
 *
 *   - the transfer goes through the device's design writer on the mock QDMA
 *     (libslash "@mock"); a pipe holds a write in flight, a directory makes
 *     it fail;
 *   - the steps that act on the card (AMC check, clock park, PF2 refresh) are
 *     the test's own, through state.design_write_hw;
 *   - hotplug requests reach libslash with no hotplug handle open
 *     (g_hotplug == NULL, checked in SetUp), so they fail with EINVAL after
 *     vrtd has done its bookkeeping;
 *   - no request takes a shell-switch reset: the mock board is in the shell
 *     every write asks for, except where the request is refused before.
 *
 * The rules themselves are covered by design_state_test.cpp; these tests
 * check that serve.c applies them.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <vrtd/vrtd.h>

#include "test_helpers.hpp"

extern "C" {
#include <slash/ctldev.h>
#include <slash/qdma.h>
#include "accept.h"
#include "config.h"
#include "design_state.h"
#include "design_writer.h"
#include "device.h"
#include "flash_worker.h"
#include "hotplug.h"
#include "serve.h"
#include "state.h"
}

namespace {

using namespace std::chrono_literals;

const uint8_t kNull[VRTD_DESIGN_UUID_LEN] = {0};
const uint8_t kDesignA[VRTD_DESIGN_UUID_LEN] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
const uint8_t kDesignB[VRTD_DESIGN_UUID_LEN] = {
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

constexpr uint64_t kDeferredWorkUsec = 20 * 1000;

/* ---- The card steps of a design write, as the test plays them ------------ */

struct TestHw {
    std::atomic<int> amcChecks{0};
    std::atomic<int> parks{0};
    std::atomic<int> refreshes{0};
    std::atomic<uint16_t> refreshResult{VRTD_RET_OK};
};

TestHw* gHw = nullptr;

bool testAmcReady(const struct device*, char* state, size_t stateLen)
{
    gHw->amcChecks++;
    snprintf(state, stateLen, "READY (test)");
    return true;
}

int testParkUserClock(struct device*)
{
    gHw->parks++;
    return 0;
}

uint16_t testRefreshPf2(struct device*)
{
    gHw->refreshes++;
    return gHw->refreshResult.load();
}

const struct design_write_hw kTestHw = {
    .amc_ready = testAmcReady,
    .park_user_clock = testParkUserClock,
    .refresh_pf2 = testRefreshPf2,
};

/* ---- One client connection to the in-process daemon ----------------------- */

class Conn {
   public:
    explicit Conn(const std::string& path) : fd_(vrtd_connect(path.c_str())) {}
    ~Conn() { disconnect(); }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    int fd() const { return fd_; }

    void disconnect() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    enum vrtd_ret writeFile(const std::string& path, const uint8_t* uuid, uint8_t flags,
                            struct vrtd_design_write_result* result,
                            uint8_t shell = VRTD_SHELL_COMPUTE) const {
        return vrtd_design_write_file(fd_, 0, path.c_str(), shell, uuid, flags, result);
    }

    /* A request for a design the device holds is never transferred; any file does. */
    enum vrtd_ret request(const uint8_t* uuid, uint8_t flags,
                          struct vrtd_design_write_result* result,
                          uint8_t shell = VRTD_SHELL_COMPUTE) const {
        return writeFile("/dev/null", uuid, flags, result, shell);
    }

    enum vrtd_ret writeFd(int inputFd, const uint8_t* uuid, uint8_t flags,
                          struct vrtd_design_write_result* result) const {
        return vrtd_design_write(fd_, 0, inputFd, VRTD_SHELL_COMPUTE, uuid, flags, result);
    }

    enum vrtd_ret forget() const { return vrtd_forget_design(fd_, 0); }

    struct vrtd_design_info info() const {
        struct vrtd_design_info info;
        std::memset(&info, 0xff, sizeof(info));
        EXPECT_EQ(vrtd_get_design_info(fd_, 0, &info), VRTD_RET_OK);
        return info;
    }

   private:
    int fd_;
};

bool isUuid(const struct vrtd_design_info& info, const uint8_t* uuid) {
    return std::memcmp(info.uuid, uuid, VRTD_DESIGN_UUID_LEN) == 0;
}

size_t countOpenFds() {
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        n++;
    }
    return n;
}

/* A PDI whose transfer lasts until the test closes it. */
class HeldPdi {
   public:
    HeldPdi() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0) {
            throw std::runtime_error("pipe2 failed");
        }
        readFd_ = fds[0];
        writeFd_ = fds[1];
        const std::string payload(4096, '\x5a');
        if (::write(writeFd_, payload.data(), payload.size()) != (ssize_t)payload.size()) {
            throw std::runtime_error("pipe write failed");
        }
    }
    ~HeldPdi() {
        release();
        if (readFd_ >= 0) {
            close(readFd_);
        }
    }
    int readFd() const { return readFd_; }
    void release() {
        if (writeFd_ >= 0) {
            close(writeFd_);
            writeFd_ = -1;
        }
    }

   private:
    int readFd_ = -1;
    int writeFd_ = -1;
};

class DesignWriteServeTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    std::string sockPath;
    std::string pdiPath;

    TestHw hw;
    struct config cfg {};
    struct user_config defaultUser {};
    struct vrtd state {};
    struct device* dev = nullptr; /* owned by state.devices */

    int listenFd = -1;
    std::thread server;
    std::atomic<bool> stop{false};
    std::atomic<bool> deferredPaused{false};

    std::mutex tasksMutex;
    std::deque<std::function<void()>> tasks;

    void SetUp() override {
        /* Hotplug requests must not be able to reach the PCIe bus. */
        ASSERT_EQ(g_hotplug, nullptr);

        gHw = &hw;
        state.design_write_hw = &kTestHw;

        /* Every user may do everything on every device. */
        struct role* role = nullptr;
        ASSERT_EQ(role_merge_new(&role, "fullaccess"), 0);
        role->query = true;
        auto* dp = static_cast<struct device_policy*>(calloc(1, sizeof(struct device_policy)));
        ASSERT_NE(dp, nullptr);
        dp->bdf = strdup("any");
        dp->design_write = true;
        dp->pcie_hotplug = true;
        ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &dp), 0);
        ASSERT_EQ(role_ref_array_push(&defaultUser.roles, role), 0);
        ASSERT_EQ(role_ptr_array_push_move(&cfg.roles, &role), 0);
        defaultUser.name = strdup("*");
        cfg.default_user = &defaultUser;
        state.config = &cfg;

        /* An idle flash worker: async device ops complete as soon as polled. */
        state.flash_worker = flash_worker_create();
        ASSERT_NE(state.flash_worker, nullptr);

        /* A mock board in the compute shell, as after a daemon start. */
        dev = static_cast<struct device*>(calloc(1, sizeof(*dev)));
        ASSERT_NE(dev, nullptr);
        dev->path = strdup("@mock");
        dev->buffers = buffer_ptr_array_init();
        dev->design = design_state_init();
        dev->ctl = slash_ctldev_open("@mock");
        ASSERT_NE(dev->ctl, nullptr);
        dev->qdma = slash_qdma_open("@mock");
        ASSERT_NE(dev->qdma, nullptr);
        dev->design_writer = design_writer_create(dev->qdma);
        ASSERT_NE(dev->design_writer, nullptr);
        std::strncpy(dev->pci_info.bdf, "0000:00:00", sizeof(dev->pci_info.bdf) - 1);
        dev->current_shell = VRTD_SHELL_COMPUTE;
        ASSERT_EQ(device_ptr_array_push(&state.devices, dev), 0);

        tmpDir = makeTempDir("vrtd-serve-test");
        sockPath = (tmpDir / "vrtd.sock").string();
        pdiPath = writeTempFile(tmpDir, "design.pdi", std::string(8192, '\x3c'));

        struct sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        ASSERT_LT(sockPath.size(), sizeof(addr.sun_path));
        std::strcpy(addr.sun_path, sockPath.c_str());
        listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        ASSERT_GE(listenFd, 0);
        ASSERT_EQ(bind(listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(listen(listenFd, 8), 0);
    }

    /* The design the board holds when the daemon starts serving. */
    void preload(const uint8_t* uuid) { std::memcpy(dev->design.uuid, uuid, VRTD_DESIGN_UUID_LEN); }

    void startServer() {
        std::promise<bool> ready;
        auto isReady = ready.get_future();
        server = std::thread([this, &ready] { serve(ready); });
        ASSERT_TRUE(isReady.get());
    }

    static int onDeferredWork(sd_event_source* s, uint64_t usec, void* user) {
        auto* self = static_cast<DesignWriteServeTest*>(user);
        if (self->deferredPaused) {
            (void)sd_event_source_set_time(s, usec + kDeferredWorkUsec);
            (void)sd_event_source_set_enabled(s, SD_EVENT_ON);
            return 0;
        }
        return on_event_deferred_work(s, usec, &self->state);
    }

    void serve(std::promise<bool>& ready) {
        sd_event* ev = nullptr;
        sd_event_source* listener = nullptr;
        sd_event_source* timer = nullptr;
        uint64_t now = 0;
        bool ok = sd_event_new(&ev) >= 0 &&
                  sd_event_add_io(ev, &listener, listenFd, EPOLLIN, on_event_new_connection,
                                  &state) >= 0 &&
                  sd_event_now(ev, CLOCK_MONOTONIC, &now) >= 0 &&
                  sd_event_add_time(ev, &timer, CLOCK_MONOTONIC, now + kDeferredWorkUsec,
                                    kDeferredWorkUsec / 2, onDeferredWork, this) >= 0;
        ready.set_value(ok);

        while (ok && !stop) {
            (void)sd_event_run(ev, 5 * 1000);
            runTasks();
        }

        /* Clients go before the devices they hold designs on. */
        client_ptr_array_free(&state.clients);
        sd_event_source_disable_unref(timer);
        sd_event_source_disable_unref(listener);
        sd_event_unref(ev);
    }

    void runTasks() {
        std::deque<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> lk(tasksMutex);
            todo.swap(tasks);
        }
        for (auto& task : todo) {
            task();
        }
    }

    /* Run @p fn on the daemon's thread, between two event-loop iterations. */
    template <typename F>
    auto onServer(F fn) -> decltype(fn()) {
        using R = decltype(fn());
        auto task = std::make_shared<std::packaged_task<R()>>(fn);
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lk(tasksMutex);
            tasks.emplace_back([task] { (*task)(); });
        }
        return result.get();
    }

    /* Wait until @p cond, evaluated on the daemon's thread, holds. */
    template <typename F>
    bool waitOnServer(F cond) {
        for (int i = 0; i < 500; i++) {
            if (onServer(cond)) {
                return true;
            }
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    bool waitForWriteInFlight() {
        return waitOnServer([this] {
            return dev->design.writing && design_writer_is_busy(dev->design_writer);
        });
    }

    size_t holdersOnServer() {
        return onServer([this] { return dev->design.holders.len; });
    }

    void TearDown() override {
        stop = true;
        if (server.joinable()) {
            server.join();
        }
        if (listenFd >= 0) {
            close(listenFd);
        }

        cleanup_flash_worker(state.flash_worker);
        device_ptr_array_free(&state.devices);
        uint64_array_free(&state.deferred_cleanup_conn_ids);
        role_ptr_array_free(&cfg.roles);
        role_ref_array_free(&defaultUser.roles);
        str_array_free(&defaultUser.role_names);
        free(defaultUser.name);

        std::filesystem::remove_all(tmpDir);
        gHw = nullptr;
    }
};

/* ---- Query ------------------------------------------------------------------- */

TEST_F(DesignWriteServeTest, UnknownDesignIsReportedAsNull) {
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    struct vrtd_design_info info = alice.info();

    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 0u);
}

/* ---- Skip -------------------------------------------------------------------- */

TEST_F(DesignWriteServeTest, LoadedDesignIsSkippedAndHeld) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    ASSERT_GE(alice.fd(), 0);
    ASSERT_GE(bob.fd(), 0);

    struct vrtd_design_write_result result {};
    EXPECT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.num_holders, 1u);

    struct vrtd_design_info info = alice.info();
    EXPECT_TRUE(isUuid(info, kDesignA));
    EXPECT_EQ(info.num_holders, 1u);

    result = {};
    EXPECT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.num_holders, 2u);

    /* A second request of the same client does not count twice. */
    result = {};
    EXPECT_EQ(alice.request(kDesignA, VRTD_DESIGN_WRITE_MORE, &result), VRTD_RET_OK);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.num_holders, 2u);

    /* Nothing touched the card. */
    EXPECT_EQ(hw.amcChecks, 0);
    EXPECT_EQ(hw.parks, 0);
    EXPECT_EQ(hw.refreshes, 0);
}

TEST_F(DesignWriteServeTest, SkipRefusedWhenAShellSwitchIsRequired) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
    ASSERT_TRUE(result.skipped);

    /* Same design, other shell: it has to be written, and Alice holds it. */
    result = {};
    EXPECT_EQ(bob.request(kDesignA, 0, &result, VRTD_SHELL_SERVICE), VRTD_RET_BUSY);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(bob.info().num_holders, 1u);
}

TEST_F(DesignWriteServeTest, SkipRefusedWhenTheDeviceNeedsAReset) {
    preload(kDesignA);
    dev->needs_reset = true;
    startServer();
    Conn alice(sockPath);

    struct vrtd_design_write_result result {};
    EXPECT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_DEVICE_UNHEALTHY);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(hw.parks, 0);
}

TEST_F(DesignWriteServeTest, SkippedWriteClosesTheReceivedFd) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
    ASSERT_TRUE(result.skipped);

    /* Client and daemon share this process: a PDI fd the daemon kept would show. */
    const size_t before = countOpenFds();
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
        ASSERT_TRUE(result.skipped);
    }
    EXPECT_EQ(countOpenFds(), before);
}

/* ---- Malformed requests --------------------------------------------------------- */

TEST_F(DesignWriteServeTest, UnknownFlagsAreRejected) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    struct vrtd_design_write_result result {};
    result.skipped = true;
    EXPECT_EQ(alice.request(kDesignA, 0x80, &result), VRTD_RET_INVALID_ARGUMENT);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(alice.info().num_holders, 0u);
}

TEST_F(DesignWriteServeTest, RequestOfThePreviousSizeIsRejected) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);

    /* What a client built before the UUID existed sends: dev_number + required_shell. */
    struct __attribute__((packed)) {
        uint32_t dev_number;
        uint8_t required_shell;
    } oldReq = {0, VRTD_SHELL_COMPUTE};
    int pdiFd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(pdiFd, 0);
    uint8_t resp[16] = {};

    EXPECT_EQ(vrtd_raw_request(alice.fd(), VRTD_REQ_DESIGN_WRITE, &oldReq, sizeof(oldReq),
                               resp, sizeof(resp), nullptr, &pdiFd),
              VRTD_RET_BAD_REQUEST);
    close(pdiFd);
    EXPECT_EQ(alice.info().num_holders, 0u);
}

/* ---- Written designs ------------------------------------------------------------- */

TEST_F(DesignWriteServeTest, WrittenDesignIsRecordedAndHeld) {
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.writeFile(pdiPath, kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.num_holders, 1u);
    EXPECT_EQ(hw.amcChecks, 1);
    EXPECT_EQ(hw.parks, 1);
    EXPECT_EQ(hw.refreshes, 1);

    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kDesignA));
    EXPECT_EQ(info.num_holders, 1u);

    /* The next opener of the same vbin writes nothing and shares the design. */
    result = {};
    ASSERT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.num_holders, 2u);
    EXPECT_EQ(hw.parks, 1);
}

TEST_F(DesignWriteServeTest, AcceptedWriteForgetsTheOldDesignAndHoldsTheWriter) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    HeldPdi pdi;
    auto write = std::async(std::launch::async, [&] {
        struct vrtd_design_write_result result {};
        return std::make_pair(alice.writeFd(pdi.readFd(), kDesignB, 0, &result), result);
    });
    if (!waitForWriteInFlight()) {
        /* Let the transfer end so the pending request can return. */
        pdi.release();
        FAIL() << "the design write never went in flight";
    }

    /* While B is being written the card holds neither A nor B. */
    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 1u);

    /* Another client asking for the same design does not wait: it is refused. */
    struct vrtd_design_write_result bobResult {};
    EXPECT_EQ(bob.request(kDesignB, 0, &bobResult), VRTD_RET_BUSY);
    /* And the old design is no longer there to be skipped. */
    EXPECT_EQ(bob.request(kDesignA, 0, &bobResult), VRTD_RET_BUSY);

    pdi.release();
    auto [ret, result] = write.get();
    EXPECT_EQ(ret, VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);
    EXPECT_TRUE(isUuid(bob.info(), kDesignB));
}

TEST_F(DesignWriteServeTest, MorePdisRecordNothingUntilTheLast) {
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.writeFile(pdiPath, kDesignA, VRTD_DESIGN_WRITE_MORE, &result), VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);

    /* Half a design is not the design. */
    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 1u);
    EXPECT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_BUSY);

    result = {};
    ASSERT_EQ(alice.writeFile(pdiPath, kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);
    EXPECT_TRUE(isUuid(bob.info(), kDesignA));
}

TEST_F(DesignWriteServeTest, Pf2RefreshFailureLeavesTheDesignUnknown) {
    hw.refreshResult = VRTD_RET_INTERNAL_ERROR;
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    struct vrtd_design_write_result result {};
    EXPECT_EQ(alice.writeFile(pdiPath, kDesignA, 0, &result), VRTD_RET_INTERNAL_ERROR);
    EXPECT_EQ(hw.refreshes, 1);

    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    /* The transfer went through: the writer holds whatever is on the card. */
    EXPECT_EQ(info.num_holders, 1u);
    EXPECT_FALSE(onServer([this] { return dev->needs_reset; }));

    /* The same vbin is written again, not skipped. */
    hw.refreshResult = VRTD_RET_OK;
    result = {};
    ASSERT_EQ(alice.writeFile(pdiPath, kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);
    EXPECT_TRUE(isUuid(bob.info(), kDesignA));
}

TEST_F(DesignWriteServeTest, FailedTransferLeavesTheDesignUnknownAndUnheld) {
    preload(kDesignB);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    /* Reading a directory fails inside the design writer. */
    struct vrtd_design_write_result result {};
    EXPECT_EQ(alice.writeFile(tmpDir.string(), kDesignA, 0, &result), VRTD_RET_INTERNAL_ERROR);
    EXPECT_EQ(hw.refreshes, 0);

    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 0u);
    EXPECT_TRUE(onServer([this] { return dev->needs_reset; }));

    /* Nobody holds the broken design: Bob is refused for the reset, not for Alice. */
    EXPECT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_DEVICE_UNHEALTHY);
}

/* ---- References and the clock ------------------------------------------------------ */

TEST_F(DesignWriteServeTest, DesignInUseIsNotReplaced) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    ASSERT_GE(alice.fd(), 0);
    ASSERT_GE(bob.fd(), 0);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
    ASSERT_TRUE(result.skipped);

    /* Another design, the same design forced, a design without UUID: all refused. */
    result.skipped = true;
    EXPECT_EQ(bob.request(kDesignB, 0, &result), VRTD_RET_BUSY);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(bob.request(kDesignA, VRTD_DESIGN_WRITE_FORCE, &result), VRTD_RET_BUSY);
    EXPECT_EQ(bob.request(nullptr, 0, &result), VRTD_RET_BUSY);

    /* Nothing changed: Alice's design is still loaded and Bob holds nothing. */
    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kDesignA));
    EXPECT_EQ(info.num_holders, 1u);
    EXPECT_EQ(hw.parks, 0);
}

TEST_F(DesignWriteServeTest, ClockIsParkedWhenTheLastHolderLeaves) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    Conn observer(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);
    ASSERT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_OK);
    ASSERT_EQ(result.num_holders, 2u);

    alice.disconnect();
    ASSERT_TRUE(waitOnServer([this] { return dev->design.holders.len == 1; }));
    /* Bob still runs the design: its clock stays, and the observer cannot replace it. */
    EXPECT_EQ(hw.parks, 0);
    EXPECT_EQ(observer.request(kDesignB, 0, &result), VRTD_RET_BUSY);

    bob.disconnect();
    ASSERT_TRUE(waitOnServer([this] { return dev->design.holders.len == 0; }));
    EXPECT_EQ(hw.parks, 1);

    /* The design stays loaded and known after its last user left. */
    struct vrtd_design_info info = observer.info();
    EXPECT_TRUE(isUuid(info, kDesignA));
    result = {};
    EXPECT_EQ(observer.request(kDesignA, 0, &result), VRTD_RET_OK);
    EXPECT_TRUE(result.skipped);
    /* A sole holder after a park: the client sets the clock again. */
    EXPECT_EQ(result.num_holders, 1u);
}

TEST_F(DesignWriteServeTest, ReleaseWaitsForAnAsyncDeviceOp) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);

    /* A reset or cfgmem job owns the device list. */
    deferredPaused = true;
    onServer([this] { state.async_device_op_in_progress = true; });
    const size_t clients = onServer([this] { return state.clients.len; });

    alice.disconnect();
    ASSERT_TRUE(waitOnServer([&] { return state.clients.len == clients - 1; }));

    /* The client is gone, but its reference stays until the job is done. */
    EXPECT_EQ(holdersOnServer(), 1u);
    EXPECT_EQ(onServer([this] { return state.deferred_cleanup_conn_ids.len; }), 1u);
    EXPECT_EQ(hw.parks, 0);

    deferredPaused = false;
    ASSERT_TRUE(waitOnServer([this] {
        return !state.async_device_op_in_progress && dev->design.holders.len == 0;
    }));
    EXPECT_EQ(onServer([this] { return state.deferred_cleanup_conn_ids.len; }), 0u);
    EXPECT_EQ(hw.parks, 1);
}

/* ---- Forgetting the loaded design ---------------------------------------------------- */

TEST_F(DesignWriteServeTest, ForgetRequestForgetsAndKeepsHolders) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    struct vrtd_design_write_result result {};
    ASSERT_EQ(alice.request(kDesignA, 0, &result), VRTD_RET_OK);

    EXPECT_EQ(bob.forget(), VRTD_RET_OK);

    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 1u);
    /* Not skipped any more, and still Alice's. */
    EXPECT_EQ(bob.request(kDesignA, 0, &result), VRTD_RET_BUSY);
    EXPECT_EQ(hw.parks, 0);
}

TEST_F(DesignWriteServeTest, ForgetDuringAWriteMakesItRecordNothing) {
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);

    HeldPdi pdi;
    auto write = std::async(std::launch::async, [&] {
        struct vrtd_design_write_result result {};
        return std::make_pair(alice.writeFd(pdi.readFd(), kDesignA, 0, &result), result);
    });
    if (!waitForWriteInFlight()) {
        /* Let the transfer end so the pending request can return. */
        pdi.release();
        FAIL() << "the design write never went in flight";
    }

    /* An image is loaded over JTAG while the transfer runs. */
    EXPECT_EQ(bob.forget(), VRTD_RET_OK);

    pdi.release();
    auto [ret, result] = write.get();
    EXPECT_EQ(ret, VRTD_RET_OK);
    EXPECT_FALSE(result.skipped);

    struct vrtd_design_info info = bob.info();
    EXPECT_TRUE(isUuid(info, kNull));
    EXPECT_EQ(info.num_holders, 1u);
}

TEST_F(DesignWriteServeTest, RefusedShellStateReportStillForgets) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);

    /* The board is known to be in the compute shell; a report of another one is refused. */
    EXPECT_EQ(vrtd_set_shell_state(alice.fd(), 0, VRTD_SHELL_SERVICE, 1), VRTD_RET_BUSY);

    EXPECT_TRUE(isUuid(alice.info(), kNull));
}

TEST_F(DesignWriteServeTest, HotplugForgetsOnlyAValidRequest) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);

    /* Refused before any hotplug call: the design is still there. */
    EXPECT_EQ(vrtd_device_hotplug_toggle_sbr(alice.fd(), 0, VRTD_DEVICE_HOTPLUG_FUNCTION_ALL),
              VRTD_RET_INVALID_ARGUMENT);
    EXPECT_TRUE(isUuid(alice.info(), kDesignA));
    EXPECT_EQ(vrtd_device_hotplug_toggle_sbr(alice.fd(), 0, 9), VRTD_RET_INVALID_ARGUMENT);
    EXPECT_TRUE(isUuid(alice.info(), kDesignA));

    /* A valid request reaches the (absent) hotplug handle: forgotten first. */
    EXPECT_NE(vrtd_device_hotplug_toggle_sbr(alice.fd(), 0, 0), VRTD_RET_OK);
    EXPECT_TRUE(isUuid(alice.info(), kNull));

    onServer([this] { preload(kDesignA); });
    EXPECT_NE(vrtd_device_hotplug_remove(alice.fd(), 0, VRTD_DEVICE_HOTPLUG_FUNCTION_ALL),
              VRTD_RET_OK);
    EXPECT_TRUE(isUuid(alice.info(), kNull));
}

}  // namespace
