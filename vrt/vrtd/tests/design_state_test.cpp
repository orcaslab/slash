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

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

extern "C" {
#include "design_state.h"
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

const uint64_t kAlice = 7;
const uint64_t kBob = 8;
const uint64_t kCarol = 9;

class DesignStateTest : public ::testing::Test {
   protected:
    struct design_state ds = design_state_init();

    void TearDown() override { cleanup_design_state(&ds); }

    bool loadedIs(const uint8_t uuid[VRTD_DESIGN_UUID_LEN]) const {
        return std::memcmp(ds.uuid, uuid, VRTD_DESIGN_UUID_LEN) == 0;
    }

    enum design_request_action decide(uint64_t conn, const uint8_t* uuid, bool force = false,
                                      bool deviceReady = true, uint64_t* holder = nullptr) const {
        return design_state_decide(&ds, conn, uuid, force, deviceReady, holder);
    }

    /* A complete, successful single-PDI write of @p uuid by @p conn. */
    void write(uint64_t conn, const uint8_t* uuid) {
        ASSERT_EQ(decide(conn, uuid), DESIGN_REQUEST_WRITE);
        ASSERT_EQ(design_state_write_begin(&ds, conn), 0);
        design_state_write_completed(&ds, uuid, false);
    }

    /* A request of @p conn for the loaded design @p uuid that is skipped. */
    void skip(uint64_t conn, const uint8_t* uuid) {
        ASSERT_EQ(decide(conn, uuid), DESIGN_REQUEST_SKIP);
        ASSERT_EQ(design_state_hold(&ds, conn), 0);
    }
};

/* ---- UUID helpers --------------------------------------------------------- */

TEST(DesignUuidTest, NullUuidIsAllZero) {
    EXPECT_TRUE(design_uuid_is_null(kNull));
    EXPECT_FALSE(design_uuid_is_null(kDesignA));

    uint8_t lastByteSet[VRTD_DESIGN_UUID_LEN] = {0};
    lastByteSet[VRTD_DESIGN_UUID_LEN - 1] = 1;
    EXPECT_FALSE(design_uuid_is_null(lastByteSet));
}

TEST(DesignUuidTest, FormatsCanonicalText) {
    char text[DESIGN_UUID_STR_LEN];

    design_uuid_to_string(kDesignA, text);
    EXPECT_STREQ(text, "12345678-9abc-def0-0123-456789abcdef");

    design_uuid_to_string(kNull, text);
    EXPECT_STREQ(text, "00000000-0000-0000-0000-000000000000");
}

/* ---- Unknown at start ----------------------------------------------------- */

TEST_F(DesignStateTest, FreshStateKnowsNoDesignAndHasNoHolders) {
    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(ds.holders.len, 0u);
    EXPECT_FALSE(design_state_holds(&ds, kAlice));
}

TEST_F(DesignStateTest, ZeroedStateIsAFreshState) {
    /* struct device is calloc()ed; the all-zero state must be usable as is. */
    struct design_state zeroed;
    std::memset(&zeroed, 0, sizeof(zeroed));

    EXPECT_TRUE(design_uuid_is_null(zeroed.uuid));
    EXPECT_EQ(design_state_decide(&zeroed, kAlice, kDesignA, false, true, nullptr),
              DESIGN_REQUEST_WRITE);
    ASSERT_EQ(design_state_hold(&zeroed, kAlice), 0);
    EXPECT_TRUE(design_state_holds(&zeroed, kAlice));
    cleanup_design_state(&zeroed);
}

TEST_F(DesignStateTest, UnknownDesignIsAlwaysWritten) {
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, NullRequestNeverMatchesUnknownDesign) {
    /* Both sides are all-zero, but "no identity" is not an identity. */
    EXPECT_EQ(decide(kAlice, kNull), DESIGN_REQUEST_WRITE);
}

/* ---- Write: nulled when accepted, set when completed ----------------------- */

TEST_F(DesignStateTest, AcceptedWriteForgetsLoadedUuidBeforeTransfer) {
    write(kAlice, kDesignA);
    ASSERT_TRUE(loadedIs(kDesignA));

    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
}

TEST_F(DesignStateTest, CompletedWriteRecordsRequestUuid) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, false);

    EXPECT_TRUE(loadedIs(kDesignA));
}

TEST_F(DesignStateTest, CompletedWriteOfNullUuidLeavesDesignUnknown) {
    write(kAlice, kDesignA);

    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kNull, false);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(decide(kAlice, kNull), DESIGN_REQUEST_WRITE);
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, WriterHoldsTheDesignItWrote) {
    write(kAlice, kDesignA);

    EXPECT_TRUE(design_state_holds(&ds, kAlice));
    EXPECT_EQ(ds.holders.len, 1u);
}

