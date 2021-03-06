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

#ifndef ANJAY_SERVERS_CONNECTION_INFO_H
#define ANJAY_SERVERS_CONNECTION_INFO_H

#include "../anjay_core.h"
#include "../utils_core.h"

#if !defined(ANJAY_SERVERS_INTERNALS) && !defined(ANJAY_TEST)
#error "Headers from servers/ are not meant to be included from outside"
#endif

VISIBILITY_PRIVATE_HEADER_BEGIN

typedef struct {
    avs_net_resolved_endpoint_t preferred_endpoint;
    char dtls_session_buffer[ANJAY_DTLS_SESSION_BUFFER_SIZE];
    char last_local_port[ANJAY_MAX_URL_PORT_SIZE];
} anjay_server_connection_nontransient_state_t;

/**
 * State of a specific connection to an LwM2M server. One server entry may have
 * multiple connections, if multiple binding is used (e.g. US binding mode,
 * signifying UDP+SMS).
 */
typedef struct {
    /**
     * Socket used for communication with the given server. Aside from being
     * used for actual communication, the value of this field is also used as
     * kind of a three-state flag:
     *
     * - When it is NULL - it means either of the three:
     *   - the server is either inactive (see docs to anjay_server_info_t for
     *     details)
     *   - initial attempt to connect the socket failed - the server may still
     *     be active if some other transport could be connected -
     *     _anjay_active_server_refresh() reschedules reload_servers_sched_job()
     *     in such case
     *   - the transport represented by this connection object is not used in
     *     the current binding
     *
     * - The socket may exist, but be offline (closed), when:
     *   - reconnection is scheduled, as part of the executions path of
     *     _anjay_schedule_server_reconnect(), anjay_schedule_reconnect() or
     *     registration_update_with_ctx() - see those functions' docs and call
     *     graphs for details
     *   - when the queue mode for this connection is used, and
     *     MAX_TRANSMIT_WAIT passed since last communication
     *   - when Client- or Server-Initiated Bootstrap is in progress - all
     *     non-Bootstrap sockets are disconnected in such a case.
     *
     *   Note that the server is still considered active if it has a created,
     *   but disconnected socket. Such closed socket still retains some of its
     *   previous state (including the remote endpoint's hostname and security
     *   keys etc.) in avs_commons' internal structures. This is used by
     *   _anjay_connection_internal_ensure_online() to reconnect the socket if
     *   necessary.
     *
     *   We cannot rely on reading the connection information from the data
     *   model instead, because it may be gone - for example when trying to
     *   De-register from a server that has just been deleted by a Bootstrap
     *   Server. At least that example was used in the docs prior to the June
     *   2018 server subsystem docs rewrite, because currently we don't seem to
     *   send Deregister messages in such a case anyway, so this might be a TODO
     *   for investigating.
     *
     * - The socket may exist and be online (ready for communication) - this is
     *   the normal, fully active state.
     */
    avs_net_abstract_socket_t *conn_socket_;
#if defined(__GNUC__) && !defined(__CC_ARM) \
        && !(defined(ANJAY_SERVERS_CONNECTION_SOURCE) || defined(ANJAY_TEST))
#pragma GCC poison conn_socket_
#endif

    /**
     * The part of active connection state that is intentionally NOT cleaned up
     * when deactivating the server. It contains:
     *
     * - preferred_endpoint, i.e. the preference which server IP address to use
     *   if multiple are returned during DNS resolution
     * - DTLS session cache
     * - Last bound local port
     *
     * These information will be used during the next reactivation to attempt
     * recreating the socket in a state most similar possible to how it was
     * before.
     */
    anjay_server_connection_nontransient_state_t nontransient_state;

    /**
     * Cached value of the connection mode, according to the Binding value most
     * recently read in _anjay_active_server_refresh().
     */
    anjay_server_connection_mode_t mode;

    /**
     * Handle to scheduled queue_mode_close_socket() scheduler job. Scheduled
     * by _anjay_connection_schedule_queue_mode_close().
     */
    anjay_sched_handle_t queue_mode_close_socket_clb_handle;
} anjay_server_connection_t;

anjay_server_connection_t *
_anjay_get_server_connection(anjay_connection_ref_t ref);

avs_net_abstract_socket_t *_anjay_connection_internal_get_socket(
        const anjay_server_connection_t *connection);

void
_anjay_connection_internal_clean_socket(anjay_server_connection_t *connection);

bool _anjay_connection_is_online(anjay_server_connection_t *connection);

int
_anjay_connection_internal_bring_online(anjay_t *anjay,
                                        anjay_server_connection_t *connection,
                                        bool *out_session_resumed);

/**
 * @returns @li 0 on success,
 *          @li a positive errno value in case of a primary socket (UDP) error,
 *          @li a negative value in case of other error.
 */
int _anjay_active_server_refresh(anjay_t *anjay, anjay_server_info_t *server);

VISIBILITY_PRIVATE_HEADER_END

#endif // ANJAY_SERVERS_CONNECTION_INFO_H
