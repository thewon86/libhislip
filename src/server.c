/*
 * Copyright (c) 2017-2022  Martin Lund
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <hislip/server.h>
#include <server_p.h>
#include "tcp.h"
#include "message.h"
#include "print.h"
#include "session.h"

typedef LIST_HEAD(subaddress_head_t, hs_subaddress_data) subaddress_head_t;
subaddress_head_t *subaddress_head;
hs_subaddress_data_t *subaddress_default = NULL;

static hs_subaddress_data_t *find_subaddress_data(char *subaddress)
{
    hs_subaddress_data_t *sd;

    // Lookup subaddress in list of registered subaddresses
    LIST_FOREACH(sd, subaddress_head, entries)
    {
        if (strcmp(sd->subaddress, subaddress) == 0)
        {
            debug_printf("Found subaddress\n");
            break;
        }
    }

    return sd;
}

static void hs_process(int socket, hs_server_t *server)
{
    msg_header_t msg_header;
    int bytes_read, sessionID = -1;
    void *payload = NULL;
    int payload_accumulated_size = 0;
    int timeout = server->config.message_timeout;

    // Enter message processing loop
    while (1)
    {
        /* 1. Receive message (blocking, no timeout)
         * 1.1 Receive header
         * 1.2 Decode payload length
         * 1.3 Allocate payload length memory
         * 1.4 Receive payload length
         * 2. Decode message
         * 3. Execute request
         * 4. Send response (blocking, with timeout)
         */

        // Receive message header (blocking until data available)
        if ((bytes_read = server->tcp_read(socket, &msg_header, MSG_HEADER_SIZE, 0)) <= 0)
        {
            debug_printf("Client closed connection (socket = %d)\n", socket);
            server->tcp_stop(socket);
            goto __exit_hs_process;
        }

        // Skip until we have enough bytes representing a message header
        if (bytes_read < MSG_HEADER_SIZE)
        {
            continue;
        }

        // Convert header multi-byte values to host byte order
        msg_header.prologue = ntohs(msg_header.prologue);
        msg_header.parameter = ntohl(msg_header.parameter);
        msg_header.payload_length = ntohll(msg_header.payload_length);

        debug_printf("Received message:\n");
        debug_printf(" prologue = %" PRIu32 "\n", msg_header.prologue);
        debug_printf(" type = %" PRIu32 "\n", msg_header.type);
        debug_printf(" control_code = %" PRIu32 "\n", msg_header.control_code);
        debug_printf(" parameter = %" PRIu32 "\n", msg_header.parameter);
        debug_printf(" payload_length = %" PRIu64 "\n", msg_header.payload_length);

        // Verify message header
        if (msg_header_verify(&msg_header))
        {
            // Invalid header
            error_printf("Invalid header\n");

            // Send FatalError message with error code 1 (Poorly formed message header)
            //msg_send(FatalError, 1, 0, error_string(1), error_string_length(1));

            continue; // Skip until valid header received
        }

        // Receive any payload
        if (msg_header.payload_length > 0)
        {
            // Check payload size
            if (msg_header.payload_length > (server->config.message_size_max - MSG_HEADER_SIZE))
            {
                error_printf("Maximum message size exceeded\n");
                continue;
            }

            // Allocate payload receive buffer
            payload = malloc(msg_header.payload_length);
            if (payload == NULL)
            {
                error_printf("malloc() failed\n");
                continue;
            }

            // Read payload
            if ((bytes_read = server->tcp_read(socket, payload, msg_header.payload_length, server->config.message_timeout)) <= 0)
            {
                debug_printf("Client closed connection\n");
                server->tcp_stop(socket);
                goto __exit_hs_process;
            }
        }

        // Perform action depending on message type
        switch (msg_header.type)
        {
            void *message;
            uint8_t control_code;;
            uint32_t parameter;
            uint32_t message_id;

            case Initialize:
            {
                debug_printf("Received Initialize message!\n");

                // Decode parameter field:
                //  Client protocol version (upper)
                //  Client vendor id (lower)
                uint16_t client_vendor_id = msg_header.parameter;
                uint16_t client_protocol_version = msg_header.parameter >> 16;
                uint16_t server_protocol_version = (HISLIP_VERSION_MAJOR << 8) + HISLIP_VERSION_MINOR;

                // Create new connection session
                sessionID = session_new();
                if (sessionID < 0)
                {
                    error_printf("Could not allocate new session!\n");
                    server->tcp_stop(socket);
                    goto __exit_hs_process;
                }
                debug_printf("(Server) sessionID = %d\n", sessionID);

                // Link connection session with registered subaddress callbacks
                if (payload == NULL) {
                    session[sessionID].subaddress_data = subaddress_default;
                } else {
                    hs_subaddress_data_t *sd;
                    char *subaddress;
                    subaddress = payload;
                    sd = find_subaddress_data(subaddress);
                    if (sd != NULL) {
                        session[sessionID].subaddress_data = sd;
                    } else {
                        error_printf("Unable to find subaddress\n");
                        // TODO: Respond FatalError
                        server->tcp_stop(socket);
                        goto __exit_hs_process;
                    }
                }

                session[sessionID].socket_sync = socket;
                session[sessionID].client_vendor_id = client_vendor_id;
                session[sessionID].client_protocol_version = client_protocol_version;
                session[sessionID].server_message_size_max = server->config.message_size_max;

                // Construct InitializeResponse message including
                //  Session ID
                //  Overlap-mode (synchronized)
                //  Server protocol version
                control_code = CC_PREFER_SYNC;
                uint16_t version = server_protocol_version < client_protocol_version ? server_protocol_version : client_protocol_version;
                parameter = (version << 16) + sessionID;
                msg_create(&message, InitializeResponse, control_code, parameter, 0, NULL);

                // Send InitializeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent InitializeResponse message\n");
            }
                break;

            case InitializeResponse:
                break;

            case FatalError:
                debug_printf("FatalError: %.*s\n", (int)msg_header.payload_length, (char *)payload);
                break;

            case Error:
                break;

            case AsyncLock:
                debug_printf("Received AsyncLock message!\n");

                sessionID = msg_header.parameter;
                debug_printf("Received SessionID = %d\n", sessionID);

                if (msg_header.control_code == CC_REQUEST) {
                    control_code = CC_REQUEST_RSP_SUCCESS;

                    if (msg_header.payload_length == 0) {
                        // TODO: Exclusive
                        server->asyncLock |= 1;
                        server->asyncLockCnt++;
                    } else {
                        // TODO: Shared
                        server->asyncLock |= 2;
                    }
                } else if (msg_header.control_code == CC_RELEASE) {
                    if ((server->asyncLock & 0x1) == 1) {
                        control_code = CC_RELEASE_RSP_SUCCESS_EXCLUSIVE;
                        server->asyncLockCnt--;
                        if (server->asyncLockCnt == 0) {
                            server->asyncLock &= ~0x1;
                        }
                    } else {
                        control_code = CC_RELEASE_RSP_SUCCESS_SHARED;
                        server->asyncLock &= ~0x2;
                    }
                } else {
                    control_code = CC_REQUEST_RSP_SUCCESS;
                }
                msg_create(&message, AsyncLockResponse, control_code, 0, 0, NULL);

                // Send InitializeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncLockResponse message\n");
            break;

            case AsyncLockResponse:
                break;

            case Data:
            {
                // FIXME: Accumulate payload
                message_id = msg_header.parameter;
                debug_printf("Received Data message (message ID = %" PRIu32 ")\n", message_id);

                hs_subaddress_data_t *subaddress_data = session[sessionID].subaddress_data;

                if (subaddress_data->callbacks.message_sync != NULL)
                {
                    hs_msg_ctx_t msg_ctx;
                    msg_ctx.socket = socket;
                    msg_ctx.sessionID = sessionID;
                    msg_ctx.message_id = message_id;
                    msg_ctx.timeout = timeout;
                    msg_ctx.rmt = true;
                    subaddress_data->callbacks.message_sync(&msg_ctx, payload, msg_header.payload_length, false);
                }
            }
                break;

            case DataEnd:
            {
                // FIXME: Allocate memory for full payload and copy payloads
                // accumulated
                message_id = msg_header.parameter;
                debug_printf("Received DataEnd message (message ID = %" PRIu32 ")\n", message_id);

                hs_subaddress_data_t *subaddress_data = session[sessionID].subaddress_data;

                if (subaddress_data->callbacks.message_sync != NULL)
                {
                    hs_msg_ctx_t msg_ctx;
                    msg_ctx.socket = socket;
                    msg_ctx.sessionID = sessionID;
                    msg_ctx.message_id = message_id;
                    msg_ctx.timeout = timeout;
                    msg_ctx.rmt = true;
                    subaddress_data->callbacks.message_sync(&msg_ctx, payload, msg_header.payload_length, true);
                }
            }
                break;

            case DeviceClearComplete:
                debug_printf("Received DeviceClearComplete message!\n");

                msg_create(&message, DeviceClearAcknowledge, 0, 0, 0, NULL);

                // Send DeviceClearAcknowledge message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent DeviceClearAcknowledge message\n");

                break;

            case DeviceClearAcknowledge:
                break;

            case AsyncRemoteLocalControl:
            {
                debug_printf("Received AsyncRemoteLocalControl message!\n");

                uint8_t rlc = msg_header.control_code;
#if DEBUG
                switch (rlc) {
                case 0:
                    debug_printf("Disable remote\n");
                    break;
                case 1:
                    debug_printf("Enable remote\n");
                    break;
                case 2:
                    debug_printf("Disable remote and go to local\n");
                    break;
                case 3:
                    debug_printf("Enable remote and go to remote\n");
                    break;
                case 4:
                    debug_printf("Enable remote and lockout local\n");
                    break;
                case 5:
                    debug_printf("Enable remote, go to remote, and set local lockout\n");
                    break;
                case 6:
                    debug_printf("go to local without channging REN or lockout state\n");
                    break;
                default:
                    error_printf("Unkown RemoteLocalControl code\n");
                    break;
                }
#endif
                // TODO: send rlc to application

                msg_create(&message, AsyncRemoteLocalResponse, 0, 0, 0, NULL);

                // Send AsyncRemoteLocalResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncRemoteLocalResponse message\n");
            }
            break;

            case AsyncRemoteLocalResponse:
                break;

            case Trigger:
                debug_printf("Received Trigger message!\n");

                // TODO: do something

                break;

            case AsyncMaximumMessageSize:
            {
                debug_printf("Received AsyncMaximumMessageSize message!\n");

                debug_printf("Received SessionID = %d\n", sessionID);

                uint64_t size_p = *(uint64_t *)payload;
                uint64_t size = ntohll(size_p);

                debug_printf("(Client) AsyncMaximumMessageSize message (size = %" PRIu64 ")\n", size);
                session[sessionID].client_message_size_max = size;
                free(session[sessionID].data);
                session[sessionID].data = malloc(size);

                debug_printf("(Server) AsyncMaximumMessageSizeResponse message (size = %" PRIu64 ")\n", session[sessionID].server_message_size_max);
                size = ntohll(session[sessionID].server_message_size_max);
                msg_create(&message, AsyncMaximumMessageSizeResponse, 0, 0, 8, &size);

                // Send AsyncMaximumMessageSizeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncMaximumMessageSizeResponse message\n");
            }
                break;

            case AsyncMaximumMessageSizeResponse:
                break;

            case AsyncInitialize:
            {
                debug_printf("Received AsyncInitialize message!\n");

                sessionID = msg_header.parameter;
                debug_printf("Received SessionID = %d\n", sessionID);
                session[sessionID].socket_async = socket;

                // Construct AsyncInitializeResponse message including
                //  Server-vendorID
                parameter = HISLIP_VENDOR_ID;
                msg_create(&message, AsyncInitializeResponse, 0, parameter, 0, NULL);

                // Send InitializeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncInitializeResponse message\n");
            }
                break;

            case AsyncInitializeResponse:
                break;

            case AsyncDeviceClear:
                debug_printf("Received AsyncDeviceClear message!\n");

                msg_create(&message, AsyncDeviceClearAcknowledge, 0, 0, 0, NULL);

                // Send AsyncDeviceClearAcknowledge message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncDeviceClearAcknowledge message\n");

                break;

            case AsyncDeviceClearAcknowledge:
                break;

            case AsyncStatusQuery:
                debug_printf("Received AsyncStatusQuery message!\n");

                msg_create(&message, AsyncStatusResponse, 0, 0, 0, NULL);

                // Send AsyncStatusResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncStatusResponse message\n");

                break;

            case AsyncStatusResponse:
                break;

            case AsyncLockInfo:
                debug_printf("Received AsyncLockInfo message!\n");

                if (server->asyncLockCnt == 0) {
                    control_code = 0;
                } else {
                    control_code = 1;
                }
                parameter = server->asyncLockCnt;

                msg_create(&message, AsyncLockInfoResponse, control_code, parameter, 0, NULL);

                // Send AsyncStatusResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncLockInfoResponse message\n");

                break;

            case AsyncLockInfoResponse:
                break;

            default:
                error_printf("Received Unkown message! (type = %" PRIu32 ")\n", msg_header.type);
                break;
        }

        if (payload != NULL) {
            free(payload);
                payload = NULL;
        }
    }

