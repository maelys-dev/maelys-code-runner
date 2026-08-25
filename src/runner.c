#include "runner.h"

#include "bridge.h"

#include <quickjs.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct runner_context {
    maelys_bridge_t *bridge;
    char **tool_names;
    size_t tool_count;
    uint64_t next_call_id;
    uint64_t call_count;
    uint64_t max_calls;
    size_t max_result_bytes;
    uint64_t timeout_ms;
    uint64_t started_ms;
    uint64_t deadline_ms;
    int tool_limit_exceeded;
    int protocol_failed;
} runner_context_t;

static volatile sig_atomic_t process_cancelled = 0;

static void signal_handler(int signal_number) {
    (void)signal_number;
    process_cancelled = 1;
}

static void set_error(char **out_error, const char *message) {
    if (!out_error || *out_error)
        return;
    *out_error = strdup(message ? message : "runner error");
}

static uint64_t monotonic_ms(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0u;
    return ((uint64_t)now.tv_sec * UINT64_C(1000)) + ((uint64_t)now.tv_nsec / UINT64_C(1000000));
}

static int string_is_safe(const json_t *value) {
    if (!json_is_string(value))
        return 0;
    const char *text = json_string_value(value);
    return text && strlen(text) == json_string_length(value);
}

static int request_has_version_and_type(const json_t *request) {
    const json_t *version = json_object_get(request, "version");
    const json_t *type = json_object_get(request, "type");
    return string_is_safe(version) && string_is_safe(type) &&
           strcmp(json_string_value(version), "code-bridge-v1") == 0 &&
           strcmp(json_string_value(type), "execute") == 0;
}

static int read_limit(const json_t *limits, const char *name, int allow_zero, uint64_t *out_value,
                      char **out_error) {
    const json_t *value = json_is_object(limits) ? json_object_get(limits, name) : NULL;
    if (!json_is_integer(value) || json_integer_value(value) < (allow_zero ? 0 : 1)) {
        char message[160];
        (void)snprintf(message, sizeof(message), "execute.limits.%s must be a %s integer", name,
                       allow_zero ? "non-negative" : "positive");
        set_error(out_error, message);
        return -1;
    }
    *out_value = (uint64_t)json_integer_value(value);
    return 0;
}

static int load_effective_limits(runner_context_t *runner, const json_t *request,
                                 const maelys_runner_limits_t *hard_limits, char **out_error) {
    const json_t *requested = json_object_get(request, "limits");
    uint64_t max_calls = 0u;
    uint64_t max_result_bytes = 0u;
    uint64_t timeout_ms = 0u;
    if (!json_is_object(requested) ||
        read_limit(requested, "maxToolCalls", 1, &max_calls, out_error) != 0 ||
        read_limit(requested, "maxResultBytes", 0, &max_result_bytes, out_error) != 0 ||
        read_limit(requested, "timeoutMs", 0, &timeout_ms, out_error) != 0) {
        if (out_error && !*out_error)
            set_error(out_error, "execute.limits must be an object");
        return -1;
    }
    if (max_result_bytes > SIZE_MAX) {
        set_error(out_error, "execute.limits.maxResultBytes exceeds the platform size limit");
        return -1;
    }
    if (max_calls >= (uint64_t)INT64_MAX) {
        set_error(out_error, "execute.limits.maxToolCalls is too large");
        return -1;
    }
    runner->max_calls = max_calls < hard_limits->max_calls ? max_calls : hard_limits->max_calls;
    runner->max_result_bytes = (size_t)max_result_bytes < hard_limits->max_result_bytes
                                   ? (size_t)max_result_bytes
                                   : hard_limits->max_result_bytes;
    runner->timeout_ms =
        timeout_ms < hard_limits->timeout_ms ? timeout_ms : hard_limits->timeout_ms;
    return 0;
}

static void free_catalog(runner_context_t *runner) {
    if (!runner)
        return;
    for (size_t i = 0u; i < runner->tool_count; ++i)
        free(runner->tool_names[i]);
    free(runner->tool_names);
    runner->tool_names = NULL;
    runner->tool_count = 0u;
}

static int catalog_contains(const runner_context_t *runner, const char *name) {
    if (!runner || !name)
        return 0;
    for (size_t i = 0u; i < runner->tool_count; ++i) {
        if (strcmp(runner->tool_names[i], name) == 0)
            return 1;
    }
    return 0;
}

