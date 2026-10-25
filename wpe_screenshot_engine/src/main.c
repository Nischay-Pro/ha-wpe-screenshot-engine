#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#define IMAGE_WIDTH 1280
#define IMAGE_HEIGHT 800
#define MAX_IMAGE_BYTES (20u * 1024u * 1024u)
#define MAX_HA_TOKEN_BYTES 8192u
#define MAX_HA_URI_BYTES 8192u
#define MAX_CONFIG_PATH_BYTES 4096u
#define MAX_EXPECTED_TEXT_BYTES 256u
#define MAX_FIXTURE_HTML_BYTES (1024u * 1024u)
#define MAX_DASHBOARD_PATH_BYTES 256u
#define MAX_QUERY_BYTES 4096u
#define MAX_QUERY_PARAMETERS 16u
#define DEFAULT_PORT 8080
#define DEFAULT_RENDER_DEADLINE_MS 55000
#define DEFAULT_WAIT_MS 2500
#define DEFAULT_DASHBOARD_REUSE_IDLE_SECONDS 300
#define MAX_DASHBOARD_REUSE_IDLE_SECONDS 3600
#define DEFAULT_WEBKIT_PROCESS_MEMORY_LIMIT_MB 512
#define MIN_WEBKIT_PROCESS_MEMORY_LIMIT_MB 512
#define MAX_WEBKIT_PROCESS_MEMORY_LIMIT_MB 3072
#define MAX_WAIT_MS 30000

typedef struct _App App;
typedef struct _RenderRequest RenderRequest;

struct _RenderRequest {
    gint refs;
    App *app;
    SoupServerMessage *message;
    GCancellable *cancellable;
    guint64 id;
    guint64 view_generation;
    guint timeout_source;
    guint readiness_source;
    guint wait_source;
    guint disconnect_source;
    gulong message_disconnected_handler;
    gboolean readiness_pending;
    gboolean finished;
    guint wait_ms;
    gint64 started_at_us;
    gchar *path;
};

struct _App {
    GMainLoop *loop;
    SoupServer *server;
    WebKitWebContext *web_context;
    WebKitNetworkSession *network_session;
    WPEDisplay *display;
    WebKitWebView *view;
    WPEView *wpe_view;
    RenderRequest *active;
    guint64 next_request_id;
    guint64 view_generation;
    guint port;
    guint render_deadline_ms;
    guint dashboard_reuse_idle_seconds;
    guint webkit_process_memory_limit_mb;
    gboolean renderer_ready;
    gboolean dashboard_loaded;
    gint64 dashboard_last_capture_at_us;
    gboolean fixture_mode;
    guint fixture_load_count;
    guint fixture_delay_ms;
    guint fixture_first_delay_ms;
    gchar *ha_url;
    gchar *ha_origin;
    gchar *token_file;
    gchar *token;
    gchar *readiness_path;
    gchar *expected_text;
    gchar *fixture_html_path;
};

typedef struct {
    guint wait_ms;
} CaptureOptions;

static RenderRequest *request_ref(RenderRequest *request);
static void request_unref(RenderRequest *request);
static void disconnect_message_disconnected_handler(RenderRequest *request);
static void cancel_disconnected_request(RenderRequest *request);
static void app_recreate_view(App *app);
static gboolean current_request(RenderRequest *request);
static gboolean is_dashboard_uri(App *app, const gchar *candidate,
                                 const gchar *dashboard_path);
static gboolean is_same_origin_uri(App *app, const gchar *uri_text);
static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data);
static void on_uri_changed(WebKitWebView *view, GParamSpec *property, gpointer user_data);
static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
                               const gchar *uri, GError *error, gpointer user_data);
static gboolean on_permission_request(WebKitWebView *view,
                                      WebKitPermissionRequest *permission,
                                      gpointer user_data);
static void on_web_process_terminated(WebKitWebView *view,
                                     WebKitWebProcessTerminationReason reason,
                                     gpointer user_data);
static void on_snapshot_ready(GObject *source, GAsyncResult *result, gpointer user_data);
static void on_message_disconnected(SoupServerMessage *message, gpointer user_data);
static gboolean check_client_connection(gpointer user_data);

static gboolean
env_truthy(const gchar *name)
{
    const gchar *value = g_getenv(name);
    return value && (!g_ascii_strcasecmp(value, "1") ||
                     !g_ascii_strcasecmp(value, "true") ||
                     !g_ascii_strcasecmp(value, "yes"));
}

static void
secure_free_string(gchar *value)
{
    if (!value)
        return;
    explicit_bzero(value, strlen(value));
    g_free(value);
}

static gboolean
parse_bounded_uint(const gchar *name, const gchar *value, guint minimum,
                   guint maximum, guint *result)
{
    (void) name;
    if (!value || !*value || strnlen(value, MAX_QUERY_BYTES + 1u) > MAX_QUERY_BYTES)
        return FALSE;

    for (const gchar *p = value; *p; ++p) {
        if (!g_ascii_isdigit(*p))
            return FALSE;
    }

    errno = 0;
    gchar *end = NULL;
    guint64 parsed = g_ascii_strtoull(value, &end, 10);
    if (errno || !end || *end || parsed < minimum || parsed > maximum) {
        return FALSE;
    }
    *result = (guint) parsed;
    return TRUE;
}

