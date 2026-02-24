#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <hislip/server.h>

void *sync_data = NULL;
int payload_accumulated_size = 0;

int hislip0_message_sync(hs_msg_ctx_t *msg_ctx, void *buffer, int length, bool end)
{
    char *data;
    char *response_buffer;
    int response_length;

    if (length > 0)
    {
        printf("Received command: %s\n", (char *) buffer);
    }

    if (payload_accumulated_size == 0) {
        data = malloc(length);
    } else {
        data = realloc(sync_data, payload_accumulated_size + length);
    }
    if (data == NULL) {
        payload_accumulated_size = 0;
        if (sync_data != NULL) {
            free(sync_data);
            sync_data = NULL;
        }
        // TODO: respond error?
        return -1;
    }
    sync_data = data;

    memcpy(sync_data+payload_accumulated_size, buffer, length);
    payload_accumulated_size += length;

    if (end) {

        if (strcmp(sync_data, "*IDN?\n") == 0)
        {
            response_buffer = "WOPR Computer,2026,A0123456789,V0.0.1\n";
            response_length = strlen(response_buffer);
        }
        else if (strcmp(sync_data, "MMEM:DATA?\n") == 0)
        {
            response_buffer = "WOPR Computer,2026,A0123456789,V0.0.1\n";
            response_length = 3 * 1024 * 1024;
        }
        else if (strcmp(sync_data, "*CLS;*IDN?\n") == 0)
        {
            response_buffer = "WOPR Computer,2026,A0123456789,V0.0.1\n";
            response_length = strlen(response_buffer);
        }
        else
        {
            response_buffer = NULL;
            response_length = 0;
        }

        payload_accumulated_size = 0;
        free(sync_data);
        sync_data = NULL;

        return hs_server_send_response(msg_ctx, response_buffer, response_length);
    }

    return 0;
}

int hislip0_message_async(hs_msg_ctx_t *msg_ctx, void *buffer, int length, bool end)
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
