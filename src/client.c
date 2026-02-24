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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <hislip/client.h>
#include "tcp.h"
#include "session.h"
#include "print.h"
#include "message.h"

EXPORT hs_device_t hs_connect(char *address, int port, char *subaddress, int timeout)
{
    int sfd_sync, sfd_async, device;
    void *message = NULL;
    uint16_t version;
    uint32_t parameter;

    // Create new device session
    device = session_new();
    if (device < 0)
    {
        error_printf("Could not allocate new session!\n");
        goto error_session;
    }

    // Create TCP connection for sync channel
    if (tcp_connect(&sfd_sync, address, port, timeout) != 0)
    {
        goto error_connect_sync;
    }

    debug_printf("Connected to server on %s port %d\n", address, port);
    debug_printf("Device session ID = %d\n", device);

    // Save sync channel device socket descriptor
    session[device].socket_sync = sfd_sync;

    // Create Initialize message to setup sync channel
    version = (HISLIP_VERSION_MAJOR << 8) + HISLIP_VERSION_MINOR;
    parameter = (version << 16) + HISLIP_VENDOR_ID;

    if (subaddress == NULL)
    {
        // Subaddress of NULL the initialize opens the default (perhaps only)
        // device at the IP address
        msg_create(&message, Initialize, 0, parameter, 0, NULL);
    }
    else
    {
        msg_create(&message, Initialize, 0, parameter, strlen(subaddress), subaddress);
    }

    // Send Initialize message
    if (msg_send(sfd_sync, message, timeout) == -1)
    {
        goto error_send_Initialize;
    }

    debug_printf("Sent Initialize message\n");

    // Free memory of sent message
    free(message);

    // Wait for InitializeResponse message
    int max_payload_size = 1000000;

    if (msg_receive(sfd_sync, &message, max_payload_size, timeout) == -1)
    {
        goto error_receive_InitializeResponse;
    }
    msg_header_t *header = message;

    if (header->type != InitializeResponse)
    {
        goto error_InitializeResponse;
    }

    debug_printf("Received InitializeResponse message\n");

    // Save received server SessionID
    session[device].SessionID = header->parameter &= 0xFFFF;
    debug_printf("Received SessionID = %d\n", session[device].SessionID);
    free(message);

    // Create TCP connection for async channel
    if (tcp_connect(&sfd_async, address, port, timeout) != 0)
    {
        goto error_connect_async;
    }

    // Save async channel device socket descriptor
    session[device].socket_async = sfd_async;

    // Create AsyncInitialize message to setup async channel
    parameter = session[device].SessionID;
    msg_create(&message, AsyncInitialize, 0, parameter, 0, NULL);

    // Send AsyncInitialize message on async channel
    if (msg_send(sfd_async, message, timeout) == -1)
    {
        goto error_send_AsyncInitialize;
    }

    debug_printf("Sent AsyncInitialize message\n");

    // Free memory of sent message
    free(message);

    // Wait for AsyncInitializeResponse message
    if (msg_receive(sfd_async, &message, max_payload_size, timeout) == -1)
    {
        goto error_receive_AsyncInitializeResponse;
    }
    header = message;

    if (header->type != AsyncInitializeResponse)
    {
        error_printf("Expected AsyncInitializeResponse message\n");
        goto error_async_InitializeResponse;
    }

    debug_printf("Received AsyncInitializeResponse message\n");

    uint16_t vendor_id = header->parameter;

    debug_printf("Vendor ID = %d\n", vendor_id);

    free(message);

    // Return device session handle
    return device;

error_async_InitializeResponse:
    free(message);
error_receive_AsyncInitializeResponse:
error_send_AsyncInitialize:
    tcp_disconnect(sfd_async);
error_connect_async:
    tcp_disconnect(sfd_sync);
error_InitializeResponse:
error_receive_InitializeResponse:
error_send_Initialize:
error_connect_sync:
    session_free(device);
error_session:
    return -1;
}

EXPORT int hs_disconnect(hs_device_t device)
{
    if (session[device].socket_sync != -1)
    {
        tcp_disconnect(session[device].socket_sync);
    }

    if (session[device].socket_async != -1)
    {
        tcp_disconnect(session[device].socket_async);
    }

    session_free(device);

    return 0;
}


