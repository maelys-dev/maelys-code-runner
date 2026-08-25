#pragma once

#include <jansson.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct maelys_bridge maelys_bridge_t;

typedef enum maelys_bridge_result {
    MAELYS_BRIDGE_OK = 0,
    MAELYS_BRIDGE_CANCELLED = 1,
    MAELYS_BRIDGE_CLOSED = 2,
    MAELYS_BRIDGE_ERROR = 3
} maelys_bridge_result_t;

int maelys_bridge_read_initial(int fd, size_t max_frame_bytes, json_t **out_frame,
                               char **out_error);

int maelys_bridge_create(int fd, size_t max_frame_bytes, maelys_bridge_t **out_bridge,
                         char **out_error);
void maelys_bridge_destroy(maelys_bridge_t *bridge);

int maelys_bridge_start(maelys_bridge_t *bridge, char **out_error);
void maelys_bridge_request_cancel(maelys_bridge_t *bridge);
int maelys_bridge_is_cancelled(const maelys_bridge_t *bridge);
int maelys_bridge_has_failed(const maelys_bridge_t *bridge);

maelys_bridge_result_t maelys_bridge_wait_frame(maelys_bridge_t *bridge, json_t **out_frame,
                                                char **out_error);

int maelys_bridge_send(maelys_bridge_t *bridge, const json_t *frame, char **out_error);
