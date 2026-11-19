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
 * The test runs on_event_new_connection() and on_client_io() on an sd_event
 * loop in this process, against a mock device and a private socket, and talks
 * to it with libvrtd.  It never connects to a running vrtd.
 *
 * Only requests that the daemon answers before it touches the card are sent:
 * skipped writes, refused writes, malformed writes and queries.  A request
 * that is accepted for writing goes on to check the AMC of a real board, so
 * the loaded design is put in place directly instead of being written.  The
 * rules themselves are covered by design_state_test.cpp.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

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
#include "serve.h"
#include "state.h"
}

namespace {

const uint8_t kNull[VRTD_DESIGN_UUID_LEN] = {0};
const uint8_t kDesignA[VRTD_DESIGN_UUID_LEN] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};
const uint8_t kDesignB[VRTD_DESIGN_UUID_LEN] = {
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
};

/* One client connection to the in-process daemon. */
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

    enum vrtd_ret write(const uint8_t* uuid, uint8_t flags, bool* skipped) const {
        return vrtd_design_write_file(fd_, 0, "/dev/null", VRTD_SHELL_COMPUTE, uuid, flags,
                                      skipped);
    }

    struct vrtd_design_info info() const {
        struct vrtd_design_info info;
        std::memset(&info, 0xff, sizeof(info));
        EXPECT_EQ(vrtd_get_design_info(fd_, 0, &info), VRTD_RET_OK);
        return info;
    }

   private:
    int fd_;
};

size_t countOpenFds() {
    size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        n++;
    }
    return n;
}

class DesignWriteServeTest : public ::testing::Test {
   protected:
    std::filesystem::path tmpDir;
    std::string sockPath;

    struct config cfg {};
    struct user_config defaultUser {};
    struct vrtd state {};
    struct device* dev = nullptr; /* owned by state.devices */

    int listenFd = -1;
    std::thread server;
    std::atomic<bool> stop{false};

