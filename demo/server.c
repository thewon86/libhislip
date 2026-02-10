#include <stdio.h>
#include <string.h>
#include <hislip/server.h>

int hislip0_message_sync(int socket, int sessionID, uint32_t message_id, void *buffer, int length, int timeout)
{
    char *response_buffer;
    int response_length;

    if (length > 0)
    {
        printf("Received command: %s\n", (char *) buffer);
    }

    if (strcmp(buffer, "*IDN?\n") == 0)
    {
        response_buffer = "WOPR Computer,2026,A0123456789,V0.0.1\n";
        response_length = strlen(response_buffer);
    }
    else
    {
        response_buffer = NULL;
        response_length = 0;
    }

    return hs_server_send_response(socket, sessionID, message_id, response_buffer, response_length, timeout);
}

int hislip0_message_async(int socket, uint32_t message_id, void *buffer, int length, int timeout)
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
    config.message_size_max = 516; // Header + 500 bytes
    config.message_timeout = 2000; // 2 seconds

    // Initialize server
    hs_server_init(&server, config);

    // Register server message handlers
    hislip0_callbacks.message_sync = hislip0_message_sync;
    hislip0_callbacks.message_async = hislip0_message_async;
    hs_server_register_subaddress(&server, "hislip0", hislip0_callbacks);
    hs_server_register_subaddress(&server, "hislip1", hislip0_callbacks);
    hs_server_register_subaddress(&server, "hislip2", hislip0_callbacks);

    // Start server
    status = hs_server_run(&server);

    return status;
}
