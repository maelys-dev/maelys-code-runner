#include "bridge.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct frame_node {
    json_t *frame;
    struct frame_node *next;
} frame_node_t;

enum { MAELYS_BRIDGE_MAX_QUEUED_FRAMES = 8 };

struct maelys_bridge {
    int fd;
    size_t max_frame_bytes;
    pthread_t reader;
    int reader_started;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    frame_node_t *head;
    frame_node_t *tail;
    size_t queued_frames;
    atomic_bool cancelled;
    atomic_bool failed;
    int closed;
    char *reader_error;
};

static void set_error(char **out_error, const char *message) {
    if (!out_error || *out_error)
        return;
    *out_error = strdup(message ? message : "bridge error");
}

static void set_errno_error(char **out_error, const char *operation) {
    if (!out_error || *out_error)
        return;
    const char *detail = strerror(errno);
    size_t size = strlen(operation) + strlen(detail) + 3u;
    char *message = malloc(size);
    if (!message)
        return;
    (void)snprintf(message, size, "%s: %s", operation, detail);
    *out_error = message;
}

static int read_line(int fd, size_t maximum, char **out_line, size_t *out_size, char **out_error) {
    if (!out_line || !out_size || maximum == 0u || maximum == SIZE_MAX) {
        set_error(out_error, "invalid line reader arguments");
        return -1;
    }
    char *line = malloc(maximum + 1u);
    if (!line) {
        set_error(out_error, "out of memory allocating bridge frame");
        return -1;
    }
    size_t used = 0u;
    while (used < maximum) {
        char byte = '\0';
        ssize_t count = read(fd, &byte, 1u);
        if (count == 0) {
            free(line);
            set_error(out_error, "bridge closed before a complete frame");
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            free(line);
            set_errno_error(out_error, "read bridge");
            return -1;
        }
        if (byte == '\n') {
            line[used] = '\0';
            *out_line = line;
            *out_size = used;
            return 0;
        }
        if (byte == '\0') {
            free(line);
            set_error(out_error, "bridge frame contains an embedded NUL byte");
            return -1;
        }
        line[used++] = byte;
    }
    free(line);
    set_error(out_error, "bridge frame exceeds max_frame_bytes");
    return -1;
}

static int parse_line(const char *line, size_t size, json_t **out_frame, char **out_error) {
    json_error_t parse_error;
    json_t *frame = json_loadb(line, size, JSON_REJECT_DUPLICATES, &parse_error);
    if (!frame || !json_is_object(frame)) {
        json_decref(frame);
        if (out_error && !*out_error) {
            char message[256];
            (void)snprintf(message, sizeof(message),
                           "invalid JSON bridge frame at column %d: %.160s", parse_error.column,
                           parse_error.text);
            set_error(out_error, message);
        }
        return -1;
    }
    *out_frame = frame;
    return 0;
}

int maelys_bridge_read_initial(int fd, size_t max_frame_bytes, json_t **out_frame,
                               char **out_error) {
    if (fd < 0 || !out_frame) {
        set_error(out_error, "invalid initial bridge read arguments");
        return -1;
    }
    *out_frame = NULL;
    char *line = NULL;
    size_t size = 0u;
    if (read_line(fd, max_frame_bytes, &line, &size, out_error) != 0)
        return -1;
    int result = parse_line(line, size, out_frame, out_error);
    free(line);
    return result;
}

static int frame_is_cancel(const json_t *frame) {
    const json_t *version = json_object_get(frame, "version");
    const json_t *type = json_object_get(frame, "type");
    return json_is_string(version) && json_is_string(type) &&
           strcmp(json_string_value(version), "code-bridge-v1") == 0 &&
           strcmp(json_string_value(type), "cancel") == 0;
}

static void reader_fail(maelys_bridge_t *bridge, char *error) {
    atomic_store_explicit(&bridge->failed, true, memory_order_release);
    (void)pthread_mutex_lock(&bridge->mutex);
    bridge->closed = 1;
    if (!bridge->reader_error)
        bridge->reader_error = error;
    else
        free(error);
    (void)pthread_cond_broadcast(&bridge->condition);
    (void)pthread_mutex_unlock(&bridge->mutex);
}

static void *reader_main(void *opaque) {
    maelys_bridge_t *bridge = opaque;
    for (;;) {
        char *line = NULL;
        size_t size = 0u;
        char *error = NULL;
        if (read_line(bridge->fd, bridge->max_frame_bytes, &line, &size, &error) != 0) {
            reader_fail(bridge, error);
            return NULL;
        }
        json_t *frame = NULL;
        if (parse_line(line, size, &frame, &error) != 0) {
            free(line);
            reader_fail(bridge, error);
            return NULL;
        }
        free(line);
        if (frame_is_cancel(frame)) {
            atomic_store_explicit(&bridge->cancelled, true, memory_order_release);
            json_decref(frame);
            (void)pthread_mutex_lock(&bridge->mutex);
            (void)pthread_cond_broadcast(&bridge->condition);
            (void)pthread_mutex_unlock(&bridge->mutex);
            continue;
        }
        frame_node_t *node = calloc(1u, sizeof(*node));
        if (!node) {
            json_decref(frame);
            reader_fail(bridge, strdup("out of memory queueing bridge frame"));
            return NULL;
        }
        node->frame = frame;
        (void)pthread_mutex_lock(&bridge->mutex);
        if (bridge->queued_frames >= MAELYS_BRIDGE_MAX_QUEUED_FRAMES) {
            (void)pthread_mutex_unlock(&bridge->mutex);
            json_decref(node->frame);
            free(node);
            reader_fail(bridge, strdup("bridge receive queue exceeds its bounded capacity"));
            return NULL;
        }
        if (bridge->tail)
            bridge->tail->next = node;
        else
            bridge->head = node;
        bridge->tail = node;
        bridge->queued_frames++;
        (void)pthread_cond_signal(&bridge->condition);
        (void)pthread_mutex_unlock(&bridge->mutex);
    }
}