static gboolean
valid_dashboard_path(const gchar *path)
{
    if (!path || !*path)
        return FALSE;
    gsize path_length = strnlen(path, MAX_DASHBOARD_PATH_BYTES + 1u);
    if (path_length > MAX_DASHBOARD_PATH_BYTES || path[0] == '/' ||
        path[path_length - 1] == '/')
        return FALSE;

    const gchar *segment = path;
    for (const gchar *p = path;; ++p) {
        if (*p == '/' || *p == '\0') {
            gsize length = (gsize) (p - segment);
            if (!length || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return FALSE;
            for (const gchar *c = segment; c < p; ++c) {
                if (!(g_ascii_isalnum(*c) || *c == '-' || *c == '_' || *c == '.'))
                    return FALSE;
            }
            if (!*p)
                break;
            segment = p + 1;
        }
    }
    return TRUE;
}

static gchar *
origin_from_uri(const gchar *text, gboolean require_root_path, GError **error)
{
    if (!text || strnlen(text, MAX_HA_URI_BYTES + 1u) > MAX_HA_URI_BYTES) {
        g_set_error_literal(error, G_URI_ERROR, G_URI_ERROR_FAILED,
                            "URL is missing or exceeds the supported length");
        return NULL;
    }

    GUri *uri = g_uri_parse(text, G_URI_FLAGS_NONE, error);
    if (!uri)
        return NULL;

    const gchar *scheme = g_uri_get_scheme(uri);
    const gchar *host = g_uri_get_host(uri);
    const gchar *path = g_uri_get_path(uri);
    gboolean secure = scheme && !g_ascii_strcasecmp(scheme, "https");
    gboolean plain = scheme && !g_ascii_strcasecmp(scheme, "http");
    gint port = g_uri_get_port(uri);

    if ((!secure && !plain) || !host || !*host || g_uri_get_userinfo(uri) ||
        (require_root_path && (g_uri_get_query(uri) || g_uri_get_fragment(uri))) ||
        (require_root_path && path && *path && strcmp(path, "/")) ||
        port == 0 || port > 65535 || port < -1) {
        g_set_error_literal(error, G_URI_ERROR, G_URI_ERROR_FAILED,
                            "URL must be an HTTP(S) origin without credentials, query, fragment, or base path");
        g_uri_unref(uri);
        return NULL;
    }

    gint default_port = secure ? 443 : 80;
    gint normalized_port = (port == -1 || port == default_port) ? -1 : port;
    gchar *lower_host = g_ascii_strdown(host, -1);
    gchar *origin = g_uri_join(G_URI_FLAGS_NONE, secure ? "https" : "http", NULL,
                               lower_host, normalized_port, "", NULL, NULL);
    g_free(lower_host);
    g_uri_unref(uri);
    return origin;
}

static gchar *
normalize_ha_url(const gchar *url, gchar **origin_out, GError **error)
{
    gchar *origin = origin_from_uri(url, TRUE, error);
    if (!origin)
        return NULL;
    *origin_out = g_strdup(origin);
    return origin;
}

static gchar *
read_token_file_bounded(const gchar *path)
{
    if (!path || strnlen(path, MAX_CONFIG_PATH_BYTES + 1u) > MAX_CONFIG_PATH_BYTES)
        return NULL;

    gint fd = g_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
    if (fd < 0)
        return NULL;

    struct stat metadata;
    if (fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 || (guint64) metadata.st_size > MAX_HA_TOKEN_BYTES) {
        close(fd);
        return NULL;
    }

    gchar *contents = g_malloc(MAX_HA_TOKEN_BYTES + 1u);
    gsize length = 0;
    gboolean valid = TRUE;
    while (length <= MAX_HA_TOKEN_BYTES) {
        ssize_t count = read(fd, contents + length,
                             MAX_HA_TOKEN_BYTES + 1u - length);
        if (count > 0) {
            length += (gsize) count;
            if (length > MAX_HA_TOKEN_BYTES) {
                valid = FALSE;
                break;
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            valid = FALSE;
            break;
        }
    }
    close(fd);

    if (!valid || !length || memchr(contents, '\0', length) ||
        !g_utf8_validate(contents, (gssize) length, NULL)) {
        explicit_bzero(contents, MAX_HA_TOKEN_BYTES + 1u);
        g_free(contents);
        return NULL;
    }

    contents[length] = '\0';
    g_strstrip(contents);
    if (!*contents) {
        explicit_bzero(contents, MAX_HA_TOKEN_BYTES + 1u);
        g_free(contents);
        return NULL;
    }

    gchar *token = g_strdup(contents);
    explicit_bzero(contents, MAX_HA_TOKEN_BYTES + 1u);
    g_free(contents);
    return token;
}

static gchar *
js_quote(const gchar *input)
{
    GString *output = g_string_new("\"");
    for (const guchar *p = (const guchar *) input; *p; ++p) {
        switch (*p) {
        case '\\': g_string_append(output, "\\\\"); break;
        case '"': g_string_append(output, "\\\""); break;
        case '\n': g_string_append(output, "\\n"); break;
        case '\r': g_string_append(output, "\\r"); break;
        case '\t': g_string_append(output, "\\t"); break;
        case '<': g_string_append(output, "\\u003c"); break;
        case '>': g_string_append(output, "\\u003e"); break;
        case '&': g_string_append(output, "\\u0026"); break;
        default:
            if (*p < 0x20)
                g_string_append_printf(output, "\\u%04x", *p);
            else
                g_string_append_c(output, (gchar) *p);
        }
    }
    g_string_append_c(output, '"');
    return g_string_free(output, FALSE);
}

static gboolean
load_runtime_config(App *app)
{
    GError *error = NULL;
    const gchar *ha_url = g_getenv("HA_URL");
    const gchar *readiness_path = g_getenv("DASHBOARD_PATH");
    const gchar *expected_text = g_getenv("EXPECTED_TEXT");
    const gchar *token_file = g_getenv("HA_TOKEN_FILE");
    const gchar *fixture_path = g_getenv("FIXTURE_HTML_PATH");
    const gchar *port_text = g_getenv("PORT");
    const gchar *deadline_text = g_getenv("RENDER_DEADLINE_SECONDS");
    const gchar *reuse_idle_text = g_getenv("DASHBOARD_REUSE_IDLE_SECONDS");
    const gchar *webkit_memory_limit_text = g_getenv("WEBKIT_PROCESS_MEMORY_LIMIT_MB");
    guint deadline_seconds = 55;

    app->fixture_mode = env_truthy("WPE_FIXTURE_MODE");
    app->ha_url = normalize_ha_url(ha_url && *ha_url ? ha_url :
                                   (app->fixture_mode ? "http://fixture.local" : "http://homeassistant:8123"),
                                   &app->ha_origin, &error);
    if (!app->ha_url) {
        g_printerr("configuration error: HA_URL must be an HTTP(S) origin\n");
        g_clear_error(&error);
        return FALSE;
    }

    const gchar *configured_path = readiness_path && *readiness_path ?
        readiness_path : "lovelace/energy";
    if (!valid_dashboard_path(configured_path)) {
        g_printerr("configuration error: DASHBOARD_PATH is not a safe relative dashboard route\n");
        return FALSE;
    }
    app->readiness_path = g_strdup(configured_path);
    const gchar *configured_expected_text = expected_text && *expected_text ?
        expected_text : "";
    gsize expected_text_length = strnlen(
        configured_expected_text, MAX_EXPECTED_TEXT_BYTES + 1u);
    if (expected_text_length > MAX_EXPECTED_TEXT_BYTES ||
        !g_utf8_validate(configured_expected_text, (gssize) expected_text_length, NULL)) {
        g_printerr("configuration error: EXPECTED_TEXT must be UTF-8 and at most 256 bytes\n");
        return FALSE;
    }
    app->expected_text = g_strndup(configured_expected_text, expected_text_length);
    const gchar *configured_token_file = token_file && *token_file ?
        token_file : "/run/secrets/ha_token";
    if (strnlen(configured_token_file, MAX_CONFIG_PATH_BYTES + 1u) >
        MAX_CONFIG_PATH_BYTES) {
        g_printerr("configuration error: HA token file path is too long\n");
        return FALSE;
    }
    app->token_file = g_strdup(configured_token_file);
    const gchar *configured_fixture_path = fixture_path && *fixture_path ?
        fixture_path : "/opt/wpe-engine/fixtures/dashboard.html";
    if (strnlen(configured_fixture_path, MAX_CONFIG_PATH_BYTES + 1u) >
        MAX_CONFIG_PATH_BYTES) {
        g_printerr("configuration error: fixture file path is too long\n");
        return FALSE;
    }
    app->fixture_html_path = g_strdup(configured_fixture_path);

    app->port = DEFAULT_PORT;
    if (port_text && !parse_bounded_uint("PORT", port_text, 1, 65535, &app->port))
        return FALSE;
    if (deadline_text && !parse_bounded_uint("RENDER_DEADLINE_SECONDS", deadline_text,
                                              1, 55, &deadline_seconds))
        return FALSE;
    app->render_deadline_ms = deadline_seconds * 1000;
    app->dashboard_reuse_idle_seconds = DEFAULT_DASHBOARD_REUSE_IDLE_SECONDS;
    if (reuse_idle_text && !parse_bounded_uint(
            "DASHBOARD_REUSE_IDLE_SECONDS", reuse_idle_text, 0,
            MAX_DASHBOARD_REUSE_IDLE_SECONDS,
            &app->dashboard_reuse_idle_seconds))
        return FALSE;
    app->webkit_process_memory_limit_mb = DEFAULT_WEBKIT_PROCESS_MEMORY_LIMIT_MB;
    if (webkit_memory_limit_text && !parse_bounded_uint(
            "WEBKIT_PROCESS_MEMORY_LIMIT_MB", webkit_memory_limit_text,
            MIN_WEBKIT_PROCESS_MEMORY_LIMIT_MB,
            MAX_WEBKIT_PROCESS_MEMORY_LIMIT_MB,
            &app->webkit_process_memory_limit_mb)) {
        g_printerr("configuration error: WEBKIT_PROCESS_MEMORY_LIMIT_MB must be between 512 and 3072 MiB\n");
        return FALSE;
    }

    app->fixture_delay_ms = 350;
    app->fixture_first_delay_ms = 350;
    const gchar *delay = g_getenv("FIXTURE_DELAY_MS");
    const gchar *first_delay = g_getenv("FIXTURE_FIRST_DELAY_MS");
    if (delay && !parse_bounded_uint("FIXTURE_DELAY_MS", delay, 0, 30000,
                                     &app->fixture_delay_ms))
        return FALSE;
    if (first_delay && !parse_bounded_uint("FIXTURE_FIRST_DELAY_MS", first_delay, 0, 30000,
                                           &app->fixture_first_delay_ms))
        return FALSE;

    if (!app->fixture_mode) {
        app->token = read_token_file_bounded(app->token_file);
        if (!app->token) {
            g_printerr("configuration error: HA token file must be a readable regular file containing a bounded UTF-8 token\n");
            return FALSE;
        }
        if (g_str_has_prefix(app->token_file, "/tmp/wpe-ha-token.") &&
            g_unlink(app->token_file) < 0 && errno != ENOENT) {
            secure_free_string(app->token);
            app->token = NULL;
            g_printerr("configuration error: unable to remove temporary Home Assistant token file\n");
            return FALSE;
        }
    }
    g_unsetenv("HA_TOKEN_FILE");
    g_clear_pointer(&app->token_file, g_free);
    return TRUE;
}

static RenderRequest *
request_ref(RenderRequest *request)
{
    g_atomic_int_inc(&request->refs);
    return request;
}

static void
request_unref(RenderRequest *request)
{
    if (!g_atomic_int_dec_and_test(&request->refs))
        return;
    g_clear_object(&request->message);
    g_clear_object(&request->cancellable);
    g_free(request->path);
    g_free(request);
}

static void
disconnect_message_disconnected_handler(RenderRequest *request)
{
    if (!request->message_disconnected_handler)
        return;

    gulong handler = request->message_disconnected_handler;
    request->message_disconnected_handler = 0;
    if (g_signal_handler_is_connected(request->message, handler))
        g_signal_handler_disconnect(request->message, handler);
    request_unref(request);
}

static gboolean
current_request(RenderRequest *request)
{
    return request && !request->finished && request->app->active == request &&
           request->id == request->app->next_request_id &&
           request->view_generation == request->app->view_generation;
}

static void
remove_source(guint *source_id)
{
    if (*source_id) {
        guint id = *source_id;
        *source_id = 0;
        g_source_remove(id);
    }
}

static void
set_response_headers(SoupServerMessage *message, const gchar *content_type,
                     const gchar *cache_control, gsize body_length)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(message);
    gchar length[32];
    g_snprintf(length, sizeof(length), "%zu", body_length);
    soup_message_headers_replace(headers, "Content-Type", content_type);
    soup_message_headers_replace(headers, "Content-Length", length);
    soup_message_headers_replace(headers, "Cache-Control", cache_control ? cache_control : "no-store");
    soup_message_headers_replace(headers, "X-Content-Type-Options", "nosniff");
}

static void
respond_text(SoupServerMessage *message, guint status, const gchar *text)
{
    const gchar *body = text ? text : "request failed\n";
    gsize length = strlen(body);
    soup_server_message_set_status(message, status, NULL);
    soup_server_message_set_response(message, "text/plain; charset=utf-8",
                                     SOUP_MEMORY_COPY, body, length);
    set_response_headers(message, "text/plain; charset=utf-8", "no-store", length);
}

static void
respond_error(SoupServerMessage *message, guint status, const gchar *text)
{
    respond_text(message, status, text);
}

static void
handle_early_request(SoupServer *server, SoupServerMessage *message,
                     const gchar *path, GHashTable *query, gpointer user_data)
{
    (void) server;
    (void) path;
    (void) query;
    (void) user_data;

    SoupMessageBody *body = soup_server_message_get_request_body(message);
    if (body)
        soup_message_body_set_accumulate(body, FALSE);

    const gchar *method = soup_server_message_get_method(message);
    SoupMessageHeaders *request_headers = soup_server_message_get_request_headers(message);
    goffset content_length = soup_message_headers_get_content_length(request_headers);
    const gchar *transfer_encoding = soup_message_headers_get_one(request_headers,
                                                                    "Transfer-Encoding");
    if (g_strcmp0(method, "GET")) {
        respond_error(message, 405, "Only GET is supported.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message),
                                    "Allow", "GET");
    } else if (content_length > 0 || transfer_encoding) {
        respond_error(message, 400, "Request bodies are not supported.\n");
    }

    if (content_length > 0 || transfer_encoding || g_strcmp0(method, "GET"))
        soup_message_headers_replace(soup_server_message_get_response_headers(message),
                                    "Connection", "close");
}

static void
request_finish(RenderRequest *request, guint status, const gchar *text,
               GBytes *image, gboolean reset_view)
{
    App *app = request->app;
    if (request->finished)
        return;
    request->finished = TRUE;
    disconnect_message_disconnected_handler(request);
    remove_source(&request->timeout_source);
    remove_source(&request->readiness_source);
    remove_source(&request->wait_source);
    remove_source(&request->disconnect_source);
    g_cancellable_cancel(request->cancellable);

    if (image && status == 200) {
        app->dashboard_loaded = is_dashboard_uri(
            app, webkit_web_view_get_uri(app->view), request->path);
        app->dashboard_last_capture_at_us = app->dashboard_loaded ?
            g_get_monotonic_time() : 0;
        gsize length = 0;
        const gchar *bytes = g_bytes_get_data(image, &length);
        gint64 elapsed_us = g_get_monotonic_time() - request->started_at_us;
        guint64 elapsed_ms = elapsed_us > 0 ? (guint64) elapsed_us / 1000u : 0;
        g_message("Screenshot complete: request_id=%llu route=/%s status=200 elapsed_ms=%llu"
                  " image_bytes=%zu", (unsigned long long) request->id,
                  request->path, (unsigned long long) elapsed_ms, length);
        soup_server_message_set_status(request->message, 200, NULL);
        soup_server_message_set_response(request->message, "image/png", SOUP_MEMORY_COPY,
                                         bytes, length);
        set_response_headers(request->message, "image/png", "no-store", length);
    } else {
        gint64 elapsed_us = g_get_monotonic_time() - request->started_at_us;
        guint64 elapsed_ms = elapsed_us > 0 ? (guint64) elapsed_us / 1000u : 0;
        gchar *reason = g_strdup(text ? text : "unspecified renderer error");
        g_strchomp(reason);
        g_warning("Screenshot failed: request_id=%llu route=/%s status=%u elapsed_ms=%llu"
                  " reason=%s", (unsigned long long) request->id, request->path,
                  status, (unsigned long long) elapsed_ms, reason);
        g_free(reason);
        respond_error(request->message, status, text);
    }

    gboolean owns_active = app->active == request;
    if (owns_active)
        app->active = NULL;
    if (reset_view)
        app_recreate_view(app);
    soup_server_message_unpause(request->message);
    if (owns_active)
        request_unref(request);
}

static void
cancel_disconnected_request(RenderRequest *request)
{
    App *app = request->app;
    if (request->finished || app->active != request)
        return;

    gint64 elapsed_us = g_get_monotonic_time() - request->started_at_us;
    guint64 elapsed_ms = elapsed_us > 0 ? (guint64) elapsed_us / 1000u : 0;
    g_message("Screenshot client disconnected: request_id=%llu route=/%s elapsed_ms=%llu",
              (unsigned long long) request->id, request->path,
              (unsigned long long) elapsed_ms);

    request->finished = TRUE;
    disconnect_message_disconnected_handler(request);
    remove_source(&request->timeout_source);
    remove_source(&request->readiness_source);
    remove_source(&request->wait_source);
    remove_source(&request->disconnect_source);
    g_cancellable_cancel(request->cancellable);
    app->active = NULL;
    app_recreate_view(app);
    request_unref(request); /* Release the app->active reference. */
}

static void
on_message_disconnected(SoupServerMessage *message, gpointer user_data)
{
    (void) message;
    RenderRequest *request = request_ref(user_data);
    cancel_disconnected_request(request);
    request_unref(request); /* Release the temporary callback reference. */
}

static gboolean
check_client_connection(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (!current_request(request)) {
        request->disconnect_source = 0;
        return G_SOURCE_REMOVE;
    }

    GSocket *socket = soup_server_message_get_socket(request->message);
    gint socket_fd = socket ? g_socket_get_fd(socket) : -1;
    if (socket_fd < 0)
        return G_SOURCE_CONTINUE;

    gchar byte;
    ssize_t received = recv(socket_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (received == 0 ||
        (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        request->disconnect_source = 0;
        cancel_disconnected_request(request);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static guint
add_request_timeout(guint interval, GSourceFunc callback, RenderRequest *request)
{
    return g_timeout_add_full(G_PRIORITY_DEFAULT, interval, callback,
                              request_ref(request), (GDestroyNotify) request_unref);
}

static gboolean
same_origin(App *app, const gchar *candidate)
{
    GError *error = NULL;
    gchar *candidate_origin = origin_from_uri(candidate, FALSE, &error);
    g_clear_error(&error);
    gboolean same = candidate_origin && !g_strcmp0(candidate_origin, app->ha_origin);
    g_free(candidate_origin);
    return same;
}

static gboolean
is_dashboard_uri(App *app, const gchar *candidate, const gchar *dashboard_path)
{
    if (!candidate || !dashboard_path || !same_origin(app, candidate))
        return FALSE;

    GError *error = NULL;
    GUri *uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, &error);
    g_clear_error(&error);
    if (!uri)
        return FALSE;

    const gchar *uri_path = g_uri_get_path(uri);
    gboolean matches = uri_path && uri_path[0] == '/' &&
        !g_strcmp0(uri_path + 1, dashboard_path);
    g_uri_unref(uri);
    return matches;
}

static gboolean
dashboard_page_can_be_reused(App *app, const gchar *requested_path)
{
    if (!app->dashboard_loaded || !app->dashboard_reuse_idle_seconds ||
        app->dashboard_last_capture_at_us <= 0)
        return FALSE;

    gint64 idle_us = g_get_monotonic_time() - app->dashboard_last_capture_at_us;
    if (idle_us < 0 || idle_us >
        (gint64) app->dashboard_reuse_idle_seconds * G_USEC_PER_SEC)
        return FALSE;

    return is_dashboard_uri(app, webkit_web_view_get_uri(app->view), requested_path);
}

static gchar *
auth_bootstrap_script(App *app)
{
    gchar *quoted_origin = js_quote(app->ha_origin);
    gchar *quoted_token = js_quote(app->token ? app->token : "");
    gchar *script = g_strdup_printf(
        "(()=>{if(window.top!==window||location.origin!==%s)return;"
        "const clientId=new URL('/',%s).toString();"
        "localStorage.setItem('hassTokens',JSON.stringify({"
        "access_token:%s,token_type:'Bearer',expires_in:1800,"
        "hassUrl:clientId.replace(/\\/$/,''),clientId:clientId,"
        "expires:9999999999999,refresh_token:''}));})();",
        quoted_origin, quoted_origin, quoted_token);
    g_free(quoted_origin);
    secure_free_string(quoted_token);
    return script;
}

static gboolean
is_same_origin_uri(App *app, const gchar *uri_text)
{
    if (!uri_text || strnlen(uri_text, MAX_HA_URI_BYTES + 1u) > MAX_HA_URI_BYTES)
        return FALSE;

    return same_origin(app, uri_text);
}

static gboolean
on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                 WebKitPolicyDecisionType type, gpointer user_data)
{
    App *app = user_data;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        webkit_policy_decision_ignore(decision);
        if (view == app->view && app->active)
            request_finish(app->active, 502, "Home Assistant opened a blocked window.\n", NULL, TRUE);
        return TRUE;
    }

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *navigation_decision =
            WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *navigation =
            webkit_navigation_policy_decision_get_navigation_action(navigation_decision);
        WebKitURIRequest *request = navigation ?
            webkit_navigation_action_get_request(navigation) : NULL;
        const gchar *uri = request ? webkit_uri_request_get_uri(request) : NULL;
        if (!uri || !is_same_origin_uri(app, uri)) {
            webkit_policy_decision_ignore(decision);
            if (view == app->view && app->active)
                request_finish(app->active, 502,
                               "Home Assistant navigation outside the configured origin was blocked.\n",
                               NULL, TRUE);
            return TRUE;
        }
    }

    return FALSE;
}

static WebKitWebView *
create_view(App *app)
{
    app->view_generation++;
    WebKitUserContentManager *manager = webkit_user_content_manager_new();
    if (!app->fixture_mode) {
        gchar *source = auth_bootstrap_script(app);
        WebKitUserScript *script = webkit_user_script_new(
            source, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
        webkit_user_content_manager_add_script(manager, script);
        webkit_user_script_unref(script);
        secure_free_string(source);
    }

    WebKitWebView *view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "display", app->display,
        "web-context", app->web_context,
        "network-session", app->network_session,
        "user-content-manager", manager,
        NULL));
    g_object_unref(manager);
    if (!view)
        return NULL;

    WPEView *wpe_view = webkit_web_view_get_wpe_view(view);
    if (!wpe_view || !WPE_IS_VIEW_HEADLESS(wpe_view) ||
        !WPE_IS_DISPLAY_HEADLESS(wpe_view_get_display(wpe_view))) {
        g_object_unref(view);
        return NULL;
    }
    wpe_view_resized(wpe_view, IMAGE_WIDTH, IMAGE_HEIGHT);
    wpe_view_set_visible(wpe_view, TRUE);

    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), app);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_uri_changed), app);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), app);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), app);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), app);
    app->wpe_view = wpe_view;
    app->renderer_ready = TRUE;
    return view;
}

