#include "bridge.h"
#include "runner.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    (void)fprintf(stream,
                  "usage: maelys-code-runner [OPTIONS]\n"
                  "\n"
                  "Reads one code-bridge-v1 execute frame and serves the execution on fd 3.\n"
                  "\n"
                  "  --fd N                 bridge descriptor (default: 3)\n"
                  "  --memory-bytes N       QuickJS heap limit (default: 67108864)\n"
                  "  --stack-bytes N        QuickJS stack limit (default: 2097152)\n"
                  "  --timeout-ms N         wall-clock deadline (default: 5000)\n"
                  "  --max-calls N          tool-call limit (default: 64)\n"
                  "  --max-frame-bytes N    JSON Lines frame limit (default: 1048576)\n"
                  "  --max-result-bytes N   final result limit (default: 524288)\n"
                  "  --version              print the runner version\n"
                  "  --help                 show this help\n");
}

static int parse_u64(const char *text, uint64_t minimum, uint64_t *out_value) {
    if (!text || !*text || text[0] == '-')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < minimum)
        return -1;
    *out_value = (uint64_t)value;
    return 0;
}

int main(int argc, char **argv) {
    int bridge_fd = 3;
    maelys_runner_limits_t limits = {.memory_bytes = 64u * 1024u * 1024u,
                                     .stack_bytes = 2u * 1024u * 1024u,
                                     .max_frame_bytes = 1024u * 1024u,
                                     .max_result_bytes = 512u * 1024u,
                                     .timeout_ms = 5000u,
                                     .max_calls = 64u};
    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        if (strcmp(option, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(option, "--version") == 0) {
            (void)puts(MAELYS_CODE_RUNNER_VERSION);
            return 0;
        }
        if (index + 1 >= argc) {
            (void)fprintf(stderr, "%s requires a value\n", option);
            return 2;
        }
        uint64_t parsed = 0u;
        if (parse_u64(argv[++index], 1u, &parsed) != 0) {
            (void)fprintf(stderr, "invalid value for %s\n", option);
            return 2;
        }
        if (strcmp(option, "--fd") == 0 && parsed <= INT32_MAX) {
            bridge_fd = (int)parsed;
        } else if (strcmp(option, "--memory-bytes") == 0 && parsed <= SIZE_MAX) {
            limits.memory_bytes = (size_t)parsed;
        } else if (strcmp(option, "--stack-bytes") == 0 && parsed <= SIZE_MAX) {
            limits.stack_bytes = (size_t)parsed;
        } else if (strcmp(option, "--timeout-ms") == 0) {
            limits.timeout_ms = parsed;
        } else if (strcmp(option, "--max-calls") == 0) {
            limits.max_calls = parsed;
        } else if (strcmp(option, "--max-frame-bytes") == 0 && parsed < SIZE_MAX) {
            limits.max_frame_bytes = (size_t)parsed;
        } else if (strcmp(option, "--max-result-bytes") == 0 && parsed <= SIZE_MAX) {
            limits.max_result_bytes = (size_t)parsed;
        } else {
            (void)fprintf(stderr, "unknown option or out-of-range value: %s\n", option);
            return 2;
        }
    }

    char *error = NULL;
    if (maelys_runner_install_signal_handlers(&error) != 0) {
        (void)fprintf(stderr, "%s\n", error ? error : "cannot install signal handlers");
        free(error);
        return 2;
    }
    json_t *request = NULL;
    if (maelys_bridge_read_initial(bridge_fd, limits.max_frame_bytes, &request, &error) != 0) {
        (void)fprintf(stderr, "%s\n", error ? error : "cannot read execute frame");
        free(error);
        return 2;
    }
    int result = maelys_runner_execute(bridge_fd, request, &limits, &error);
    json_decref(request);
    if (result != 0) {
        (void)fprintf(stderr, "%s\n", error ? error : "runner execution failed");
        free(error);
        return 1;
    }
    free(error);
    return 0;
}
