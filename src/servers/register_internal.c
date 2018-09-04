/*
 * Copyright 2017-2018 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <anjay_config.h>

#include <inttypes.h>

#include <anjay_modules/time_defs.h>

#define ANJAY_SERVERS_INTERNALS

#include "../anjay_core.h"
#include "../servers.h"
#include "../servers_utils.h"
#include "../interface/register.h"

#include "activate.h"
#include "connection_info.h"
#include "reload.h"
#include "register_internal.h"
#include "servers_internal.h"

VISIBILITY_SOURCE_BEGIN

/** Update messages are sent to the server every
 * LIFETIME/ANJAY_UPDATE_INTERVAL_FACTOR seconds. */
#define ANJAY_UPDATE_INTERVAL_MARGIN_FACTOR 2

/** To avoid flooding the network in case of a very small lifetime, Update
 * messages are not sent more often than every ANJAY_MIN_UPDATE_INTERVAL_S
 * seconds. */
#define ANJAY_MIN_UPDATE_INTERVAL_S 1

static void send_update_sched_job(anjay_t *anjay, const void *ssid_ptr) {
    anjay_ssid_t ssid = *(const anjay_ssid_t *) ssid_ptr;
    assert(ssid != ANJAY_SSID_ANY);

    AVS_LIST(anjay_server_info_t) server =
            _anjay_servers_find_active(anjay, ssid);
    if (!server) {
        return;
    }

    if (_anjay_active_server_refresh(anjay, server)) {
        if (!_anjay_server_registration_expired(server)) {
            goto retry;
        }
    } else if (ssid == ANJAY_SSID_BOOTSTRAP) {
        return;
    } else {
        switch (_anjay_server_registration_update(anjay, server)) {
        case ANJAY_UPDATE_SUCCESS:
            return;
        case ANJAY_UPDATE_NEEDS_REGISTRATION:
            break;
        case ANJAY_UPDATE_FAILED:
            goto retry;
        }
    }
    // mark that the registration is expired; prevents superfluous Deregister
    server->data_active.registration_info.expire_time = AVS_TIME_REAL_INVALID;
    _anjay_server_deactivate(anjay, ssid, AVS_TIME_DURATION_ZERO);
    return;
retry:
    if (_anjay_servers_schedule_next_retryable(anjay->sched, server,
                                               send_update_sched_job, ssid)) {
        anjay_log(ERROR, "could not reschedule send_update_sched_job");
    }
}

/**
 * Returns the duration that we should reserve before expiration of lifetime for
 * performing the Update operation.
 */
static avs_time_duration_t
get_server_update_interval_margin(anjay_t *anjay,
                                  const anjay_server_info_t *server) {
    avs_time_duration_t half_lifetime = avs_time_duration_div(
        avs_time_duration_from_scalar(
            server->data_active.registration_info.last_update_params.lifetime_s,
            AVS_TIME_S),
        ANJAY_UPDATE_INTERVAL_MARGIN_FACTOR);
    avs_time_duration_t max_transmit_wait =
            avs_coap_max_transmit_wait(_anjay_tx_params_for_conn_type(
                    anjay, server->data_active.primary_conn_type));
    if (avs_time_duration_less(half_lifetime, max_transmit_wait)) {
        return half_lifetime;
    } else {
        return max_transmit_wait;
    }
}

static int schedule_update(anjay_t *anjay,
                           anjay_server_info_t *server,
                           avs_time_duration_t delay) {
    anjay_log(DEBUG, "scheduling update for SSID %u after "
                     "%" PRId64 ".%09" PRId32,
              server->ssid, delay.seconds, delay.nanoseconds);

    return _anjay_servers_schedule_first_retryable(
            anjay->sched, server, delay,
            send_update_sched_job, server->ssid);
}

static int schedule_next_update(anjay_t *anjay, anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    avs_time_duration_t remaining = _anjay_register_time_remaining(
            &server->data_active.registration_info);
    avs_time_duration_t interval_margin =
            get_server_update_interval_margin(anjay, server);
    remaining = avs_time_duration_diff(remaining, interval_margin);

    if (remaining.seconds < ANJAY_MIN_UPDATE_INTERVAL_S) {
        remaining = avs_time_duration_from_scalar(ANJAY_MIN_UPDATE_INTERVAL_S,
                                                  AVS_TIME_S);
    }

    return schedule_update(anjay, server, remaining);
}