static void
app_recreate_view(App *app)
{
    app->renderer_ready = FALSE;
    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;
    app->wpe_view = NULL;
    if (app->view) {
        g_signal_handlers_disconnect_by_data(app->view, app);
        g_clear_object(&app->view);
    }
    app->view = create_view(app);
}

static gboolean
fixture_load(App *app, RenderRequest *request)
{
    gint fd = g_open(app->fixture_html_path,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);
    struct stat metadata;
    if (fd < 0 || fstat(fd, &metadata) < 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_size < 0 ||
        (guint64) metadata.st_size > MAX_FIXTURE_HTML_BYTES) {
        if (fd >= 0)
            close(fd);
        request_finish(request, 503, "Fixture renderer is unavailable.\n", NULL, TRUE);
        return FALSE;
    }

    gchar *html = g_malloc(MAX_FIXTURE_HTML_BYTES + 1u);
    gsize html_length = 0;
    gboolean read_ok = TRUE;
    while (html_length <= MAX_FIXTURE_HTML_BYTES) {
        ssize_t count = read(fd, html + html_length,
                             MAX_FIXTURE_HTML_BYTES + 1u - html_length);
        if (count > 0) {
            html_length += (gsize) count;
            if (html_length > MAX_FIXTURE_HTML_BYTES) {
                read_ok = FALSE;
                break;
            }
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            read_ok = FALSE;
            break;
        }
    }
    close(fd);
    if (!read_ok || memchr(html, '\0', html_length) ||
        !g_utf8_validate(html, (gssize) html_length, NULL)) {
        g_free(html);
        request_finish(request, 503, "Fixture renderer is unavailable.\n", NULL, TRUE);
        return FALSE;
    }
    html[html_length] = '\0';

    guint delay = app->fixture_load_count++ == 0 ? app->fixture_first_delay_ms : app->fixture_delay_ms;
    gchar *delay_text = g_strdup_printf("%u", delay);
    static const gchar marker[] = "__FIXTURE_DELAY_MS__";
    const gchar *cursor = html;
    const gchar *end = html + html_length;
    GString *replaced = g_string_sized_new(html_length);
    while (cursor < end) {
        const gchar *match = g_strstr_len(cursor, (gssize) (end - cursor), marker);
        if (!match)
            break;
        g_string_append_len(replaced, cursor, (gssize) (match - cursor));
        g_string_append(replaced, delay_text);
        cursor = match + sizeof(marker) - 1u;
    }
    g_string_append_len(replaced, cursor, (gssize) (end - cursor));
    g_free(html);
    g_free(delay_text);
    gchar *base_uri = g_strdup_printf("%s/%s", app->ha_url, request->path);
    webkit_web_view_load_html(app->view, replaced->str, base_uri);
    g_free(base_uri);
    g_string_free(replaced, TRUE);
    return TRUE;
}

