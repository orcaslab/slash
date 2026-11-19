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
 * @file design_state.h
 * @brief The design loaded on a device, and the clients that use it.
 *
 * vrtd writes every design, so it can remember which one a device holds and
 * answer a request for the same design without writing it again.  This module
 * keeps that memory for one device.  It holds no device handles and does no
 * I/O: serve.c reports what happens to the device and acts on the answers.
 *
 * @section design_state_uuid Loaded UUID
 *
 * The linker stamps a random UUID into each vbin.  The state records the UUID
 * of the design the device holds.  The all-zero UUID means unknown.  It is
 * unknown:
 *
 *   - in a fresh state (daemon start, device object rebuilt after a reset),
 *   - from the moment a design write is accepted until it completes, so a
 *     failed or interrupted write leaves it unknown,
 *   - after a write of a design that carries no UUID,
 *   - after anything that may have changed the fabric outside a design write
 *     (@c design_state_forget).
 *
 * "Completes" means that the transfer and the PF2 refresh succeeded.  vrtd
 * does not check that the card applied the PDI.
 *
 * A forget can arrive while a design is being written: between the accept
 * and the completion of a transfer, or between two PDIs of one design.  The
 * fabric then holds neither the old design nor a clean copy of the new one,
 * so the completion of that design records nothing.
 *
 * @section design_state_holders Holders
 *
 * A client connection holds a reference on the loaded design when its request
 * wrote the design or was skipped because the design was already loaded.  It
 * holds the reference until it disconnects.  While a connection holds a
 * reference no other connection may replace the design with a design write.
 * When the last holder leaves, the caller parks the design's clock.
 *
 * The references guard against design writes only.  A reset, a cfgmem
 * program and a hotplug operation are administrative requests: they are not
 * refused because a design is held.  A reset rebuilds the device object, and
 * the new object starts with no holders.
 */

#ifndef VRTD_DESIGN_STATE_H
#define VRTD_DESIGN_STATE_H

#include "array.h"
#include "utils.h"
#include "vrtd/wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Size of a UUID in its canonical text form, including the terminating NUL. */
#define DESIGN_UUID_STR_LEN 37

/**
 * @brief What to do with a design write request.
 */
enum design_request_action {
    /** Write the design. */
    DESIGN_REQUEST_WRITE,
    /** The device already holds the design: answer success without writing. */
    DESIGN_REQUEST_SKIP,
    /** Another connection holds the loaded design: refuse. */
    DESIGN_REQUEST_BUSY,
};

/**
 * @brief Loaded design of one device.
 *
 * The zero value is a valid state: no known design, no holders.
 */
struct design_state {
    /** @brief UUID of the loaded design; all-zero if unknown. */
    uint8_t uuid[VRTD_DESIGN_UUID_LEN];
    /** @brief Connection ids that hold a reference on the loaded design (owning). */
    struct uint64_array holders;
    /** @brief A design is being written: a write was accepted and the last
     *  PDI of its design has not completed yet. */
    bool writing;
    /** @brief A forget arrived while @c writing: the design being written is
     *  not recorded when it completes. */
    bool poisoned;
};

/**
 * @brief Return a state with no known design and no holders.
 */
static inline
struct design_state design_state_init(void)
{
    return (struct design_state) {
        .uuid = {0},
        .holders = uint64_array_init(),
        .writing = false,
        .poisoned = false,
    };
}

/**
 * @brief Release the memory owned by a state and reset it.
 * @param ds The state to clean up. May be NULL (no-op).
 */
void cleanup_design_state(struct design_state *ds);

/**
 * @brief Tell whether a UUID is the null UUID.
 * @param uuid The UUID to test.
 * @return true if all bytes are zero.
 */
bool design_uuid_is_null(const uint8_t uuid[VRTD_DESIGN_UUID_LEN]);

/**
 * @brief Format a UUID in its canonical text form (8-4-4-4-12 lowercase hex).
 * @param uuid The UUID to format.
 * @param out  Receives the NUL-terminated text.
 */
void design_uuid_to_string(
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    char out[DESIGN_UUID_STR_LEN]
);