    void SetUp() override {
        /* Every user may do everything on every device. */
        struct role* role = nullptr;
        ASSERT_EQ(role_merge_new(&role, "fullaccess"), 0);
        role->query = true;
        auto* dp = static_cast<struct device_policy*>(calloc(1, sizeof(struct device_policy)));
        ASSERT_NE(dp, nullptr);
        dp->bdf = strdup("any");
        dp->design_write = true;
        ASSERT_EQ(device_policy_ptr_array_push_move(&role->device_policies, &dp), 0);
        ASSERT_EQ(role_ref_array_push(&defaultUser.roles, role), 0);
        ASSERT_EQ(role_ptr_array_push_move(&cfg.roles, &role), 0);
        defaultUser.name = strdup("*");
        cfg.default_user = &defaultUser;
        state.config = &cfg;

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

    void serve(std::promise<bool>& ready) {
        sd_event* ev = nullptr;
        sd_event_source* listener = nullptr;
        bool ok = sd_event_new(&ev) >= 0 &&
                  sd_event_add_io(ev, &listener, listenFd, EPOLLIN, on_event_new_connection,
                                  &state) >= 0;
        ready.set_value(ok);

        while (ok && !stop) {
            (void)sd_event_run(ev, 10 * 1000);
        }

        /* Clients go before the devices they hold designs on. */
        client_ptr_array_free(&state.clients);
        sd_event_source_disable_unref(listener);
        sd_event_unref(ev);
    }

    /* Clients that disconnect are noticed by the event loop a moment later. */
    bool waitForHolders(const Conn& observer, uint32_t expected) {
        for (int i = 0; i < 500; i++) {
            if (observer.info().num_holders == expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    void TearDown() override {
        stop = true;
        if (server.joinable()) {
            server.join();
        }
        if (listenFd >= 0) {
            close(listenFd);
        }

        device_ptr_array_free(&state.devices);
        role_ptr_array_free(&cfg.roles);
        role_ref_array_free(&defaultUser.roles);
        str_array_free(&defaultUser.role_names);
        free(defaultUser.name);

        std::filesystem::remove_all(tmpDir);
    }
};

TEST_F(DesignWriteServeTest, UnknownDesignIsReportedAsNull) {
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    struct vrtd_design_info info = alice.info();

    EXPECT_EQ(std::memcmp(info.uuid, kNull, sizeof(kNull)), 0);
    EXPECT_EQ(info.num_holders, 0u);
}

TEST_F(DesignWriteServeTest, LoadedDesignIsSkippedAndHeld) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    ASSERT_GE(alice.fd(), 0);
    ASSERT_GE(bob.fd(), 0);

    bool skipped = false;
    EXPECT_EQ(alice.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    EXPECT_TRUE(skipped);

    struct vrtd_design_info info = alice.info();
    EXPECT_EQ(std::memcmp(info.uuid, kDesignA, sizeof(kDesignA)), 0);
    EXPECT_EQ(info.num_holders, 1u);

    skipped = false;
    EXPECT_EQ(bob.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    EXPECT_TRUE(skipped);
    EXPECT_EQ(bob.info().num_holders, 2u);

    /* A second request of the same client does not count twice. */
    skipped = false;
    EXPECT_EQ(alice.write(kDesignA, VRTD_DESIGN_WRITE_MORE, &skipped), VRTD_RET_OK);
    EXPECT_TRUE(skipped);
    EXPECT_EQ(alice.info().num_holders, 2u);
}

TEST_F(DesignWriteServeTest, DesignInUseIsNotReplaced) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    ASSERT_GE(alice.fd(), 0);
    ASSERT_GE(bob.fd(), 0);

    bool skipped = false;
    ASSERT_EQ(alice.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    ASSERT_TRUE(skipped);

    /* Another design, the same design forced, a design without UUID: all refused. */
    skipped = true;
    EXPECT_EQ(bob.write(kDesignB, 0, &skipped), VRTD_RET_BUSY);
    EXPECT_FALSE(skipped);
    EXPECT_EQ(bob.write(kDesignA, VRTD_DESIGN_WRITE_FORCE, &skipped), VRTD_RET_BUSY);
    EXPECT_EQ(bob.write(nullptr, 0, &skipped), VRTD_RET_BUSY);

    /* Nothing changed: Alice's design is still loaded and Bob holds nothing. */
    struct vrtd_design_info info = bob.info();
    EXPECT_EQ(std::memcmp(info.uuid, kDesignA, sizeof(kDesignA)), 0);
    EXPECT_EQ(info.num_holders, 1u);
}

TEST_F(DesignWriteServeTest, ReferenceLastsUntilTheConnectionCloses) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    Conn bob(sockPath);
    Conn observer(sockPath);
    ASSERT_GE(alice.fd(), 0);
    ASSERT_GE(bob.fd(), 0);
    ASSERT_GE(observer.fd(), 0);

    bool skipped = false;
    ASSERT_EQ(alice.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    ASSERT_EQ(bob.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    ASSERT_EQ(observer.info().num_holders, 2u);

    alice.disconnect();
    EXPECT_TRUE(waitForHolders(observer, 1));
    /* Bob still uses the design: the observer cannot replace it. */
    EXPECT_EQ(observer.write(kDesignB, 0, &skipped), VRTD_RET_BUSY);

    bob.disconnect();
    EXPECT_TRUE(waitForHolders(observer, 0));

    /* The design stays loaded and known after its last user left. */
    struct vrtd_design_info info = observer.info();
    EXPECT_EQ(std::memcmp(info.uuid, kDesignA, sizeof(kDesignA)), 0);
    skipped = false;
    EXPECT_EQ(observer.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    EXPECT_TRUE(skipped);
}

TEST_F(DesignWriteServeTest, UnknownFlagsAreRejected) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    bool skipped = true;
    EXPECT_EQ(alice.write(kDesignA, 0x80, &skipped), VRTD_RET_INVALID_ARGUMENT);
    EXPECT_FALSE(skipped);
    EXPECT_EQ(alice.info().num_holders, 0u);
}

TEST_F(DesignWriteServeTest, SkippedWriteClosesTheReceivedFd) {
    preload(kDesignA);
    startServer();
    Conn alice(sockPath);
    ASSERT_GE(alice.fd(), 0);

    bool skipped = false;
    ASSERT_EQ(alice.write(kDesignA, 0, &skipped), VRTD_RET_OK);
    ASSERT_TRUE(skipped);

    /* Client and daemon share this process: a PDI fd the daemon kept would show. */
    const size_t before = countOpenFds();
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ(alice.write(kDesignA, 0, &skipped), VRTD_RET_OK);
        ASSERT_TRUE(skipped);
    }
    EXPECT_EQ(countOpenFds(), before);
}

}  // namespace