static int load_catalog(runner_context_t *runner, const json_t *request, char **out_error) {
    const json_t *tools = json_object_get(request, "tools");
    if (!json_is_array(tools) || json_array_size(tools) > 512u) {
        set_error(out_error, "execute.tools must be an array with at most 512 entries");
        return -1;
    }
    size_t count = json_array_size(tools);
    char **names = calloc(count == 0u ? 1u : count, sizeof(*names));
    if (!names) {
        set_error(out_error, "out of memory allocating tool catalog");
        return -1;
    }
    runner->tool_names = names;
    for (size_t i = 0u; i < count; ++i) {
        const json_t *descriptor = json_array_get(tools, i);
        const json_t *name_value =
            json_is_object(descriptor) ? json_object_get(descriptor, "name") : NULL;
        if (!string_is_safe(name_value)) {
            set_error(out_error, "every tools entry must have a NUL-free string name");
            return -1;
        }
        const char *name = json_string_value(name_value);
        size_t name_size = json_string_length(name_value);
        if (name_size == 0u || name_size > 256u) {
            set_error(out_error, "tool names must contain between 1 and 256 bytes");
            return -1;
        }
        if (catalog_contains(runner, name)) {
            set_error(out_error, "tool catalog contains a duplicate name");
            return -1;
        }
        runner->tool_names[i] = strdup(name);
        if (!runner->tool_names[i]) {
            set_error(out_error, "out of memory copying tool name");
            return -1;
        }
        runner->tool_count++;
    }
    return 0;
}

static int interrupt_handler(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    runner_context_t *runner = opaque;
    if (process_cancelled || maelys_bridge_is_cancelled(runner->bridge) ||
        maelys_bridge_has_failed(runner->bridge))
        return 1;
    return runner->deadline_ms != 0u && monotonic_ms() >= runner->deadline_ms;
}

static JSValue throw_bridge_error(JSContext *context, const char *message) {
    return JS_ThrowInternalError(context, "Code Mode bridge: %s", message ? message : "error");
}

static char *stringify_js_value(JSContext *context, JSValueConst value, size_t *out_size) {
    JSValue encoded = JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(encoded) || JS_IsUndefined(encoded)) {
        JS_FreeValue(context, encoded);
        return NULL;
    }
    const char *text = JS_ToCStringLen(context, out_size, encoded);
    if (!text) {
        JS_FreeValue(context, encoded);
        return NULL;
    }
    char *copy = malloc(*out_size + 1u);
    if (copy) {
        memcpy(copy, text, *out_size);
        copy[*out_size] = '\0';
    }
    JS_FreeCString(context, text);
    JS_FreeValue(context, encoded);
    return copy;
}

static JSValue json_to_js(JSContext *context, const json_t *value) {
    char *encoded =
        json_dumps(value, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS | JSON_ENCODE_ANY);
    if (!encoded)
        return JS_ThrowInternalError(context, "cannot encode tool result");
    size_t size = strlen(encoded);
    JSValue result = JS_ParseJSON(context, encoded, size, "<tool-result>");
    free(encoded);
    return result;
}

static int response_matches(const json_t *frame, uint64_t call_id) {
    const json_t *version = json_object_get(frame, "version");
    const json_t *type = json_object_get(frame, "type");
    const json_t *id = json_object_get(frame, "id");
    return string_is_safe(version) && string_is_safe(type) && json_is_integer(id) &&
           strcmp(json_string_value(version), "code-bridge-v1") == 0 &&
           strcmp(json_string_value(type), "tool.result") == 0 && json_integer_value(id) >= 0 &&
           (uint64_t)json_integer_value(id) == call_id;
}