static gboolean
ready_script(App *app, RenderRequest *request, gchar **script_out)
{
    const gchar *expected_text = !g_strcmp0(request->path, app->readiness_path) ?
        app->expected_text : "";
    const gchar *fixture = app->fixture_mode ?
        "if(!document.querySelector('[data-fixture-ready=true]'))return 'wait';" :
        "if(location.pathname.indexOf('/auth/authorize')===0)return 'auth';"
        "const ha=document.querySelector('home-assistant');if(!ha)return 'wait';"
        "const main=ha.shadowRoot&&ha.shadowRoot.querySelector('home-assistant-main');"
        "const resolver=main&&main.shadowRoot&&main.shadowRoot.querySelector('partial-panel-resolver');"
        "if(!resolver||resolver._loading)return 'wait';const panel=resolver.children[0];"
        "if(!panel||('_loading'in panel&&panel._loading))return 'wait';";
    if (!expected_text[0]) {
        *script_out = g_strdup_printf(
            "(()=>{%sif(document.fonts&&document.fonts.status!=='loaded')return 'wait';"
            "return 'ready';})()",
            fixture);
        return TRUE;
    }

    gchar *expected = js_quote(expected_text);
    *script_out = g_strdup_printf(
        "(()=>{%sif(document.fonts&&document.fonts.status!=='loaded')return 'wait';"
        "let text='';const walk=root=>{if(!root)return;text+=' '+(root.textContent||'');"
        "for(const e of root.querySelectorAll('*'))if(e.shadowRoot)walk(e.shadowRoot);};"
        "walk(document.body);return text.toLocaleLowerCase().includes(%s.toLocaleLowerCase())?"
        "'ready':'wait';})()",
        fixture, expected);
    secure_free_string(expected);
    return TRUE;
}