TEST_F(DesignStateTest, FailedWriteLeavesDesignUnknownAndUnheld) {
    write(kAlice, kDesignA);

    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_failed(&ds);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(ds.holders.len, 0u);
    /* The design that was there before the failed write is not skipped. */
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
    EXPECT_EQ(decide(kBob, kDesignA), DESIGN_REQUEST_WRITE);
    /* Nobody is left whose departure would park the clock. */
    EXPECT_FALSE(design_state_release(&ds, kAlice));
}

TEST_F(DesignStateTest, InterruptedWriteLeavesDesignUnknown) {
    write(kAlice, kDesignA);

    /* Alice starts replacing her design and disconnects mid-transfer. */
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    EXPECT_TRUE(design_state_release(&ds, kAlice));

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(decide(kBob, kDesignA), DESIGN_REQUEST_WRITE);
}

/* ---- Skip ----------------------------------------------------------------- */

TEST_F(DesignStateTest, SameDesignIsSkipped) {
    write(kAlice, kDesignA);

    EXPECT_EQ(decide(kBob, kDesignA), DESIGN_REQUEST_SKIP);
    /* Also for the client that wrote it, and after it has gone. */
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_SKIP);
    EXPECT_TRUE(design_state_release(&ds, kAlice));
    EXPECT_EQ(decide(kCarol, kDesignA), DESIGN_REQUEST_SKIP);
}

TEST_F(DesignStateTest, SkipDoesNotChangeLoadedUuid) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    EXPECT_TRUE(loadedIs(kDesignA));
}

TEST_F(DesignStateTest, DifferentDesignIsNotSkipped) {
    write(kAlice, kDesignA);
    ASSERT_TRUE(design_state_release(&ds, kAlice));

    EXPECT_EQ(decide(kBob, kDesignB), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, NoSkipWhenDeviceIsNotReady) {
    write(kAlice, kDesignA);

    /* Shell switch required or card needs a reset: the matching UUID does not count. */
    EXPECT_EQ(decide(kAlice, kDesignA, false, false), DESIGN_REQUEST_WRITE);

    uint64_t holder = 0;
    EXPECT_EQ(decide(kBob, kDesignA, false, false, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);
}

/* ---- References ----------------------------------------------------------- */

TEST_F(DesignStateTest, SkippedClientHoldsAReference) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    EXPECT_TRUE(design_state_holds(&ds, kAlice));
    EXPECT_TRUE(design_state_holds(&ds, kBob));
    EXPECT_EQ(ds.holders.len, 2u);
}

TEST_F(DesignStateTest, HoldingTwiceCountsOnce) {
    write(kAlice, kDesignA);
    skip(kAlice, kDesignA);
    skip(kBob, kDesignA);
    skip(kBob, kDesignA);

    EXPECT_EQ(ds.holders.len, 2u);
    EXPECT_FALSE(design_state_release(&ds, kBob));
    EXPECT_FALSE(design_state_holds(&ds, kBob));
}

TEST_F(DesignStateTest, ClockIsParkedOnlyWhenTheLastHolderLeaves) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    /* The writer leaves first: Bob still uses the design. */
    EXPECT_FALSE(design_state_release(&ds, kAlice));
    EXPECT_EQ(ds.holders.len, 1u);
    EXPECT_TRUE(loadedIs(kDesignA));

    /* The second holder leaves: now the caller parks the clock. */
    EXPECT_TRUE(design_state_release(&ds, kBob));
    EXPECT_EQ(ds.holders.len, 0u);
}

TEST_F(DesignStateTest, LastHolderLeavingKeepsTheDesignKnown) {
    write(kAlice, kDesignA);
    ASSERT_TRUE(design_state_release(&ds, kAlice));

    /* The design is still on the device, only its clock is parked. */
    EXPECT_TRUE(loadedIs(kDesignA));
    skip(kBob, kDesignA);
    EXPECT_TRUE(design_state_release(&ds, kBob));
}

TEST_F(DesignStateTest, ReleasingAConnectionThatHoldsNothingParksNothing) {
    EXPECT_FALSE(design_state_release(&ds, kCarol));

    write(kAlice, kDesignA);
    EXPECT_FALSE(design_state_release(&ds, kCarol));
    EXPECT_EQ(ds.holders.len, 1u);

    ASSERT_TRUE(design_state_release(&ds, kAlice));
    /* Alice is gone already; a second release must not report "last" again. */
    EXPECT_FALSE(design_state_release(&ds, kAlice));
}