int maelys_bridge_create(int fd, size_t max_frame_bytes, maelys_bridge_t **out_bridge,
                         char **out_error) {
    if (fd < 0 || max_frame_bytes == 0u || !out_bridge) {
        set_error(out_error, "invalid bridge arguments");
        return -1;
    }
    maelys_bridge_t *bridge = calloc(1u, sizeof(*bridge));
    if (!bridge) {
        set_error(out_error, "out of memory creating bridge");
        return -1;
    }
    bridge->fd = fd;
    bridge->max_frame_bytes = max_frame_bytes;
    atomic_init(&bridge->cancelled, false);
    atomic_init(&bridge->failed, false);
    if (pthread_mutex_init(&bridge->mutex, NULL) != 0) {
        free(bridge);
        set_error(out_error, "cannot initialize bridge synchronization");
        return -1;
    }
    if (pthread_cond_init(&bridge->condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&bridge->mutex);
        free(bridge);
        set_error(out_error, "cannot initialize bridge synchronization");
        return -1;
    }
    *out_bridge = bridge;
    return 0;
}

int maelys_bridge_start(maelys_bridge_t *bridge, char **out_error) {
    if (!bridge || bridge->reader_started) {
        set_error(out_error, "bridge is missing or already started");
        return -1;
    }
    if (pthread_create(&bridge->reader, NULL, reader_main, bridge) != 0) {
        set_error(out_error, "cannot start bridge reader thread");
        return -1;
    }
    bridge->reader_started = 1;
    return 0;
}

void maelys_bridge_request_cancel(maelys_bridge_t *bridge) {
    if (!bridge)
        return;
    atomic_store_explicit(&bridge->cancelled, true, memory_order_release);
    (void)pthread_mutex_lock(&bridge->mutex);
    (void)pthread_cond_broadcast(&bridge->condition);
    (void)pthread_mutex_unlock(&bridge->mutex);
}

int maelys_bridge_is_cancelled(const maelys_bridge_t *bridge) {
    return bridge && atomic_load_explicit(&bridge->cancelled, memory_order_acquire);
}

int maelys_bridge_has_failed(const maelys_bridge_t *bridge) {
    return bridge && atomic_load_explicit(&bridge->failed, memory_order_acquire);
}

maelys_bridge_result_t maelys_bridge_wait_frame(maelys_bridge_t *bridge, json_t **out_frame,
                                                char **out_error) {
    if (!bridge || !out_frame) {
        set_error(out_error, "invalid bridge wait arguments");
        return MAELYS_BRIDGE_ERROR;
    }
    *out_frame = NULL;
    (void)pthread_mutex_lock(&bridge->mutex);
    while (!bridge->head && !bridge->closed && !maelys_bridge_is_cancelled(bridge)) {
        (void)pthread_cond_wait(&bridge->condition, &bridge->mutex);
    }
    if (maelys_bridge_is_cancelled(bridge)) {
        (void)pthread_mutex_unlock(&bridge->mutex);
        return MAELYS_BRIDGE_CANCELLED;
    }
    if (!bridge->head) {
        if (bridge->reader_error)
            set_error(out_error, bridge->reader_error);
        (void)pthread_mutex_unlock(&bridge->mutex);
        return MAELYS_BRIDGE_CLOSED;
    }
    frame_node_t *node = bridge->head;
    bridge->head = node->next;
    bridge->queued_frames--;
    if (!bridge->head)
        bridge->tail = NULL;
    (void)pthread_mutex_unlock(&bridge->mutex);
    *out_frame = node->frame;
    free(node);
    return MAELYS_BRIDGE_OK;
}

int maelys_bridge_send(maelys_bridge_t *bridge, const json_t *frame, char **out_error) {
    if (!bridge || !frame) {
        set_error(out_error, "invalid bridge send arguments");
        return -1;
    }
    char *encoded = json_dumps(frame, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    if (!encoded) {
        set_error(out_error, "cannot encode bridge frame");
        return -1;
    }
    size_t size = strlen(encoded);
    if (size > bridge->max_frame_bytes) {
        free(encoded);
        set_error(out_error, "outgoing bridge frame exceeds max_frame_bytes");
        return -1;
    }
    size_t offset = 0u;
    while (offset < size) {
        ssize_t count = write(bridge->fd, encoded + offset, size - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            free(encoded);
            set_errno_error(out_error, "write bridge");
            return -1;
        }
        offset += (size_t)count;
    }
    free(encoded);
    for (;;) {
        ssize_t count = write(bridge->fd, "\n", 1u);
        if (count == 1)
            return 0;
        if (count < 0 && errno == EINTR)
            continue;
        set_errno_error(out_error, "write bridge newline");
        return -1;
    }
}

void maelys_bridge_destroy(maelys_bridge_t *bridge) {
    if (!bridge)
        return;
    if (bridge->reader_started) {
        (void)shutdown(bridge->fd, SHUT_RD);
        (void)pthread_join(bridge->reader, NULL);
    }
    frame_node_t *node = bridge->head;
    while (node) {
        frame_node_t *next = node->next;
        json_decref(node->frame);
        free(node);
        node = next;
    }
    free(bridge->reader_error);
    (void)pthread_cond_destroy(&bridge->condition);
    (void)pthread_mutex_destroy(&bridge->mutex);
    free(bridge);
}