static gboolean
capture_timer(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (!current_request(request))
        return G_SOURCE_REMOVE;
    request->wait_source = 0;
    webkit_web_view_get_snapshot(request->app->view, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE, request->cancellable,
                                 on_snapshot_ready, request_ref(request));
    return G_SOURCE_REMOVE;
}

static void
on_readiness_result(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    request->readiness_pending = FALSE;
    if (!current_request(request)) {
        g_clear_object(&value);
        g_clear_error(&error);
        request_unref(request);
        return;
    }
    if (error || !value) {
        g_clear_error(&error);
        g_clear_object(&value);
        request_finish(request, 502, "Home Assistant readiness check failed.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    gchar *state = jsc_value_to_string(value);
    g_object_unref(value);
    if (state && !strcmp(state, "auth")) {
        g_free(state);
        request_finish(request, 502, "Home Assistant authentication failed.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    gboolean ready = state && !strcmp(state, "ready");
    g_free(state);
    if (ready) {
        remove_source(&request->readiness_source);
        guint delay = request->wait_ms + 16;
        request->wait_source = add_request_timeout(delay ? delay : 1, capture_timer, request);
    }
    request_unref(request);
}

static gboolean
readiness_tick(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (!current_request(request))
        return G_SOURCE_REMOVE;
    if (request->readiness_pending)
        return G_SOURCE_CONTINUE;

    gchar *script = NULL;
    ready_script(request->app, request, &script);
    request->readiness_pending = TRUE;
    webkit_web_view_evaluate_javascript(request->app->view, script, -1, NULL, NULL,
                                        request->cancellable, on_readiness_result,
                                        request_ref(request));
    g_free(script);
    return G_SOURCE_CONTINUE;
}

static void
start_readiness_checks(RenderRequest *request)
{
    if (request->readiness_source)
        return;

    request->readiness_source = add_request_timeout(50, readiness_tick, request);
    readiness_tick(request);
}

static void
on_snapshot_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    WebKitImage *image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
    if (!current_request(request)) {
        g_clear_object(&image);
        g_clear_error(&error);
        request_unref(request);
        return;
    }
    if (!image || error) {
        g_clear_object(&image);
        g_clear_error(&error);
        request_finish(request, 503, "WPE could not capture the rendered frame.\n", NULL, TRUE);
        request_unref(request);
        return;
    }

    gint width = webkit_image_get_width(image);
    gint height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    /* The returned GBytes is owned by WebKitImage; keep image alive below. */
    GBytes *source_bytes = webkit_image_as_bytes(image);
    gsize source_length = 0;
    if (!source_bytes || width != IMAGE_WIDTH || height != IMAGE_HEIGHT) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }

    const guint8 *source_pixels = g_bytes_get_data(source_bytes, &source_length);
    const gsize row_bytes = (gsize) IMAGE_WIDTH * 4u;
    const gsize image_height = (gsize) IMAGE_HEIGHT;
    const gsize stride_bytes = (gsize) stride;
    if (stride_bytes < row_bytes ||
        stride_bytes > (gsize) MAX_IMAGE_BYTES / image_height) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }

    /* Bound the product before multiplying, then validate the last row. */
    const gsize frame_bytes = stride_bytes * image_height;
    const gsize last_row_offset = frame_bytes - stride_bytes;
    if (last_row_offset > source_length ||
        row_bytes > source_length - last_row_offset) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }

    gboolean opaque = TRUE;
    for (gint y = 0; y < height && opaque; ++y) {
        const guint8 *row = source_pixels + (gsize) y * stride_bytes;
        for (gint x = 0; x < width; ++x) {
            if (row[x * 4 + 3] != 255) {
                opaque = FALSE;
                break;
            }
        }
    }

    /* Opaque frames can be encoded from WebKit's BGRA bytes directly. */
    gsize pixel_length = row_bytes * image_height;
    guint8 *rgba = NULL;
    const guint8 *png_pixels = source_pixels;
    png_int_32 png_stride = (png_int_32) stride_bytes;
    png_uint_32 png_format = PNG_FORMAT_BGRA;
    if (!opaque) {
        rgba = g_malloc(pixel_length);
        for (gint y = 0; y < height; ++y) {
            const guint8 *src = source_pixels + (gsize) y * stride_bytes;
            guint8 *dst = rgba + (gsize) y * row_bytes;
            for (gint x = 0; x < width; ++x) {
                guint8 b = src[x * 4 + 0];
                guint8 g = src[x * 4 + 1];
                guint8 r = src[x * 4 + 2];
                guint8 a = src[x * 4 + 3];
                if (a && a < 255) {
                    r = (guint8) MIN(255, ((guint) r * 255 + a / 2) / a);
                    g = (guint8) MIN(255, ((guint) g * 255 + a / 2) / a);
                    b = (guint8) MIN(255, ((guint) b * 255 + a / 2) / a);
                }
                dst[x * 4 + 0] = r;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = b;
                dst[x * 4 + 3] = a;
            }
        }
        png_pixels = rgba;
        png_stride = width * 4;
        png_format = PNG_FORMAT_RGBA;
    }

    png_image png = { .version = PNG_IMAGE_VERSION,
                      .width = IMAGE_WIDTH,
                      .height = IMAGE_HEIGHT,
                      .format = png_format,
                      .flags = PNG_IMAGE_FLAG_FAST };
    size_t png_length = 0;
    char *png_data = NULL;
    FILE *png_stream = open_memstream(&png_data, &png_length);
    if (!png_stream) {
        g_free(rgba);
        g_object_unref(image);
        request_finish(request, 503, "PNG output buffer could not be allocated.\n", NULL, FALSE);
        request_unref(request);
        return;
    }
    gboolean encoded = png_image_write_to_stdio(&png, png_stream, 0,
                                                 png_pixels, png_stride, NULL);
    gint close_status = fclose(png_stream);
    g_free(rgba);
    g_object_unref(image);
    if (!encoded || close_status || !png_length || png_length > MAX_IMAGE_BYTES) {
        free(png_data);
        request_finish(request, 503, "PNG encoding failed or exceeded the image limit.\n", NULL, FALSE);
        request_unref(request);
        return;
    }
    GBytes *result_bytes = g_bytes_new_with_free_func(
        (const guint8 *) png_data, png_length, free, png_data);
    request_finish(request, 200, NULL, result_bytes, FALSE);
    g_bytes_unref(result_bytes);
    request_unref(request);
}

