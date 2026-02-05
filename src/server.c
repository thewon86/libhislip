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
#include "tcp.h"
#include "message.h"
#include "print.h"
#include "session.h"

typedef LIST_HEAD(subaddress_head_t, hs_subaddress_data_t) subaddress_head_t;
subaddress_head_t *subaddress_head;

static int server_subaddress_link(hs_server_t *server, char *subaddress, hs_subaddress_data_t *subaddress_data)
{
    bool match_found = false;
    hs_subaddress_data_t *sd;

    // Lookup subaddress in list of registered subaddresses
    LIST_FOREACH(sd, subaddress_head, entries)
    {
        if (strcmp(sd->subaddress, subaddress) == 0)
        {
            match_found = true;
            subaddress_data = sd;
            debug_printf("Found subaddress\n");
            break;
        }
    }

    // Link connection session to subaddress

    return match_found;
}

static void hs_process(int socket, hs_server_t *server)
{
    msg_header_t msg_header;
    int bytes_read, sessionID;
    char *subaddress;
    void *payload = NULL;
    int timeout = server->config->message_timeout;

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
        debug_printf(" prologue = %d\n", msg_header.prologue);
        debug_printf(" type = %d\n", msg_header.type);
        debug_printf(" control_code = %d\n", msg_header.control_code);
        debug_printf(" parameter = %d\n", msg_header.parameter);
        debug_printf(" payload_length = %ld\n", msg_header.payload_length);

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
            if (msg_header.payload_length > (server->config->message_size_max - MSG_HEADER_SIZE))
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
            if ((bytes_read = server->tcp_read(socket, payload, msg_header.payload_length, server->config->message_timeout)) <= 0)
            {
                debug_printf("Client closed connection\n");
                server->tcp_stop(socket);
                goto __exit_hs_process;
            }
        }

        // Perform action depending on message type
        switch (msg_header.type)
        {
            void *message = NULL;
            uint8_t control_code;;
            uint32_t parameter;
            uint32_t message_id;
            int received_SessionID;

            case Initialize:
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
                subaddress = payload;
                if (server_subaddress_link(server, subaddress, session[sessionID].subaddress_data) == -1)
                {
                    error_printf("Unable to link subaddress\n");
                    // TODO: Respond FatalError
                    continue;
                }

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

                break;

            case AsyncLock:
                debug_printf("Received AsyncLock message!\n");

                received_SessionID = msg_header.parameter;
                debug_printf("Received SessionID = %d\n", received_SessionID);

                if (msg_header.control_code == CC_REQUEST) {
                    control_code = CC_REQUEST_RSP_SUCCESS;
                } else if (msg_header.control_code == CC_RELEASE) {
                    control_code = CC_RELEASE_RSP_SUCCESS_SHARED;
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

            case InitializeResponse:
                break;

            case AsyncInitialize:
                debug_printf("Received AsyncInitialize message!\n");

                received_SessionID = msg_header.parameter;
                debug_printf("Received SessionID = %d\n", received_SessionID);

                // Construct AsyncInitializeResponse message including
                //  Server-vendorID
                parameter = HISLIP_VENDOR_ID;
                msg_create(&message, AsyncInitializeResponse, 0, parameter, 0, NULL);

                // Send InitializeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncInitializeResponse message\n");

                break;

            case AsyncInitializeResponse:
                break;

            case Data:
            {
                // FIXME: Accumulate payload
                message_id = msg_header.parameter;
                debug_printf("Received Data message (message ID = %d)\n", message_id);

                hs_subaddress_data_t *subaddress_data = server->subaddress_data;

                if (subaddress_data->callbacks->message_sync != NULL)
                {
                    subaddress_data->callbacks->message_sync(socket, message_id, payload, msg_header.payload_length, timeout);
                }
            }
                break;

            case DataEnd:
            {
                // FIXME: Allocate memory for full payload and copy payloads
                // accumulated
                message_id = msg_header.parameter;
                debug_printf("Received Data message (message ID = %d)\n", message_id);

                hs_subaddress_data_t *subaddress_data = server->subaddress_data;

                if (subaddress_data->callbacks->message_sync != NULL)
                {
                    subaddress_data->callbacks->message_sync(socket, message_id, payload, msg_header.payload_length, timeout);
                }
            }
                break;

            case AsyncMaximumMessageSize:
                debug_printf("Received AsyncMaximumMessageSize message!\n");

                received_SessionID = msg_header.parameter;
                debug_printf("Received SessionID = %d\n", received_SessionID);
                // TODO: find session by sessionID

                uint64_t *size_p = (uint64_t *)payload;
                uint64_t size = ntohll(*size_p);
                debug_printf("(Server) AsyncMaximumMessageSize message (size = %ld)\n", size);
                size = ntohll(server->config->message_size_max);
                msg_create(&message, AsyncMaximumMessageSizeResponse, 0, received_SessionID, 8, &size);

                // Send AsyncMaximumMessageSizeResponse message
                msg_send(socket, message, timeout);
                free(message);

                debug_printf("Sent AsyncMaximumMessageSizeResponse message\n");

                break;

            case AsyncMaximumMessageSizeResponse:
                break;

            case Error:
                break;
            case FatalError:
                break;
            default:
                error_printf("Unkown message type: %u!!!\n", msg_header.type);
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
    server->tcp_start(server->config->port, server->config->connections_max, connection_callback, server);

    return 0;
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

EXPORT int hs_server_init(hs_server_t *server, hs_server_config_t *config)
{
    if (config->message_size_max < MSG_HEADER_SIZE)
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

    return 0;
}

EXPORT int hs_server_register_subaddress(hs_server_t *server, char *subaddress, hs_subaddress_callbacks_t *callbacks)
{
    // Add subaddres to list of registered subaddresses
    server->subaddress_data = malloc(sizeof(hs_subaddress_data_t));
    if (server->subaddress_data == NULL)
    {
        error_printf("Could not allocated space for new subaddress\n");
        return -1;
    }

    // Install subaddress data
    server->subaddress_data->callbacks = callbacks;
    server->subaddress_data->subaddress = subaddress;

    // Add to list
    LIST_INSERT_HEAD(subaddress_head, server->subaddress_data, entries);

    return 0;
}

EXPORT int hs_server_send_response(int socket, uint32_t message_id, void *data, int length, int timeout)
{
    // Create DataEnd response message
    void *message = NULL;

    msg_create(&message, DataEnd, CC_RMT_DELIVERED, message_id, length, data);

    // Send DataEnd response message
    msg_send(socket, message, timeout);
    free(message);

    debug_printf("Sent DataEnd response message (message ID = %d)\n", message_id);

    return 0;
}