static JSValue invoke_tool(JSContext *context, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
    (void)this_value;
    runner_context_t *runner = JS_GetContextOpaque(context);
    if (!runner || argc != 2) {
        return JS_ThrowTypeError(context, "invoke requires a tool name and arguments object");
    }
    size_t name_size = 0u;
    const char *name = JS_ToCStringLen(context, &name_size, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    if (name_size == 0u || name_size > 256u || !catalog_contains(runner, name)) {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "tool is not declared in this execution");
    }
    if (!JS_IsObject(argv[1]) || JS_IsArray(argv[1])) {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "tool arguments must be an object");
    }
    if (runner->call_count >= runner->max_calls) {
        runner->tool_limit_exceeded = 1;
        JS_FreeCString(context, name);
        return JS_ThrowInternalError(context, "tool call limit exceeded");
    }
    size_t arguments_size = 0u;
    char *arguments_text = stringify_js_value(context, argv[1], &arguments_size);
    if (!arguments_text) {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "tool arguments must be JSON serializable");
    }
    json_error_t parse_error;
    json_t *arguments =
        json_loadb(arguments_text, arguments_size, JSON_REJECT_DUPLICATES, &parse_error);
    free(arguments_text);
    if (!json_is_object(arguments)) {
        json_decref(arguments);
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "tool arguments must encode as a JSON object");
    }
    uint64_t call_id = ++runner->next_call_id;
    runner->call_count++;
    json_t *call =
        json_pack("{s:s,s:s,s:I,s:s,s:o}", "version", "code-bridge-v1", "type", "tool.call", "id",
                  (json_int_t)call_id, "name", name, "arguments", arguments);
    JS_FreeCString(context, name);
    char *bridge_error = NULL;
    if (!call || maelys_bridge_send(runner->bridge, call, &bridge_error) != 0) {
        runner->protocol_failed = 1;
        json_decref(call);
        JSValue exception = throw_bridge_error(context, bridge_error);
        free(bridge_error);
        return exception;
    }
    json_decref(call);
    json_t *response = NULL;
    maelys_bridge_result_t wait_result =
        maelys_bridge_wait_frame(runner->bridge, &response, &bridge_error);
    if (wait_result != MAELYS_BRIDGE_OK) {
        if (wait_result != MAELYS_BRIDGE_CANCELLED)
            runner->protocol_failed = 1;
        JSValue exception = wait_result == MAELYS_BRIDGE_CANCELLED
                                ? JS_ThrowInternalError(context, "execution cancelled")
                                : throw_bridge_error(context, bridge_error);
        free(bridge_error);
        return exception;
    }
    if (!response_matches(response, call_id)) {
        runner->protocol_failed = 1;
        json_decref(response);
        return JS_ThrowInternalError(context, "unexpected or mismatched tool response");
    }
    const json_t *ok = json_object_get(response, "ok");
    if (!json_is_boolean(ok)) {
        runner->protocol_failed = 1;
        json_decref(response);
        return JS_ThrowInternalError(context, "tool response has no boolean ok field");
    }
    if (!json_is_true(ok)) {
        const json_t *error = json_object_get(response, "error");
        const json_t *message = json_is_object(error) ? json_object_get(error, "message") : NULL;
        JSValue exception = JS_ThrowInternalError(
            context, "tool failed: %s",
            string_is_safe(message) ? json_string_value(message) : "unspecified provider error");
        json_decref(response);
        return exception;
    }
    const json_t *result = json_object_get(response, "result");
    JSValue js_result = json_to_js(context, result ? result : json_null());
    if (JS_IsException(js_result))
        runner->protocol_failed = 1;
    json_decref(response);
    return js_result;
}

static char *exception_text(JSContext *context) {
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    char *copy = message ? strdup(message) : strdup("JavaScript exception");
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
        const char *stack_text = JS_ToCString(context, stack);
        if (stack_text && (!message || strcmp(stack_text, message) != 0)) {
            size_t first = copy ? strlen(copy) : 0u;
            size_t second = strlen(stack_text);
            char *combined = malloc(first + second + 2u);
            if (combined) {
                if (copy)
                    memcpy(combined, copy, first);
                combined[first] = '\n';
                memcpy(combined + first + 1u, stack_text, second + 1u);
                free(copy);
                copy = combined;
            }
        }
        if (stack_text)
            JS_FreeCString(context, stack_text);
    }
    JS_FreeValue(context, stack);
    if (message)
        JS_FreeCString(context, message);
    JS_FreeValue(context, exception);
    return copy;
}