/* ---- In use --------------------------------------------------------------- */

TEST_F(DesignStateTest, ReplacingADesignAnotherClientHoldsIsRefused) {
    write(kAlice, kDesignA);

    uint64_t holder = 0;
    EXPECT_EQ(decide(kBob, kDesignB, false, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);

    /* A design without a UUID replaces the loaded one just the same. */
    holder = 0;
    EXPECT_EQ(decide(kBob, kNull, false, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);
}

TEST_F(DesignStateTest, RefusedRequestChangesNothing) {
    write(kAlice, kDesignA);

    ASSERT_EQ(decide(kBob, kDesignB), DESIGN_REQUEST_BUSY);
    errno = 0;
    EXPECT_EQ(design_state_write_begin(&ds, kBob), -1);
    EXPECT_EQ(errno, EBUSY);

    EXPECT_TRUE(loadedIs(kDesignA));
    EXPECT_EQ(ds.holders.len, 1u);
    EXPECT_FALSE(design_state_holds(&ds, kBob));
}

TEST_F(DesignStateTest, SoleHolderMayReplaceItsOwnDesign) {
    write(kAlice, kDesignA);

    EXPECT_EQ(decide(kAlice, kDesignB), DESIGN_REQUEST_WRITE);
    write(kAlice, kDesignB);

    EXPECT_TRUE(loadedIs(kDesignB));
    EXPECT_EQ(ds.holders.len, 1u);
    EXPECT_TRUE(design_state_release(&ds, kAlice));
}

TEST_F(DesignStateTest, HolderAmongOthersMayNotReplaceTheDesign) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    uint64_t holder = 0;
    EXPECT_EQ(decide(kAlice, kDesignB, false, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kBob);

    holder = 0;
    EXPECT_EQ(decide(kBob, kDesignB, false, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);
}

TEST_F(DesignStateTest, DesignIsFreeAgainWhenItsHoldersAreGone) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);
    ASSERT_EQ(decide(kCarol, kDesignB), DESIGN_REQUEST_BUSY);

    EXPECT_FALSE(design_state_release(&ds, kAlice));
    EXPECT_EQ(decide(kCarol, kDesignB), DESIGN_REQUEST_BUSY);

    EXPECT_TRUE(design_state_release(&ds, kBob));
    EXPECT_EQ(decide(kCarol, kDesignB), DESIGN_REQUEST_WRITE);
}

/* ---- Force ---------------------------------------------------------------- */

TEST_F(DesignStateTest, ForceWritesALoadedDesignAgain) {
    write(kAlice, kDesignA);

    /* By its only holder ... */
    EXPECT_EQ(decide(kAlice, kDesignA, true), DESIGN_REQUEST_WRITE);

    /* ... or by anyone once nobody holds it. */
    ASSERT_TRUE(design_state_release(&ds, kAlice));
    EXPECT_EQ(decide(kBob, kDesignA, true), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, ForceDoesNotOverrideInUse) {
    write(kAlice, kDesignA);

    uint64_t holder = 0;
    EXPECT_EQ(decide(kBob, kDesignA, true, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);

    /* Without force the same request is simply skipped. */
    EXPECT_EQ(decide(kBob, kDesignA, false), DESIGN_REQUEST_SKIP);
}

TEST_F(DesignStateTest, ForcedWriteGoesThroughUnknown) {
    write(kAlice, kDesignA);

    ASSERT_EQ(decide(kAlice, kDesignA, true), DESIGN_REQUEST_WRITE);
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    EXPECT_TRUE(design_uuid_is_null(ds.uuid));

    design_state_write_completed(&ds, kDesignA, false);
    EXPECT_TRUE(loadedIs(kDesignA));
}

/* ---- Several PDIs in one design ------------------------------------------- */

TEST_F(DesignStateTest, UuidIsRecordedOnlyAfterTheLastPdi) {
    /* First PDI: more follow. */
    ASSERT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, true);
    EXPECT_TRUE(design_uuid_is_null(ds.uuid));

    /* Second PDI, same UUID: not skipped, the design is not complete yet. */
    ASSERT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, false);

    EXPECT_TRUE(loadedIs(kDesignA));
    EXPECT_EQ(ds.holders.len, 1u);
}

TEST_F(DesignStateTest, HalfWrittenDesignIsNeitherSkippedNorReplacedByOthers) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, true);

    /* Between two PDIs Alice holds what is on the device. */
    uint64_t holder = 0;
    EXPECT_EQ(decide(kBob, kDesignA, false, true, &holder), DESIGN_REQUEST_BUSY);
    EXPECT_EQ(holder, kAlice);
}