bool _anjay_server_primary_connection_valid(anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    return server->data_active.primary_conn_type != ANJAY_CONNECTION_UNSET
            && _anjay_connection_get_online_socket(
                       (anjay_connection_ref_t) {
                           .server = server,
                           .conn_type = server->data_active.primary_conn_type
                       }) != NULL;
}

int _anjay_server_reschedule_update_job(anjay_t *anjay,
                                        anjay_server_info_t *server) {
    _anjay_sched_del(anjay->sched, &server->next_action_handle);
    if (schedule_next_update(anjay, server)) {
        anjay_log(ERROR, "could not schedule next Update for server %u",
                  server->ssid);
        return -1;
    }
    return 0;
}

static int reschedule_update_for_server(anjay_t *anjay,
                                        anjay_server_info_t *server) {
    _anjay_sched_del(anjay->sched, &server->next_action_handle);
    if (schedule_update(anjay, server, AVS_TIME_DURATION_ZERO)) {
        anjay_log(ERROR, "could not schedule send_update_sched_job");
        return -1;
    }
    return 0;
}

static int
reschedule_update_for_all_servers(anjay_t *anjay) {
    int result = 0;

    AVS_LIST(anjay_server_info_t) it;
    AVS_LIST_FOREACH(it, anjay->servers->servers) {
        if (_anjay_server_active(it)) {
            int partial = reschedule_update_for_server(anjay, it);
            if (!result) {
                result = partial;
            }
        }
    }

    return result;
}

/**
 * Reschedules Update for a specified server or all servers. In the very end, it
 * calls schedule_update(), which basically speeds up the scheduled Update
 * operation (it is normally scheduled for "just before the lifetime expires",
 * this function reschedules it to now. The scheduled job is
 * send_update_sched_job() and it is also used for regular Updates.
 *
 * Aside from being a public API, this is also called in:
 *
 * - anjay_register_object() and anjay_unregister_object(), to force an Update
 *   when the set of available Objects changed
 * - serv_execute(), as a default implementation of Registration Update Trigger
 * - server_modified_notify(), to force an Update whenever Lifetime or Binding
 *   change
 * - _anjay_schedule_reregister(), although that's probably rather superfluous -
 *   see the docs of that function for details
 */
int anjay_schedule_registration_update(anjay_t *anjay,
                                       anjay_ssid_t ssid) {
    if (anjay_is_offline(anjay)) {
        anjay_log(ERROR,
                  "cannot schedule registration update while being offline");
        return -1;
    }
    int result = 0;

    if (ssid == ANJAY_SSID_ANY) {
        result = reschedule_update_for_all_servers(anjay);
    } else {
        anjay_server_info_t *server = _anjay_servers_find_active(anjay, ssid);
        if (!server) {
            anjay_log(ERROR, "no active server with SSID = %u", ssid);
            result = -1;
        } else {
            result = reschedule_update_for_server(anjay, server);
        }
    }

    return result;
}

int _anjay_schedule_reregister(anjay_t *anjay, anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    server->data_active.registration_info.expire_time = AVS_TIME_REAL_INVALID;
    return _anjay_server_deactivate(anjay, server->ssid,
                                    AVS_TIME_DURATION_ZERO);
}

int _anjay_schedule_server_reconnect(anjay_t *anjay,
                                     anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    _anjay_connection_suspend((anjay_connection_ref_t) {
        .server = server,
        .conn_type = ANJAY_CONNECTION_UNSET
    });
    return _anjay_schedule_reload_server(anjay, server);
}