__exit_hs_process:
    if (payload != NULL) {
        free(payload);
        payload = NULL;
    }

    if (sessionID != -1) {
        if (socket == session[sessionID].socket_sync) session[sessionID].socket_sync = -1;
        if (socket == session[sessionID].socket_async) session[sessionID].socket_async = -1;
        if ((session[sessionID].socket_sync == -1)
         && (session[sessionID].socket_async == -1)) {
            session_free(sessionID);
        }
    }
}

static void connection_callback(int socket, void *data)
{
    debug_printf("New connection thread (socket = %d)\n", socket);

    hs_process(socket, data);
}

EXPORT int hs_server_run(hs_server_t *server)
{
    // Start server
    debug_printf("Starting HiSLIP server. Ver:%d.%d\n", HISLIP_VERSION_MAJOR, HISLIP_VERSION_MINOR);
    return server->tcp_start(server->config.port, server->config.connections_max, connection_callback, server);
}

EXPORT int hs_server_config_init(hs_server_config_t *config)
{
    // Initialize server configuration with default values
    config->port = HISLIP_PORT;
    config->connections_max = 1;
    config->message_size_max = 516;
    config->message_timeout = 2000; // 2 seconds

    return 0;
}

EXPORT int hs_server_init(hs_server_t *server, hs_server_config_t config)
{
    if (config.message_size_max < MSG_HEADER_SIZE)
    {
        error_printf("Maximum message size must be larger than %d\n", MSG_HEADER_SIZE);
        return -1;
    }

    // Intialize subaddress list
    subaddress_head = malloc(sizeof(subaddress_head_t)); // Move sublist to
                                                         // server structure so
                                                         // we can start
                                                         // multilple servers
    LIST_INIT(subaddress_head);

    // Set configuration
    server->config = config;

    // Configure TCP calls
    server->tcp_start = tcp_server_start;
    server->tcp_read = tcp_read;
    server->tcp_write = tcp_write;
    server->tcp_stop = tcp_disconnect;

    server->asyncLock = 0;
    server->asyncLockCnt = 0;

    return 0;
}

