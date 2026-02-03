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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#include "message.h"
#include "tcp.h"
#include "print.h"

struct
{
    int value;
    const char* message;
}
fatal_error_codes[] =
{
    { FATAL_ERROR_UNIDENTIFIED, "Unidentified error" },
    { FATAL_ERROR_POORLY_FORMED_MESSAGE_HEADER, "Poorly formed message header" },
    { FATAL_ERROR_CONNECTION_NOT_READY, "Attempt to use connection without both channels established" },
    { FATAL_ERROR_INVALID_INIT_SEQUENCE, "Invalid initialization sequence" },
    { FATAL_ERROR_TOO_MANY_CLIENTS, "Server refused connection due to maximum numver of clients exceeded" },
    { 0, 0 }
};

struct
{
    int value;
    const char* message;
}
error_codes[] =
{
    { ERROR_UNIDENTIFIED, "Unidentified error" },
    { ERROR_UNRECOGNIZED_MESSAGE_TYPE, "Unrecognized message type" },
    { ERROR_UNRECOGNIZED_CONTROL_CODE, "Unrecognized control code" },
    { ERROR_UNRECOGNIZED_VENDOR_DEFINED_MESSAGE, "Unrecognized vendor defined message" },
    { ERROR_MESSAGE_TOO_LARGE, "Message too large" },
    { 0, 0 }
};

const char* fatal_error_to_msg(int code)
{
    for (int i = 0; fatal_error_codes[i].message; ++i)
    {
        if (fatal_error_codes[i].value == code)
        {
            return fatal_error_codes[i].message;
        }
    }
    return "Unknown fatal error";
}

const char* error_to_msg(int code)
{
    for (int i = 0; error_codes[i].message; ++i)
    {
        if (error_codes[i].value == code)
        {
            return error_codes[i].message;
        }
    }
    return "Unknown error";
}


int msg_header_verify(msg_header_t *header)
{
    // Verify message prefix
    if (header->prologue != MSG_HEADER_PROLOGUE)
    {
        error_printf("Received invalid message header (invalid prologue)\n");
        return 1;
    }

    return 0;
}

int msg_create(
        void **message,
        msg_type_t type,
        uint8_t control_code,
        uint32_t parameter,
        uint64_t payload_length,
        void *payload)
{
    msg_header_t *header;
    char *payload_p;

    // Allocate memory for message buffer
    *message = malloc(MSG_HEADER_SIZE + payload_length);
    if (*message == NULL)
    {
        error_printf("Failed to allocate memory for message\n");
        return -1;
    }

    // Create message header
    header = *message;
    header->prologue = MSG_HEADER_PROLOGUE;
    header->type = type;
    header->control_code = control_code;
    header->parameter = parameter;
    header->payload_length = payload_length;

    // Convert multi-byte values to network byte order (big endian)
    header->prologue = htons(header->prologue);
    header->parameter = htonl(header->parameter);
    header->payload_length = htonll(header->payload_length);

    // Copy payload if any
    if (payload_length > 0)
    {
        payload_p = *message;
        memcpy(payload_p + MSG_HEADER_SIZE, payload, payload_length);
    }

    return 0;
}

void msg_destroy(void *message)
{
    free(message);
}

int msg_receive(int socket, void **message, uint64_t payload_size_max, int timeout)
{
    msg_header_t header;
    int bytes_read = 0, bytes_left_to_read = 0;
    char *payload_p = NULL;

    // Enter message receive loop
    bytes_left_to_read = MSG_HEADER_SIZE;
    while (bytes_left_to_read > 0)
    {
        // Receive message header (blocking until data available)
        if ((bytes_read = tcp_read(socket, &header, bytes_left_to_read, timeout)) == 0)
        {
            debug_printf("Server closed connection (1)\n");
            tcp_disconnect(socket);
            return -1;
        }
        bytes_left_to_read -= bytes_read;
    }

    // Convert header multi-byte values to host byte order
    header.prologue = ntohs(header.prologue);
    header.parameter = ntohl(header.parameter);
    header.payload_length = ntohll(header.payload_length);

    debug_printf("Received message:\n");
    debug_printf(" prologue = %d\n", header.prologue);
    debug_printf(" type = %d\n", header.type);
    debug_printf(" control_code = %d\n", header.control_code);
    debug_printf(" parameter = %d\n", header.parameter);
    debug_printf(" payload_length = %ld\n", header.payload_length);

    // Verify message header
    if (msg_header_verify(&header))
    {
        // Invalid header
        error_printf("Invalid header\n");

        // Send FatalError message with error code 1 (Poorly formed message header)
        //msg_send(FatalError, 1, 0, error_string(1), error_string_length(1));
        return -1;
    }

    if (header.type == FatalError)
    {
        error_printf("%s\n", fatal_error_to_msg(header.control_code));
        return -1;
    }
    else if (header.type == Error)
    {
        error_printf("%s\n", error_to_msg(header.control_code));
        return -1;
    }

    // Check payload size
    if (header.payload_length > payload_size_max)
    {
        error_printf("Maximum client payload size exceeded\n");
        return -1;
    }

    // Allocate message (header + payload) receive buffer
    *message = malloc(MSG_HEADER_SIZE + header.payload_length);
    if (*message == NULL)
    {
        error_printf("malloc() failed\n");
        return -1;
    }

    // Install header
    memcpy(*message, &header, MSG_HEADER_SIZE);

    // Receive any payload
    if (header.payload_length > 0)
    {
        // Read until payload received
        bytes_left_to_read = header.payload_length;
        payload_p = *message + MSG_HEADER_SIZE;

        while (bytes_left_to_read > 0)
        {
            if ((bytes_read = tcp_read(socket, payload_p, bytes_left_to_read, timeout)) <= 0)
            {
                debug_printf("Server closed connection (2)\n");
                tcp_disconnect(socket);
                free(*message);
                return -1;
            }
            bytes_left_to_read -= bytes_read;
        }
    }

    return 0;
}

int msg_send(int socket, void *message, int timeout)
{
    msg_header_t *header = message;
    int length = MSG_HEADER_SIZE + ntohll(header->payload_length);

    debug_printf("Sending message:\n");
    debug_printf(" prologue = %d\n", ntohs(header->prologue));
    debug_printf(" type = %d\n", header->type);
    debug_printf(" control_code = %d\n", header->control_code);
    debug_printf(" parameter = %d\n", ntohl(header->parameter));
    debug_printf(" payload_length = %ld\n", ntohll(header->payload_length));

    // Send message
    return tcp_write(socket, message, length, timeout);
}