static gboolean
render_deadline(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (current_request(request)) {
        request->timeout_source = 0;
        request_finish(request, 504, "Dashboard render deadline exceeded.\n", NULL, TRUE);
    }
    return G_SOURCE_REMOVE;
}

static void
on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
    App *app = user_data;
    if (view != app->view || !app->active)
        return;

    RenderRequest *request = app->active;
    if (event == WEBKIT_LOAD_FINISHED) {
        const gchar *uri = webkit_web_view_get_uri(view);
        if (uri && same_origin(app, uri)) {
            GError *error = NULL;
            GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
            g_clear_error(&error);
            const gchar *path = parsed ? g_uri_get_path(parsed) : NULL;
            if (path && g_str_has_prefix(path, "/auth/authorize")) {
                if (parsed)
                    g_uri_unref(parsed);
                request_finish(request, 502, "Home Assistant authentication failed.\n", NULL, TRUE);
                return;
            }
            if (parsed)
                g_uri_unref(parsed);
        }
        start_readiness_checks(request);
    }
}

static void
on_uri_changed(WebKitWebView *view, GParamSpec *property, gpointer user_data)
{
    (void) property;
    App *app = user_data;
    if (view != app->view || !app->active)
        return;

    const gchar *uri = webkit_web_view_get_uri(view);
    if (uri && !is_same_origin_uri(app, uri))
        request_finish(app->active, 502,
                       "Home Assistant redirected outside the configured origin.\n",
                       NULL, TRUE);
}