EXPORT int hs_server_register_subaddress(hs_server_t *server, char *subaddress, hs_subaddress_callbacks_t callbacks)
{
    // Add subaddres to list of registered subaddresses
    hs_subaddress_data_t *subaddress_data = malloc(sizeof(hs_subaddress_data_t));
    if (subaddress_data == NULL)
    {
        error_printf("Could not allocated space for new subaddress\n");
        return -1;
    }

    // Install subaddress data
    subaddress_data->callbacks = callbacks;
    subaddress_data->subaddress = subaddress;

    // Add to list
    LIST_INSERT_HEAD(subaddress_head, subaddress_data, entries);

    if (subaddress_default == NULL) {
        subaddress_default = subaddress_data;
    }

    return 0;
}

EXPORT int hs_server_send_response(hs_msg_ctx_t *msg_ctx, void *data, int length)
{
    // Create DataEnd response message
    void *message = NULL;
    char *pdata = (char *)data;
    uint8_t control_code;
    uint64_t message_payload_max;
    int64_t message_bytes_sent;

    message_payload_max = session[msg_ctx->sessionID].client_message_size_max - MSG_HEADER_SIZE;

    // Calculate how many message bytes to send
    uint64_t message_bytes_remaining = length;

    debug_printf("Sending message length: %" PRIu64 "; max: %" PRIu64 "\n" , message_bytes_remaining, message_payload_max);
    control_code = CC_RMT_DELIVERED;
    while (message_bytes_remaining)
    {
        // Create Data message
        if (message_bytes_remaining > message_payload_max)
        {
            msg_create(&message, Data, control_code, msg_ctx->message_id, message_payload_max, (void*)(pdata));
            debug_printf("Sending Data message (message ID = %" PRIu32 ")\n", msg_ctx->message_id);
        }
        else
        {
            msg_create(&message, DataEnd, control_code, msg_ctx->message_id, message_bytes_remaining, (void*)(pdata));
            debug_printf("Sending DataEnd message (message ID = %" PRIu32 ")\n", msg_ctx->message_id);
        }

        message_bytes_sent = msg_send(msg_ctx->socket, message, msg_ctx->timeout);
        free(message);
        if (message_bytes_sent < 0)
        {
            error_printf("Sending Data error!!!\n");
            // Throw fatal error
            return -1;
        }

        message_bytes_remaining -= (message_bytes_sent - MSG_HEADER_SIZE);
        pdata += (message_bytes_sent - MSG_HEADER_SIZE);

        control_code = CC_RMT_NOT_DELIVERED;
    }

    return length;
}