static int registration_update_with_ctx(anjay_registration_update_ctx_t *ctx,
                                        anjay_server_info_t *server) {
    int retval = _anjay_update_registration(ctx);
    switch (retval) {
    case 0:
        return (int) ANJAY_UPDATE_SUCCESS;

    case ANJAY_REGISTRATION_UPDATE_REJECTED:
        anjay_log(DEBUG, "update rejected for SSID = %u; "
                         "needs re-registration", server->ssid);
        server->data_active.registration_info.expire_time =
                AVS_TIME_REAL_INVALID;
        return (int) ANJAY_UPDATE_NEEDS_REGISTRATION;

    case AVS_COAP_CTX_ERR_NETWORK:
        anjay_log(ERROR, "network communication error while updating "
                         "registration for SSID==%" PRIu16, server->ssid);
        // We cannot use _anjay_schedule_server_reconnect(),
        // because it would mean an endless loop without backoff
        // if the server is down. Instead, we disconnect the socket
        // and rely on scheduler's backoff. During the next call,
        // _anjay_server_refresh() will reconnect the socket.
        _anjay_connection_suspend((anjay_connection_ref_t) {
            .server = server,
            .conn_type = server->data_active.primary_conn_type
        });
        return (int) ANJAY_UPDATE_FAILED;

    default:
        anjay_log(ERROR, "could not send registration update: %d", retval);
        return (int) ANJAY_UPDATE_FAILED;
    }
}

static int
ensure_valid_registration_with_ctx(anjay_t *anjay,
                                   anjay_registration_update_ctx_t *ctx,
                                   anjay_server_info_t *server) {
    anjay_update_result_t update_result;

    if (!_anjay_server_primary_connection_valid(server)) {
        anjay_log(INFO, "No valid existing connection to Registration "
                  "Interface for SSID = %u, needs re-registration",
                  server->ssid);
        server->data_active.registration_info.expire_time =
                AVS_TIME_REAL_INVALID;
        update_result = ANJAY_UPDATE_NEEDS_REGISTRATION;
    } else if (_anjay_server_registration_expired(server)) {
        update_result = ANJAY_UPDATE_NEEDS_REGISTRATION;
    } else if (!_anjay_needs_registration_update(ctx)) {
        update_result = ANJAY_UPDATE_SUCCESS;
    } else {
        update_result = (anjay_update_result_t)
                registration_update_with_ctx(ctx, server);
    }

    switch (update_result) {
    case (int) ANJAY_UPDATE_SUCCESS:
        return (int) ANJAY_REGISTRATION_SUCCESS;
    case (int) ANJAY_UPDATE_NEEDS_REGISTRATION: {
        if (!_anjay_server_primary_connection_valid(server)
                && _anjay_server_setup_primary_connection(server)) {
            return (int) ANJAY_REGISTRATION_FAILED;
        }
        int retval = _anjay_register(ctx);
        if (retval) {
            anjay_log(DEBUG, "re-registration failed");
            if (retval == ANJAY_ERR_FORBIDDEN) {
                return (int) ANJAY_REGISTRATION_FORBIDDEN;
            } else {
                return (int) ANJAY_REGISTRATION_FAILED;
            }
        } else {
            // Failure to handle Bootstrap state is not a failure of the
            // Register operation - hence, not checking return value.
            _anjay_bootstrap_notify_regular_connection_available(anjay);
            return (int) ANJAY_REGISTRATION_SUCCESS;
        }
    }
    default:
        AVS_UNREACHABLE("Invalid value of anjay_update_result_t");
        // fall-through
    case (int) ANJAY_UPDATE_FAILED:
        return (int) ANJAY_REGISTRATION_FAILED;
    }
}

typedef int registration_action_t(anjay_t *anjay,
                                  anjay_registration_update_ctx_t *ctx,
                                  anjay_server_info_t *server);

static int perform_registration_action(anjay_t *anjay,
                                       anjay_server_info_t *server,
                                       registration_action_t *action) {
    assert(_anjay_server_active(server));
    assert(server->ssid != ANJAY_SSID_BOOTSTRAP);

    anjay_registration_update_ctx_t ctx;
    if (_anjay_registration_update_ctx_init(anjay, &ctx, server)) {
        return -1;
    }

    int retval = action(anjay, &ctx, server);
    if (!retval) {
        // Ignore errors, failure to flush notifications is not fatal.
        _anjay_observe_sched_flush(anjay, (anjay_connection_key_t) {
            .ssid = server->ssid,
            .type = server->data_active.primary_conn_type
        });
    }

    _anjay_registration_update_ctx_release(&ctx);

    if (!retval && _anjay_server_reschedule_update_job(anjay, server)) {
        // Updates are retryable, we only need to reschedule after success
        retval = -1;
    }
    return retval;
}

