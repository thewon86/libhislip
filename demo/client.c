#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <hislip/client.h>

static void receive_handler(void *buffer, int length)
{
    printf("Received: %s\n", (char *)buffer);
}

int main(void)
{
    hs_device_t hislip0;

    // Connect to HiSLIP device
    //hislip0 = hs_connect("127.0.0.1", HISLIP_PORT, "hislip0", 1000);
    hislip0 = hs_connect("192.168.0.117", HISLIP_PORT, "hislip0", 1000);
    if (hislip0 < 0)
    {
        fprintf(stderr, "Error: Connect failure\n");
        return -1;
    }

    // Set maximum message size
    uint64_t server_size = hs_set_maximum_message_size(hislip0, 516, 1000);
    printf("Server maximum message size = %ld\n", server_size);

    // Send SCPI command on sync channel
    char buffer[200] = "*IDN?";
    printf("Send buffer = %s\n", buffer);
    hs_sync_send(hislip0, buffer, strlen(buffer), 1000);

    // Receive response message
    hs_sync_receive(hislip0, buffer, 200, 1000);
    printf("Receive buffer = %s\n", buffer);

    // Send SCPI command on sync channel
    // hs_sync_send_receive(hislip0, buffer, strlen(buffer), 1000, receive_handler);

//    printf("Press any key to quit\n");
//    getchar();

    // Disconnect
    hs_disconnect(hislip0);

    return 0;
}