/**
 * @brief Tell whether a connection holds a reference on the loaded design.
 * @param ds      The device's design state.
 * @param conn_id The connection to look for.
 * @return true if @p conn_id is a holder.
 */
bool design_state_holds(const struct design_state *ds, uint64_t conn_id);

/**
 * @brief Decide what to do with a design write request.
 *
 * The request is skipped when the device is ready, the request does not force
 * a write and its UUID is non-null and equal to the loaded UUID.  Any other
 * request replaces the loaded design, which is refused while a connection
 * other than the requester holds a reference.  A requester that is the only
 * holder may replace its own design.
 *
 * @param ds            The device's design state.
 * @param conn_id       The requesting connection.
 * @param uuid          UUID carried by the request.
 * @param force         The request asks for a write even if the UUID matches.
 * @param device_ready  The device can be used as it is: no shell switch is
 *                      required and it does not need a reset.
 * @param[out] holder_out  Set to a holder other than the requester when the
 *                      result is DESIGN_REQUEST_BUSY. May be NULL.
 * @return The action to take.
 */
enum design_request_action design_state_decide(
    const struct design_state *ds,
    uint64_t conn_id,
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    bool force,
    bool device_ready,
    uint64_t *holder_out
);

/**
 * @brief Give a connection a reference on the loaded design.
 *
 * Called for a request that was skipped.  A connection holds at most one
 * reference; holding again changes nothing.
 *
 * @param ds      The device's design state.
 * @param conn_id The connection that now uses the design.
 * @return 0 on success, -1 on allocation failure.
 */
NODISCARD
int design_state_hold(struct design_state *ds, uint64_t conn_id);

/**
 * @brief Record that a design write was accepted for transfer.
 *
 * The loaded UUID becomes unknown and the requester becomes the holder of
 * whatever the write leaves on the device.  Fails without changing the state
 * if another connection holds a reference.
 *
 * The write opens a design, or continues the one that is open: a design stays
 * open until a write without more PDIs to follow completes, a write fails, or
 * its holder leaves.
 *
 * @param ds      The device's design state.
 * @param conn_id The requesting connection.
 * @return 0 on success, -1 if another connection holds the design or on
 *         allocation failure.
 */
NODISCARD
int design_state_write_begin(struct design_state *ds, uint64_t conn_id);

/**
 * @brief Record that a design write completed successfully.
 *
 * The loaded UUID becomes @p uuid, unless more PDIs of the design follow:
 * the design is complete only when its last PDI is written.  If a forget
 * arrived while the design was being written, nothing is recorded.
 *
 * @param ds           The device's design state.
 * @param uuid         UUID carried by the request; null leaves it unknown.
 * @param more_follow  More PDIs of this design follow in later requests.
 */
void design_state_write_completed(
    struct design_state *ds,
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    bool more_follow
);

/**
 * @brief Record that a design write ended but its result cannot be named.
 *
 * The transfer finished but a later step of the write failed (the PF2
 * refresh).  The UUID stays unknown and the design is closed; the requester
 * keeps its reference.
 *
 * @param ds The device's design state.
 */
void design_state_write_unconfirmed(struct design_state *ds);

/**
 * @brief Record that a design write failed.
 *
 * The device holds no usable design: the UUID is unknown and nobody holds a
 * reference.
 *
 * @param ds The device's design state.
 */
void design_state_write_failed(struct design_state *ds);

/**
 * @brief Forget the loaded UUID.
 *
 * Called when the fabric may have changed outside a design write (bus reset,
 * hotplug, cfgmem programming, an image loaded over JTAG).  Holders keep
 * their references: they are still connected and the clock of whatever runs
 * is still theirs.
 *
 * A design that is being written is poisoned: its completion will not record
 * a UUID.
 *
 * @param ds The device's design state.
 */
void design_state_forget(struct design_state *ds);

/**
 * @brief Drop the reference of a connection that is going away.
 *
 * A design the connection was writing is abandoned: the next write starts a
 * new one.
 *
 * @param ds      The device's design state.
 * @param conn_id The connection that disconnected.
 * @return true if the connection held a reference and it was the last one.
 */
bool design_state_release(struct design_state *ds, uint64_t conn_id);

#endif /* VRTD_DESIGN_STATE_H */
