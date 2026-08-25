#include "bridge.h"

#include <jansson.h>

#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size > 8192u)
        return 0;

    int descriptors[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0)
        return 0;

    size_t offset = 0u;
    while (offset < size) {
        ssize_t written = write(descriptors[0], data + offset, size - offset);
        if (written <= 0)
            break;
        offset += (size_t)written;
    }
    (void)write(descriptors[0], "\n", 1u);
    (void)shutdown(descriptors[0], SHUT_WR);

    json_t *frame = NULL;
    char *error = NULL;
    (void)maelys_bridge_read_initial(descriptors[1], 8192u, &frame, &error);
    json_decref(frame);
    free(error);
    (void)close(descriptors[0]);
    (void)close(descriptors[1]);
    return 0;
}
