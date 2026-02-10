#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <hislip/client.h>

static char resp_buffer[1024 * 1024];

static void receive_handler(void *buffer, int length)
{
    printf("Received: %s\n", (char *)buffer);
}

int main(void)
{
    hs_device_t hislip0;
    uint64_t ret;

    // Connect to HiSLIP device
    hislip0 = hs_connect("127.0.0.1", HISLIP_PORT, "hislip0", 1000);
//    hislip0 = hs_connect("192.168.0.117", HISLIP_PORT, "hislip0", 1000);
    if (hislip0 < 0)
    {
        fprintf(stderr, "Error: Connect failure\n");
        return -1;
    }

    // Set maximum message size
    uint64_t server_size = hs_set_maximum_message_size(hislip0, 516, 1000);
    printf("Server maximum message size = %ld\n", server_size);

    // Send SCPI command on sync channel
    char buffer[200] = "*IDN?\n";
    printf("Send buffer = %s\n", buffer);
    hs_sync_send(hislip0, buffer, strlen(buffer), 1000);

    // Receive response message
    ret = hs_sync_receive(hislip0, resp_buffer, sizeof(resp_buffer), 1000);
    resp_buffer[ret] = '\0';
    printf("Received = %d %.*s\n", ret, ret, resp_buffer);

    // Send SCPI command on sync channel
    // hs_sync_send_receive(hislip0, buffer, strlen(buffer), 1000, receive_handler);

//    printf("Press any key to quit\n");
//    getchar();

    // Disconnect
    hs_disconnect(hislip0);

    return 0;
}