EXPORT int hs_server_send_message(hs_msg_ctx_t *msg_ctx, void *data, int length, bool end)
{
    void *message = NULL;
    uint8_t control_code;
    uint64_t message_payload_max;
    int64_t message_bytes_sent;

    message_payload_max = session[msg_ctx->sessionID].client_message_size_max - MSG_HEADER_SIZE;

    // Calculate how many message bytes to send
    uint64_t message_bytes_remaining = length;

    debug_printf("Sending message length: %" PRIu64 "; max: %" PRIu64 "\n" , message_bytes_remaining, message_payload_max);
    if (msg_ctx->rmt) {
        control_code = CC_RMT_DELIVERED;
    } else {
        control_code = CC_RMT_NOT_DELIVERED;
    }
    if (end) {
        msg_create(&message, DataEnd, control_code, msg_ctx->message_id, length, data);
        debug_printf("Sending DataEnd message (message ID = %" PRIu32 ")\n", msg_ctx->message_id);
        msg_ctx->rmt = true;
    } else {
        msg_create(&message, Data, control_code, msg_ctx->message_id, length, data);
        debug_printf("Sending Data message (message ID = %" PRIu32 ")\n", msg_ctx->message_id);
        msg_ctx->rmt = false;
    }

    message_bytes_sent = msg_send(msg_ctx->socket, message, msg_ctx->timeout);
    free(message);
    if (message_bytes_sent < 0)
    {
        error_printf("Sending Data error!!!\n");
        // Throw fatal error
        return -1;
    }

    return length;
}

