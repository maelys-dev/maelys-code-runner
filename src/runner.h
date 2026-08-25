#pragma once

#include <jansson.h>

#include <stddef.h>
#include <stdint.h>

typedef struct maelys_runner_limits {
    size_t memory_bytes;
    size_t stack_bytes;
    size_t max_frame_bytes;
    size_t max_result_bytes;
    uint64_t timeout_ms;
    uint64_t max_calls;
} maelys_runner_limits_t;

int maelys_runner_execute(int bridge_fd, json_t *request, const maelys_runner_limits_t *limits,
                          char **out_error);

int maelys_runner_install_signal_handlers(char **out_error);