EXPORT uint64_t hs_sync_send(hs_device_t device, void *data, uint64_t length, int timeout)
{
    void *message = NULL;
    uint8_t control_code;
    uint32_t parameter;
    int socket = session[device].socket_sync;
    uint64_t message_payload_max = session[device].server_message_size_max - MSG_HEADER_SIZE;
    int64_t message_bytes_sent;

    // Calculate how many message bytes to send
    uint64_t message_bytes_remaining = length;

    control_code = CC_RMT_DELIVERED;
    while (message_bytes_remaining)
    {
        // Create Data message
        parameter = session[device].message_id;

        // Increment message ID by 2 according to spec
        session[device].message_id = session[device].message_id + 2;

        if (message_bytes_remaining > message_payload_max)
        {
            msg_create(&message, Data, control_code, parameter, message_payload_max, data);
            debug_printf("Sending Data message (message ID = %d)\n", parameter);
        }
        else
        {
            msg_create(&message, DataEnd, control_code, parameter, message_bytes_remaining, data);
            debug_printf("Sending DataEnd message (message ID = %d)\n", parameter);
        }

        message_bytes_sent = msg_send(socket, message, timeout);
        free(message);
        if (message_bytes_sent < 0)
        {
            // Throw fatal error
            // return -1;
        }

        message_bytes_remaining -= (message_bytes_sent - MSG_HEADER_SIZE);

        control_code = CC_RMT_NOT_DELIVERED;
    }

    return length;
}

EXPORT uint64_t hs_sync_receive(hs_device_t device, void *data, uint64_t length, int timeout)
{
    msg_header_t *header;
    void *message = NULL;
    int socket = session[device].socket_sync;
    char *payload_p;
    uint64_t payload_length, message_bytes_recv = 0, message_bytes_remaining = length;

    // Receive loop - receive Data messages and accumulate payload until DataEnd
    // message is received.
    while (1)
    {
        // Wait for message
        if (msg_receive(socket, &message, session[device].client_message_size_max, timeout) == -1)
        {
            return -1;
        }
        header = message;

        // Push payload on FIFO
        payload_p = message;
        payload_p += MSG_HEADER_SIZE;
        payload_length = header->payload_length;

        if (message_bytes_remaining > 0) {
            if (payload_length > message_bytes_remaining)
            {
                memcpy(data+message_bytes_recv, payload_p, message_bytes_remaining);
                message_bytes_remaining = 0;
            } else {
                memcpy(data+message_bytes_recv, payload_p, payload_length);
                message_bytes_remaining -= payload_length;
            }
        }

        message_bytes_recv += payload_length;

        if (header->type == DataEnd)
        {
            debug_printf("Received DataEnd message (message ID = %d)\n", header->parameter);
            free(message);
            break;
        }
        debug_printf("Received Data message (message ID = %d)\n", header->parameter);
        free(message);
    }

    return message_bytes_recv;
}

EXPORT uint64_t hs_sync_send_receive(hs_device_t device, void *data, uint64_t length, int timeout,
        void (*receive_callback)(void *message, uint64_t length))
{

    // Send SCPI command on sync channel
    hs_sync_send(device, data, length, timeout);

    // Receive response message
    hs_sync_receive(device, data, 200, 1000);

    return 0;
}

EXPORT uint64_t hs_set_maximum_message_size(hs_device_t device, uint64_t size, int timeout)
{
    void *message = NULL;
    int socket = session[device].socket_async;
    msg_header_t *header;
    char *payload_p;

    // Update client message size
    session[device].client_message_size_max = size;

    debug_printf("Sending AsyncMaximumMessageSize message (size = %ld)\n", size);

    // Create AsyncMaximumMessageSize message
    size = htonll(size);
    msg_create(&message, AsyncMaximumMessageSize, 0, 0, 8, &size);

    // Send message
    if (msg_send(socket, message, timeout) == -1)
    {
        return 0;
    }

    // Wait for message
    if (msg_receive(socket, &message, 8, timeout) == -1)
    {
        return -1;
    }
    header = message;

    if (header->type != AsyncMaximumMessageSizeResponse)
    {
        error_printf("Expected AsyncMaximumMessageSizeResponse message\n");
        free(message);
        return -1;
    }

    payload_p = message;
    payload_p += MSG_HEADER_SIZE;
    uint64_t *size_p = (uint64_t *)payload_p;
    size = ntohll(*size_p);

    debug_printf("Received AsyncMaximumMessageSizeResponse message (size = %ld)\n", size);

    free(message);

    // Update server message size
    session[device].server_message_size_max = size;

    return size;
}