static int
registration_update_if_possible_with_ctx(anjay_t *anjay,
                                         anjay_registration_update_ctx_t *ctx,
                                         anjay_server_info_t *server) {
    (void) anjay;

    if (!_anjay_server_primary_connection_valid(server)) {
        anjay_log(INFO, "No valid existing connection to Registration "
                  "Interface for SSID = %u, needs re-registration",
                  server->ssid);
        server->data_active.registration_info.expire_time =
                AVS_TIME_REAL_INVALID;
        return (int) ANJAY_UPDATE_NEEDS_REGISTRATION;
    }

    if (_anjay_server_registration_expired(server)) {
        return (int) ANJAY_UPDATE_NEEDS_REGISTRATION;
    }

    return registration_update_with_ctx(ctx, server);
}

anjay_update_result_t
_anjay_server_registration_update(anjay_t *anjay,
                                  anjay_server_info_t *server) {
    int result = perform_registration_action(
            anjay, server, registration_update_if_possible_with_ctx);
    return result >= 0 ? (anjay_update_result_t) result : ANJAY_UPDATE_FAILED;
}

anjay_registration_result_t
_anjay_server_ensure_valid_registration(anjay_t *anjay,
                                        anjay_server_info_t *server) {
    int result = perform_registration_action(
            anjay, server, ensure_valid_registration_with_ctx);
    return result >= 0
            ? (anjay_registration_result_t) result : ANJAY_REGISTRATION_FAILED;
}

int _anjay_server_deregister(anjay_t *anjay,
                             anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    anjay_connection_ref_t connection = {
        .server = server,
        .conn_type = server->data_active.primary_conn_type
    };
    if (connection.conn_type == ANJAY_CONNECTION_UNSET
            || _anjay_bind_server_stream(anjay, connection)) {
        anjay_log(ERROR, "could not get stream for server %u, skipping",
                  server->ssid);
        return 0;
    }

    int result = _anjay_deregister(
            anjay, server->data_active.registration_info.endpoint_path);
    if (result) {
        anjay_log(ERROR, "could not send De-Register request: %d", result);
    }

    _anjay_release_server_stream_without_scheduling_queue(anjay);
    return result;
}

const anjay_registration_info_t *
_anjay_server_registration_info(anjay_server_info_t *server) {
    assert(_anjay_server_active(server));
    return &server->data_active.registration_info;
}

static avs_time_real_t get_registration_expire_time(int64_t lifetime_s) {
    return avs_time_real_add(avs_time_real_now(),
                             avs_time_duration_from_scalar(lifetime_s,
                                                           AVS_TIME_S));
}

void _anjay_server_update_registration_info(
        anjay_server_info_t *server,
        AVS_LIST(const anjay_string_t) *move_endpoint_path,
        anjay_update_parameters_t *move_params) {
    assert(_anjay_server_active(server));
    anjay_registration_info_t *info = &server->data_active.registration_info;

    if (move_endpoint_path && move_endpoint_path != &info->endpoint_path) {
        AVS_LIST_CLEAR(&info->endpoint_path);
        info->endpoint_path = *move_endpoint_path;
        *move_endpoint_path = NULL;
    }

    if (move_params && move_params != &info->last_update_params) {
        AVS_LIST(anjay_dm_cache_object_t) tmp = info->last_update_params.dm;
        info->last_update_params.dm = move_params->dm;
        move_params->dm = tmp;

        info->last_update_params.lifetime_s = move_params->lifetime_s;
        memcpy(&info->last_update_params.binding_mode,
               &move_params->binding_mode,
               sizeof(info->last_update_params.binding_mode));

        _anjay_update_parameters_cleanup(move_params);
    }

    info->expire_time =
            get_registration_expire_time(info->last_update_params.lifetime_s);
}