EXPORT int hs_server_write(hs_msg_ctx_t *msg_ctx, void *data, int length)
{
    char *pdata = (char *)data;
    uint64_t clnt_pl_max, data_len;
    int64_t write_bytes = 0;

    // Calculate how many message bytes to write
    uint64_t ramaining = length;

    clnt_pl_max = session[msg_ctx->sessionID].client_message_size_max - MSG_HEADER_SIZE;
    data_len = session[msg_ctx->sessionID].data_len;
    debug_printf("data: %p, %d, %" PRIu64 "\n", data, length, data_len);
    while (ramaining > (clnt_pl_max - data_len)) {
        write_bytes = clnt_pl_max - data_len;
        memcpy(&session[msg_ctx->sessionID].data[data_len], (const void *)pdata, write_bytes);
        pdata += write_bytes;
        ramaining -= write_bytes;
        hs_server_send_message(msg_ctx, session[msg_ctx->sessionID].data, clnt_pl_max, false);
        data_len = 0;
    }
    if (ramaining > 0) {
        write_bytes = ramaining;
        memcpy(&session[msg_ctx->sessionID].data[data_len], (const void *)pdata, write_bytes);
        data_len += write_bytes;
    }
    session[msg_ctx->sessionID].data_len = data_len;

    return length;
}

EXPORT int hs_server_flush(hs_msg_ctx_t *msg_ctx)
{
    debug_printf("data: %d\n", session[msg_ctx->sessionID].data_len);
    hs_server_send_message(msg_ctx, session[msg_ctx->sessionID].data, session[msg_ctx->sessionID].data_len, true);
    session[msg_ctx->sessionID].data_len = 0;

    return 0;
}