TEST_F(DesignStateTest, DesignAbandonedBetweenPdisStaysUnknown) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, true);

    EXPECT_TRUE(design_state_release(&ds, kAlice));

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(decide(kBob, kDesignA), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, FirstPdiOfALoadedDesignIsSkipped) {
    write(kAlice, kDesignA);

    /* The skip decision is made on the first request, whatever follows it. */
    EXPECT_EQ(decide(kBob, kDesignA), DESIGN_REQUEST_SKIP);
}

/* ---- Fabric changed outside a design write --------------------------------- */

TEST_F(DesignStateTest, ForgetMakesTheDesignUnknown) {
    write(kAlice, kDesignA);

    design_state_forget(&ds);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, ForgetKeepsHolders) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    design_state_forget(&ds);

    EXPECT_EQ(ds.holders.len, 2u);
    EXPECT_EQ(decide(kCarol, kDesignA), DESIGN_REQUEST_BUSY);
    EXPECT_FALSE(design_state_release(&ds, kAlice));
    EXPECT_TRUE(design_state_release(&ds, kBob));
}

/* ---- Forget while a design is being written --------------------------------- */

TEST_F(DesignStateTest, ForgetDuringAWriteMakesItsCompletionRecordNothing) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);

    /* A hotplug or JTAG load hits the card while the transfer runs. */
    design_state_forget(&ds);
    design_state_write_completed(&ds, kDesignA, false);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(decide(kAlice, kDesignA), DESIGN_REQUEST_WRITE);
    /* The writer still holds what is on the card. */
    EXPECT_TRUE(design_state_holds(&ds, kAlice));
}

TEST_F(DesignStateTest, ForgetBetweenPdisPoisonsTheWholeDesign) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, true);

    design_state_forget(&ds);

    /* The last PDI is written cleanly, but the first one may be gone. */
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, false);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
}

TEST_F(DesignStateTest, PoisonLastsForOneDesignOnly) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_forget(&ds);
    design_state_write_completed(&ds, kDesignA, false);
    ASSERT_TRUE(design_uuid_is_null(ds.uuid));

    /* The next write starts clean and is recorded. */
    write(kAlice, kDesignB);
    EXPECT_TRUE(loadedIs(kDesignB));
}

TEST_F(DesignStateTest, ForgetOutsideAWritePoisonsNothing) {
    design_state_forget(&ds);

    write(kAlice, kDesignA);

    EXPECT_TRUE(loadedIs(kDesignA));
}

TEST_F(DesignStateTest, FailedWriteClearsThePoison) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_forget(&ds);
    design_state_write_failed(&ds);

    write(kBob, kDesignA);

    EXPECT_TRUE(loadedIs(kDesignA));
}

TEST_F(DesignStateTest, WriterLeavingClearsThePoison) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_completed(&ds, kDesignA, true);
    design_state_forget(&ds);
    ASSERT_TRUE(design_state_release(&ds, kAlice));

    write(kBob, kDesignB);

    EXPECT_TRUE(loadedIs(kDesignB));
}

/* ---- Write whose PF2 refresh failed ------------------------------------------ */

TEST_F(DesignStateTest, UnconfirmedWriteLeavesDesignUnknownAndHeld) {
    write(kAlice, kDesignA);

    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_write_unconfirmed(&ds);

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_TRUE(design_state_holds(&ds, kAlice));
    EXPECT_EQ(decide(kAlice, kDesignB), DESIGN_REQUEST_WRITE);
}

TEST_F(DesignStateTest, UnconfirmedWriteClosesTheDesign) {
    ASSERT_EQ(design_state_write_begin(&ds, kAlice), 0);
    design_state_forget(&ds);
    design_state_write_unconfirmed(&ds);

    write(kAlice, kDesignA);

    EXPECT_TRUE(loadedIs(kDesignA));
}

TEST_F(DesignStateTest, CleanupReturnsToAFreshState) {
    write(kAlice, kDesignA);
    skip(kBob, kDesignA);

    /* What rebuilding the device object after a reset amounts to. */
    cleanup_design_state(&ds);
    ds = design_state_init();

    EXPECT_TRUE(design_uuid_is_null(ds.uuid));
    EXPECT_EQ(ds.holders.len, 0u);
    EXPECT_EQ(decide(kCarol, kDesignA), DESIGN_REQUEST_WRITE);
}

TEST(DesignStateCleanupTest, NullIsANoOp) {
    cleanup_design_state(nullptr);
}

}  // namespace