static int install_api(JSContext *context, const runner_context_t *runner, char **out_error) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue invoke = JS_NewCFunction(context, invoke_tool, "__maelysInvoke", 2);
    if (JS_IsException(invoke) || JS_DefinePropertyValueStr(context, global, "__maelysInvoke",
                                                            invoke, JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, global);
        set_error(out_error, "cannot install native tool dispatcher");
        return -1;
    }
    JS_FreeValue(context, global);

    json_t *names = json_array();
    if (!names) {
        set_error(out_error, "out of memory encoding tool catalog");
        return -1;
    }
    for (size_t i = 0u; i < runner->tool_count; ++i) {
        if (json_array_append_new(names, json_string(runner->tool_names[i])) != 0) {
            json_decref(names);
            set_error(out_error, "cannot encode tool catalog");
            return -1;
        }
    }
    char *catalog = json_dumps(names, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(names);
    if (!catalog) {
        set_error(out_error, "cannot serialize tool catalog");
        return -1;
    }
    const char prefix[] = "(() => { 'use strict'; const names = ";
    const char suffix[] = "; const dispatch = __maelysInvoke; delete globalThis.__maelysInvoke;"
                          " const registry = Object.create(null);"
                          " for (const name of names) Object.defineProperty(registry, name, {"
                          " value: (argumentsObject = {}) => dispatch(name, argumentsObject),"
                          " enumerable: true, writable: false, configurable: false });"
                          " Object.freeze(registry);"
                          " Object.defineProperty(globalThis, 'tools', { value: registry,"
                          " writable: false, configurable: false });"
                          "})()";
    size_t script_size = strlen(prefix) + strlen(catalog) + strlen(suffix);
    char *script = malloc(script_size + 1u);
    if (!script) {
        free(catalog);
        set_error(out_error, "out of memory building tool API");
        return -1;
    }
    (void)snprintf(script, script_size + 1u, "%s%s%s", prefix, catalog, suffix);
    free(catalog);
    JSValue installed =
        JS_Eval(context, script, script_size, "<maelys-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    free(script);
    if (JS_IsException(installed)) {
        char *detail = exception_text(context);
        set_error(out_error, detail);
        free(detail);
        JS_FreeValue(context, installed);
        return -1;
    }
    JS_FreeValue(context, installed);
    return 0;
}

static int send_ready(runner_context_t *runner, const json_t *execution_id, char **out_error) {
    json_t *effective = json_pack("{s:I,s:I,s:I}", "maxToolCalls", (json_int_t)runner->max_calls,
                                  "maxResultBytes", (json_int_t)runner->max_result_bytes,
                                  "timeoutMs", (json_int_t)runner->timeout_ms);
    json_t *execution_id_copy = execution_id ? json_deep_copy(execution_id) : json_null();
    json_t *frame = json_pack("{s:s,s:s,s:s}", "version", "code-bridge-v1", "type", "ready",
                              "runnerVersion", MAELYS_CODE_RUNNER_VERSION);
    if (!effective || !execution_id_copy || !frame) {
        json_decref(effective);
        json_decref(execution_id_copy);
        json_decref(frame);
        set_error(out_error, "cannot allocate ready frame");
        return -1;
    }
    if (json_object_set(frame, "executionId", execution_id_copy) != 0 ||
        json_object_set(frame, "effectiveLimits", effective) != 0) {
        json_decref(effective);
        json_decref(execution_id_copy);
        json_decref(frame);
        set_error(out_error, "cannot populate ready frame");
        return -1;
    }
    json_decref(effective);
    json_decref(execution_id_copy);
    int result = maelys_bridge_send(runner->bridge, frame, out_error);
    json_decref(frame);
    return result;
}

static int send_terminal(runner_context_t *runner, const json_t *execution_id, int ok,
                         json_t *payload, char **out_error) {
    uint64_t elapsed = monotonic_ms() - runner->started_ms;
    json_t *metrics = json_pack("{s:I,s:I}", "elapsedMs", (json_int_t)elapsed, "toolCalls",
                                (json_int_t)runner->call_count);
    json_t *execution_id_copy = execution_id ? json_deep_copy(execution_id) : json_null();
    json_t *frame = json_pack("{s:s,s:s,s:b}", "version", "code-bridge-v1", "type",
                              "execution.result", "ok", ok);
    if (!metrics || !execution_id_copy || !frame) {
        json_decref(metrics);
        json_decref(execution_id_copy);
        json_decref(frame);
        json_decref(payload);
        set_error(out_error, "cannot allocate completion frame");
        return -1;
    }
    if (json_object_set_new(frame, "executionId", execution_id_copy) != 0 ||
        json_object_set_new(frame, "metrics", metrics) != 0) {
        json_decref(frame);
        json_decref(payload);
        set_error(out_error, "cannot populate completion frame");
        return -1;
    }
    if (ok) {
        if (json_object_set_new(frame, "result", payload ? payload : json_null()) != 0) {
            json_decref(frame);
            set_error(out_error, "cannot attach execution result");
            return -1;
        }
    } else {
        if (!json_is_object(payload) || json_object_set_new(frame, "error", payload) != 0) {
            json_decref(frame);
            json_decref(payload);
            set_error(out_error, "cannot attach execution error");
            return -1;
        }
    }
    int result = maelys_bridge_send(runner->bridge, frame, out_error);
    json_decref(frame);
    return result;
}

static int send_failure_text(runner_context_t *runner, const json_t *execution_id, const char *kind,
                             const char *message, char **out_error) {
    json_t *error = json_pack("{s:s,s:s}", "kind", kind ? kind : "internal", "message",
                              message ? message : "execution failed");
    if (!error) {
        set_error(out_error, "cannot encode execution error");
        return -1;
    }
    return send_terminal(runner, execution_id, 0, error, out_error);
}

static int send_tool_limit_failure(runner_context_t *runner, const json_t *execution_id,
                                   char **out_error) {
    json_t *error =
        json_pack("{s:s,s:s,s:s,s:I,s:I}", "kind", "limit", "code", "TOOL_CALL_LIMIT_EXCEEDED",
                  "message", "tool call limit exceeded", "limit", (json_int_t)runner->max_calls,
                  "attempted", (json_int_t)(runner->max_calls + 1u));
    if (!error) {
        set_error(out_error, "cannot encode tool-call limit error");
        return -1;
    }
    return send_terminal(runner, execution_id, 0, error, out_error);
}

int maelys_runner_execute(int bridge_fd, json_t *request, const maelys_runner_limits_t *limits,
                          char **out_error) {
    if (bridge_fd < 0 || !json_is_object(request) || !limits ||
        !request_has_version_and_type(request)) {
        set_error(out_error, "expected a code-bridge-v1 execute frame");
        return -1;
    }
    const json_t *code_value = json_object_get(request, "code");
    const json_t *execution_id = json_object_get(request, "executionId");
    if (!string_is_safe(code_value) || json_string_length(code_value) == 0u) {
        set_error(out_error, "execute.code must be a non-empty NUL-free string");
        return -1;
    }
    if (execution_id && !json_is_null(execution_id) && !string_is_safe(execution_id)) {
        set_error(out_error, "execute.executionId must be a NUL-free string or null");
        return -1;
    }
    runner_context_t runner = {.started_ms = monotonic_ms()};
    if (load_effective_limits(&runner, request, limits, out_error) != 0)
        return -1;
    if (load_catalog(&runner, request, out_error) != 0) {
        free_catalog(&runner);
        return -1;
    }
    if (maelys_bridge_create(bridge_fd, limits->max_frame_bytes, &runner.bridge, out_error) != 0 ||
        maelys_bridge_start(runner.bridge, out_error) != 0) {
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) {
        (void)send_failure_text(&runner, execution_id, "memory", "cannot create QuickJS runtime",
                                NULL);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        set_error(out_error, "cannot create QuickJS runtime");
        return -1;
    }
    JS_SetMemoryLimit(runtime, limits->memory_bytes);
    JS_SetMaxStackSize(runtime, limits->stack_bytes);
    JS_SetCanBlock(runtime, false);
    JS_SetInterruptHandler(runtime, interrupt_handler, &runner);
    JSContext *context = JS_NewContext(runtime);
    if (!context) {
        (void)send_failure_text(&runner, execution_id, "memory", "cannot create QuickJS context",
                                NULL);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        set_error(out_error, "cannot create QuickJS context");
        return -1;
    }
    JS_SetContextOpaque(context, &runner);
    if (install_api(context, &runner, out_error) != 0) {
        (void)send_failure_text(&runner, execution_id, "internal", *out_error, NULL);
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }
    uint64_t generated_code_started_ms = monotonic_ms();
    if (runner.timeout_ms > UINT64_MAX - generated_code_started_ms) {
        set_error(out_error, "timeout overflows the monotonic clock");
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }
    runner.deadline_ms = generated_code_started_ms + runner.timeout_ms;
    if (send_ready(&runner, execution_id, out_error) != 0) {
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }

    const char *code = json_string_value(code_value);
    size_t code_size = json_string_length(code_value);
    const char prefix[] = "(async function () { 'use strict';\n";
    const char suffix[] = "\n})()";
    if (code_size > SIZE_MAX - sizeof(prefix) - sizeof(suffix)) {
        set_error(out_error, "code size overflows runner allocation");
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }
    size_t wrapped_size = strlen(prefix) + code_size + strlen(suffix);
    char *wrapped = malloc(wrapped_size + 1u);
    if (!wrapped) {
        set_error(out_error, "out of memory wrapping user code");
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
        maelys_bridge_destroy(runner.bridge);
        free_catalog(&runner);
        return -1;
    }
    memcpy(wrapped, prefix, strlen(prefix));
    memcpy(wrapped + strlen(prefix), code, code_size);
    memcpy(wrapped + strlen(prefix) + code_size, suffix, strlen(suffix) + 1u);
    JSValue evaluation = JS_Eval(context, wrapped, wrapped_size, "<generated-code>",
                                 JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    free(wrapped);

    int failed = JS_IsException(evaluation);
    if (!failed && JS_IsPromise(evaluation)) {
        while (JS_PromiseState(context, evaluation) == JS_PROMISE_PENDING) {
            JSContext *job_context = NULL;
            int jobs = JS_ExecutePendingJob(runtime, &job_context);
            if (jobs < 0) {
                failed = 1;
                break;
            }
            if (jobs == 0) {
                JS_FreeValue(context, evaluation);
                evaluation = JS_ThrowInternalError(
                    context, "promise remained pending without a runnable job");
                failed = 1;
                break;
            }
        }
        if (!failed) {
            JSPromiseStateEnum state = JS_PromiseState(context, evaluation);
            JSValue settled = JS_PromiseResult(context, evaluation);
            JS_FreeValue(context, evaluation);
            evaluation = settled;
            failed = state == JS_PROMISE_REJECTED;
            if (failed)
                (void)JS_Throw(context, JS_DupValue(context, evaluation));
        }
    }

    int result = 0;
    const int cancelled = process_cancelled || maelys_bridge_is_cancelled(runner.bridge);
    const int timed_out = runner.deadline_ms != 0u && monotonic_ms() >= runner.deadline_ms;
    if (runner.tool_limit_exceeded) {
        if (failed) {
            char *ignored = exception_text(context);
            free(ignored);
        }
        result = send_tool_limit_failure(&runner, execution_id, out_error);
    } else if (cancelled || timed_out) {
        if (failed) {
            char *ignored = exception_text(context);
            free(ignored);
        }
        result =
            send_failure_text(&runner, execution_id, cancelled ? "cancelled" : "timeout",
                              cancelled ? "execution cancelled" : "execution timed out", out_error);
    } else if (runner.protocol_failed || maelys_bridge_has_failed(runner.bridge)) {
        if (failed) {
            char *ignored = exception_text(context);
            free(ignored);
        }
        result = send_failure_text(&runner, execution_id, "internal",
                                   "Code Mode bridge protocol failure", out_error);
    } else if (failed) {
        char *message = exception_text(context);
        const char *kind =
            message && strstr(message, "out of memory") != NULL ? "memory" : "javascript";
        result = send_failure_text(&runner, execution_id, kind, message, out_error);
        free(message);
    } else {
        size_t result_size = 0u;
        char *result_text = stringify_js_value(context, evaluation, &result_size);
        if (!result_text) {
            result = send_failure_text(&runner, execution_id, "result",
                                       "result is not JSON serializable", out_error);
        } else if (result_size > runner.max_result_bytes) {
            result = send_failure_text(&runner, execution_id, "result_limit",
                                       "result exceeds max_result_bytes", out_error);
        } else {
            json_error_t parse_error;
            json_t *json_result = json_loadb(
                result_text, result_size, JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &parse_error);
            if (!json_result) {
                result = send_failure_text(&runner, execution_id, "result",
                                           "result is not valid JSON", out_error);
            } else {
                result = send_terminal(&runner, execution_id, 1, json_result, out_error);
            }
        }
        free(result_text);
    }
    JS_FreeValue(context, evaluation);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    maelys_bridge_destroy(runner.bridge);
    free_catalog(&runner);
    return result;
}

int maelys_runner_install_signal_handlers(char **out_error) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        if (out_error && !*out_error) {
            const char *detail = strerror(errno);
            size_t size = strlen(detail) + 40u;
            *out_error = malloc(size);
            if (*out_error)
                (void)snprintf(*out_error, size, "cannot install signal handlers: %s", detail);
        }
        return -1;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    return 0;
}
