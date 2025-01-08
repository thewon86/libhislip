# libhislip - A client/server HiSLIP library

## STATUS: WORK IN PROGRESS (PRE-ALPHA)

## 1. Introduction

The HiSLIP library (libhislip) is an open source library for GNU/Linux systems
which offers a client and server API for communicating with clients and servers
that speak the HiSLIP (High Speed LAN Instrument) protocol.

The implemention supports sending/receiving messages on synchronous and
asynchronous HiSLIP communication channels.

The library implements the HiSLIP protocol v1.1 as described
[here](http://ivifoundation.org/downloads/Protocol%20Specifications/IVI-6.1_HiSLIP-1.1-2011-02-24.pdf).

## 2. The libhislip API

The API includes the following functions:

To be done

## 3. API usage

The following are simple code examples demonstrating how to use the libhislip
API.

### 3.1 Server example

```c
#include <stdio.h>
#include <string.h>
#include <hislip/server.h>

int hislip0_message_sync(int socket,
                         uint32_t message_id,
                         void *buffer,
                         int length,
                         int timeout)
{
    char *response_buffer;
    int response_length;

    if (length > 0)
    {
        printf("Received command: %s\n", (char *) buffer);
    }

    if (strncmp(buffer, "*IDN?", length) == 0)
    {
        response_buffer = "WOPR Computer";
        response_length = strlen(response_buffer);
    }
    else
    {
        response_buffer = NULL;
        response_length = 0;
    }

    return hs_server_send_response(socket,
                                   message_id,
                                   response_buffer,
                                   response_length,
                                   timeout);
}

int hislip0_message_async(int socket,
                          uint32_t message_id,
                          void *buffer,
                          int length,
                          int timeout)
{
    return 0;
}

int main(void)
{
    int status;
    hs_server_t server;
    hs_server_config_t config;
    hs_subaddress_callbacks_t hislip0_callbacks;

    // Initialize server configuration
    hs_server_config_init(&config);

    // Configure server
    config.connections_max = 10;
    config.message_size_max = 516; // Header + 500 bytes payload
    config.message_timeout = 2000; // 2 seconds

    // Initialize server
    hs_server_init(&server, &config);

    // Register server message handlers
    hislip0_callbacks.message_sync = hislip0_message_sync;
    hislip0_callbacks.message_async = hislip0_message_async;
    hs_server_register_subaddress(&server, "hislip0", &hislip0_callbacks);

    // Start server
    status = hs_server_run(&server);

    return status;
}
```

### 3.2 Client example

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hislip/client.h>

static void receive_handler(void *buffer, int length)
{
    printf("Received: %s\n", (char *)buffer);
}

int main(void)
{
    hs_client_t hislip0;

    // Connect to HiSLIP server
    hislip0 = hs_connect("127.0.0.1", HISLIP_PORT, "hislip0", 1000);
    if (hislip0 < 0)
    {
        fprintf(stderr, "Error: Connect failure\n");
        return -1;
    }

    // Send SCPI command on sync channel
    char buffer[200] = "*IDN?";
    printf("Send buffer = %s\n", buffer);
    hs_sync_send(hislip0, buffer, strlen(buffer), 1000);

    // Receive response message
    hs_sync_receive(hislip0, buffer, 200, 1000);
    printf("Receive buffer = %s\n", buffer);

    printf("Press any key to quit\n");
    getchar();

    // Disconnect
    hs_disconnect(hislip0);

    return 0;
}
```

## 4. Installation

Install steps:

```meson
 $ meson build
 $ meson -C build compile
 $ meson -C build install
```

See meson\_options.txt for which features to enable/disable.

Note: The meson install steps may differ depending on your specific system
environment.

## 5. Contributing

libhislip is open source. If you want to help out with the project please join
in. Any contributions (bug reports, bug fixes, code, doc, ideas, etc.) are welcome.

Please use the github issue tracker and pull request features.

## 6. Website

Visit https://lxi-tools.github.io

## 7. License

The libhislip code is covered by BSD-3, commonly known as the 3-clause (or
"modified") BSD license

For license details please see the LICENSE file.
