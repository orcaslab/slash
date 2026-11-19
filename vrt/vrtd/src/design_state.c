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
 * @file design_state.c
 * @brief The design loaded on a device, and the clients that use it.
 *
 * See design_state.h for the rules.  Everything here runs on the event-loop
 * thread; the state needs no locking.
 */

#define _GNU_SOURCE

#include "design_state.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

void cleanup_design_state(struct design_state *ds)
{
    if (ds == NULL) {
        return;
    }

    uint64_array_free(&ds->holders);
    memset(ds->uuid, 0, sizeof(ds->uuid));
    ds->writing = false;
    ds->poisoned = false;
}

bool design_uuid_is_null(const uint8_t uuid[VRTD_DESIGN_UUID_LEN])
{
    for (size_t i = 0; i < VRTD_DESIGN_UUID_LEN; i++) {
        if (uuid[i] != 0) {
            return false;
        }
    }

    return true;
}

void design_uuid_to_string(
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    char out[DESIGN_UUID_STR_LEN]
)
{
    (void) snprintf(
        out, DESIGN_UUID_STR_LEN,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5],
        uuid[6], uuid[7],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
    );
}

bool design_state_holds(const struct design_state *ds, uint64_t conn_id)
{
    for (size_t i = 0; i < ds->holders.len; i++) {
        if (ds->holders.d[i] == conn_id) {
            return true;
        }
    }

    return false;
}

/* Find a holder other than conn_id. */
static bool design_state_other_holder(
    const struct design_state *ds,
    uint64_t conn_id,
    uint64_t *holder_out
)
{
    for (size_t i = 0; i < ds->holders.len; i++) {
        if (ds->holders.d[i] != conn_id) {
            if (holder_out != NULL) {
                *holder_out = ds->holders.d[i];
            }
            return true;
        }
    }

    return false;
}

enum design_request_action design_state_decide(
    const struct design_state *ds,
    uint64_t conn_id,
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    bool force,
    bool device_ready,
    uint64_t *holder_out
)
{
    bool loaded =
        !design_uuid_is_null(uuid) &&
        memcmp(uuid, ds->uuid, VRTD_DESIGN_UUID_LEN) == 0;

    if (loaded && !force && device_ready) {
        return DESIGN_REQUEST_SKIP;
    }

    if (design_state_other_holder(ds, conn_id, holder_out)) {
        return DESIGN_REQUEST_BUSY;
    }

    return DESIGN_REQUEST_WRITE;
}

int design_state_hold(struct design_state *ds, uint64_t conn_id)
{
    if (design_state_holds(ds, conn_id)) {
        return 0;
    }

    return uint64_array_push(&ds->holders, conn_id);
}

int design_state_write_begin(struct design_state *ds, uint64_t conn_id)
{
    if (design_state_other_holder(ds, conn_id, NULL)) {
        errno = EBUSY;
        return -1;
    }

    int ret = design_state_hold(ds, conn_id);
    PROPAGATE_ERROR(ret);

    memset(ds->uuid, 0, sizeof(ds->uuid));

    /* A further PDI of the design that is open inherits its poison. */
    if (!ds->writing) {
        ds->writing = true;
        ds->poisoned = false;
    }

    return 0;
}

void design_state_write_completed(
    struct design_state *ds,
    const uint8_t uuid[VRTD_DESIGN_UUID_LEN],
    bool more_follow
)
{
    if (more_follow) {
        return;
    }

    if (!ds->poisoned) {
        memcpy(ds->uuid, uuid, sizeof(ds->uuid));
    }

    ds->writing = false;
    ds->poisoned = false;
}

void design_state_write_unconfirmed(struct design_state *ds)
{
    memset(ds->uuid, 0, sizeof(ds->uuid));
    ds->writing = false;
    ds->poisoned = false;
}

void design_state_write_failed(struct design_state *ds)
{
    memset(ds->uuid, 0, sizeof(ds->uuid));
    ds->holders.len = 0;
    ds->writing = false;
    ds->poisoned = false;
}

void design_state_forget(struct design_state *ds)
{
    memset(ds->uuid, 0, sizeof(ds->uuid));

    if (ds->writing) {
        ds->poisoned = true;
    }
}

bool design_state_release(struct design_state *ds, uint64_t conn_id)
{
    if (!design_state_holds(ds, conn_id)) {
        return false;
    }

    uint64_array_rm_by_value(&ds->holders, conn_id);

    if (ds->holders.len != 0) {
        return false;
    }

    /* Only the writer holds a design that is being written; it is gone. */
    ds->writing = false;
    ds->poisoned = false;

    return true;
}