static gboolean
on_load_failed(WebKitWebView *view, WebKitLoadEvent event, const gchar *uri,
               GError *error, gpointer user_data)
{
    (void) event;
    (void) uri;
    (void) error;
    App *app = user_data;
    if (view == app->view && app->active)
        request_finish(app->active, 502, "Home Assistant page load failed.\n", NULL, TRUE);
    return TRUE;
}

static gboolean
on_permission_request(WebKitWebView *view, WebKitPermissionRequest *permission,
                       gpointer user_data)
{
    (void) view;
    (void) user_data;
    webkit_permission_request_deny(permission);
    return TRUE;
}

static void
on_web_process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                          gpointer user_data)
{
    (void) reason;
    App *app = user_data;
    if (view != app->view)
        return;
    if (app->active)
        request_finish(app->active, 503, "WPE renderer process terminated.\n", NULL, TRUE);
    else
        app_recreate_view(app);
}

static gboolean
decode_query_value(const gchar *value, gchar **decoded)
{
    for (const gchar *p = value; *p; ++p) {
        if (*p == '%') {
            if (!p[1] || !p[2] || !g_ascii_isxdigit(p[1]) ||
                !g_ascii_isxdigit(p[2]))
                return FALSE;
            p += 2;
        }
    }
    *decoded = g_uri_unescape_string(value, NULL);
    if (!*decoded || !g_utf8_validate(*decoded, -1, NULL)) {
        g_clear_pointer(decoded, g_free);
        return FALSE;
    }
    for (const guchar *p = (const guchar *) *decoded; *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) {
            g_clear_pointer(decoded, g_free);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
parse_capture_query(GUri *uri, CaptureOptions *options, guint *status,
                    const gchar **error_text)
{
    options->wait_ms = DEFAULT_WAIT_MS;
    const gchar *query = g_uri_get_query(uri);
    if (!query || !*query)
        return TRUE;

    guint parameter_count = 1;
    for (const gchar *p = query; *p; ++p) {
        if (*p == '&' && ++parameter_count > MAX_QUERY_PARAMETERS) {
            *status = 400;
            *error_text = "Too many query parameters.\n";
            return FALSE;
        }
    }

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar **pairs = g_strsplit(query, "&", -1);
    gboolean valid = TRUE;
    for (guint i = 0; pairs[i] && valid; ++i) {
        if (!*pairs[i]) {
            *status = 400;
            *error_text = "Malformed query string.\n";
            valid = FALSE;
            break;
        }
        gchar *equal = strchr(pairs[i], '=');
        gchar *raw_key = equal ? g_strndup(pairs[i], (gsize) (equal - pairs[i])) : g_strdup(pairs[i]);
        gchar *raw_value = equal ? g_strdup(equal + 1) : g_strdup("");
        gchar *key = NULL;
        gchar *value = NULL;
        if (!decode_query_value(raw_key, &key) || !*key ||
            !decode_query_value(raw_value, &value)) {
            *status = 400;
            *error_text = "Malformed query parameter.\n";
            valid = FALSE;
        }
        g_free(raw_key);
        g_free(raw_value);
        if (!valid) {
            g_free(key);
            g_free(value);
            break;
        }
        if (g_hash_table_contains(seen, key)) {
            *status = 400;
            *error_text = "Duplicate query parameter.\n";
            g_free(key);
            g_free(value);
            valid = FALSE;
            break;
        }
        g_hash_table_insert(seen, key, value);
    }

    if (valid) {
        GHashTableIter iter;
        gpointer key_ptr, value_ptr;
        g_hash_table_iter_init(&iter, seen);
        while (valid && g_hash_table_iter_next(&iter, &key_ptr, &value_ptr)) {
            const gchar *key = key_ptr;
            const gchar *value = value_ptr;
            if (!strcmp(key, "viewport")) {
                if (strcmp(value, "1280x800")) {
                    *status = 422;
                    *error_text = "Only viewport=1280x800 is supported.\n";
                    valid = FALSE;
                }
            } else if (!strcmp(key, "zoom")) {
                if (!*value) {
                    *status = 400;
                    *error_text = "Malformed zoom parameter.\n";
                    valid = FALSE;
                    break;
                }
                for (const gchar *p = value; *p; ++p) {
                    if (!(g_ascii_isdigit(*p) || *p == '.' || *p == '-' || *p == '+')) {
                        *status = 400;
                        *error_text = "Malformed zoom parameter.\n";
                        valid = FALSE;
                        break;
                    }
                }
                if (!valid)
                    break;
                errno = 0;
                gchar *end = NULL;
                gdouble zoom = g_ascii_strtod(value, &end);
                if (errno || !end || *end || !isfinite(zoom)) {
                    *status = 400;
                    *error_text = "Malformed zoom parameter.\n";
                    valid = FALSE;
                } else if (fabs(zoom - 1.0) > 0.0000001) {
                    *status = 422;
                    *error_text = "Only zoom=1.0 is supported.\n";
                    valid = FALSE;
                }
            } else if (!strcmp(key, "wait")) {
                guint parsed = 0;
                if (!parse_bounded_uint("wait", value, 0, MAX_WAIT_MS, &parsed)) {
                    *status = 400;
                    *error_text = "wait must be an integer from 0 through 30000.\n";
                    valid = FALSE;
                } else {
                    options->wait_ms = parsed;
                }
            } else if (!strcmp(key, "format")) {
                if (strcmp(value, "png")) {
                    *status = 422;
                    *error_text = "Only format=png is supported.\n";
                    valid = FALSE;
                }
            } else if (!strcmp(key, "theme") || !strcmp(key, "dark") ||
                       !strcmp(key, "lang")) {
                *status = 422;
                *error_text = "theme, dark, and lang are not supported.\n";
                valid = FALSE;
            } else {
                *status = 400;
                *error_text = "Unknown query parameter.\n";
                valid = FALSE;
            }
        }
    }

    g_strfreev(pairs);
    g_hash_table_unref(seen);
    return valid;
}

static void
handle_health(SoupServerMessage *message, App *app, const gchar *path)
{
    const gchar *method = soup_server_message_get_method(message);
    if (g_strcmp0(method, "GET")) {
        respond_error(message, 405, "Only GET is supported.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Allow", "GET");
        return;
    }
    if (path && !strcmp(path, "/healthz")) {
        respond_text(message, 200, "ok\n");
        return;
    }
    if (path && !strcmp(path, "/readyz")) {
        if (app->renderer_ready && app->view && app->wpe_view)
            respond_text(message, 200, "ready\n");
        else
            respond_text(message, 503, "renderer unavailable\n");
        return;
    }
    respond_error(message, 404, "Not found.\n");
}

static void
handle_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
               GHashTable *query_table, gpointer user_data)
{
    (void) server;
    (void) query_table;
    App *app = user_data;
    const gchar *method = soup_server_message_get_method(message);
    GUri *uri = soup_server_message_get_uri(message);
    const gchar *raw_path = uri ? g_uri_get_path(uri) : NULL;

    const gchar *query = uri ? g_uri_get_query(uri) : NULL;
    gsize raw_path_length = raw_path ?
        strnlen(raw_path, MAX_DASHBOARD_PATH_BYTES + 2u) : 0;
    gsize query_length = query ? strnlen(query, MAX_QUERY_BYTES + 1u) : 0;
    if (raw_path_length > MAX_DASHBOARD_PATH_BYTES + 1u ||
        query_length > MAX_QUERY_BYTES) {
        respond_error(message, 400, "Request target is too long.\n");
        return;
    }

    if (!g_strcmp0(raw_path, "/healthz") || !g_strcmp0(raw_path, "/readyz")) {
        handle_health(message, app, raw_path);
        return;
    }
    if (g_strcmp0(method, "GET")) {
        respond_error(message, 405, "Only GET is supported.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Allow", "GET");
        return;
    }
    gsize request_path_length = path ?
        strnlen(path, MAX_DASHBOARD_PATH_BYTES + 2u) : 0;
    gboolean valid_route = raw_path && path && raw_path[0] == '/' &&
        request_path_length > 1u &&
        request_path_length <= MAX_DASHBOARD_PATH_BYTES + 1u &&
        !strcmp(raw_path, path) && valid_dashboard_path(path + 1);
    if (!valid_route) {
        respond_error(message, 400,
                      "Dashboard path must be a safe relative Home Assistant route.\n");
        return;
    }
    if (!app->renderer_ready || !app->view) {
        respond_error(message, 503, "WPE renderer is unavailable.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Retry-After", "1");
        return;
    }
    if (app->active) {
        respond_error(message, 503, "Renderer is busy; retry shortly.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Retry-After", "1");
        return;
    }

    CaptureOptions options = { .wait_ms = DEFAULT_WAIT_MS };
    guint error_status = 400;
    const gchar *error_text = "Malformed capture request.\n";
    if (!uri || !parse_capture_query(uri, &options, &error_status, &error_text)) {
        respond_error(message, error_status, error_text);
        return;
    }

    RenderRequest *request = g_new0(RenderRequest, 1);
    request->refs = 1;
    request->app = app;
    request->message = g_object_ref(message);
    request->cancellable = g_cancellable_new();
    request->id = ++app->next_request_id;
    request->view_generation = app->view_generation;
    request->wait_ms = options.wait_ms;
    request->started_at_us = g_get_monotonic_time();
    request->path = g_strdup(path + 1);
    app->active = request;
    request->message_disconnected_handler = g_signal_connect(
        message, "disconnected", G_CALLBACK(on_message_disconnected), request_ref(request));
    request->disconnect_source = add_request_timeout(100, check_client_connection,
                                                      request);
    g_message("Screenshot started: request_id=%llu route=/%s deadline_ms=%u",
              (unsigned long long) request->id, request->path,
              app->render_deadline_ms);
    soup_server_message_pause(message);
    request->timeout_source = add_request_timeout(app->render_deadline_ms,
                                                   render_deadline, request);

    if (dashboard_page_can_be_reused(app, request->path)) {
        start_readiness_checks(request);
        return;
    }

    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;

    if (app->fixture_mode) {
        if (!fixture_load(app, request))
            return;
    } else {
        gchar *uri_to_load = g_strdup_printf("%s/%s", app->ha_url, request->path);
        webkit_web_view_load_uri(app->view, uri_to_load);
        g_free(uri_to_load);
    }
}

static gboolean
init_app(App *app)
{
    app->loop = g_main_loop_new(NULL, FALSE);
    app->display = wpe_display_headless_new();
    WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(
        pressure, app->webkit_process_memory_limit_mb);
    /* Raise the strict threshold first so the conservative threshold setter
     * validates against the intended 0.85 value instead of WebKit's default. */
    webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.85);
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.65);
    webkit_memory_pressure_settings_set_kill_threshold(pressure, 0.0);
    webkit_memory_pressure_settings_set_poll_interval(pressure, 5.0);
    webkit_network_session_set_memory_pressure_settings(pressure);
    app->web_context = g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
                                    "memory-pressure-settings", pressure,
                                    NULL);
    webkit_memory_pressure_settings_free(pressure);
    app->network_session = webkit_network_session_new_ephemeral();
    webkit_network_session_set_persistent_credential_storage_enabled(app->network_session, FALSE);
    webkit_network_session_set_tls_errors_policy(app->network_session, WEBKIT_TLS_ERRORS_POLICY_FAIL);
    webkit_web_context_set_cache_model(app->web_context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    app->view = create_view(app);
    if (!app->view)
        return FALSE;

    app->server = soup_server_new("server-header", "wpe-screenshot-engine/0.1.6", NULL);
    soup_server_add_early_handler(app->server, NULL, handle_early_request, NULL, NULL);
    soup_server_add_handler(app->server, NULL, handle_request, app, NULL);
    GError *error = NULL;
    if (!soup_server_listen_all(app->server, app->port, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
        g_printerr("startup error: HTTP listener failed\n");
        g_clear_error(&error);
        return FALSE;
    }
    return TRUE;
}

static void
app_clear(App *app)
{
    if (app->server)
        soup_server_disconnect(app->server);
    g_clear_object(&app->server);
    g_clear_object(&app->view);
    g_clear_object(&app->network_session);
    g_clear_object(&app->web_context);
    g_clear_object(&app->display);
    if (app->loop)
        g_main_loop_unref(app->loop);
    g_free(app->ha_url);
    g_free(app->ha_origin);
    g_free(app->token_file);
    secure_free_string(app->token);
    g_free(app->readiness_path);
    g_free(app->expected_text);
    g_free(app->fixture_html_path);
}

int
main(void)
{
    App app = {0};
    if (!load_runtime_config(&app)) {
        app_clear(&app);
        return 78;
    }
    if (!init_app(&app)) {
        app_clear(&app);
        return 70;
    }

    g_message("Screenshot API ready: internal_port=%u renderer=ready", app.port);
    g_message("Capture routes may target any safe relative path on the configured Home Assistant origin");
    if (app.expected_text[0])
        g_message("Readiness text check enabled for route=/%s", app.readiness_path);
    g_main_loop_run(app.loop);
    app_clear(&app);
    return 0;
}
