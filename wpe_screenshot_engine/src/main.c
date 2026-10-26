#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <webp/encode.h>

#define DEFAULT_IMAGE_WIDTH 1280u
#define DEFAULT_IMAGE_HEIGHT 800u
#define MAX_FULL_DOCUMENT_SNAPSHOT_HEIGHT DEFAULT_IMAGE_HEIGHT
#define MAX_VIEWPORT_DIMENSION 4096u
#define MAX_AUTO_HEIGHT 4000u
#define MAX_RENDER_DEADLINE_SECONDS 300u
#define MAX_OUTPUT_IMAGE_BYTES (20u * 1024u * 1024u)
#define MAX_RAW_IMAGE_BYTES (60u * 1024u * 1024u)
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
#define MAX_THEME_BYTES 1024u
#define MAX_LANGUAGE_BYTES 64u

typedef enum {
    OUTPUT_FORMAT_PNG,
    OUTPUT_FORMAT_JPEG,
    OUTPUT_FORMAT_WEBP,
    OUTPUT_FORMAT_BMP
} OutputFormat;

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
    guint tile_source;
    guint disconnect_source;
    gulong message_disconnected_handler;
    gulong message_finished_handler;
    guint readiness_error_count;
    gboolean readiness_pending;
    gboolean reused_page;
    gboolean frontend_options_applied;
    gboolean frontend_options_verified;
    gboolean frontend_options_pending;
    gboolean capture_scheduled;
    gboolean auto_height_truncated;
    gboolean auto_height;
    gboolean tiled_auto_height;
    gboolean dark_mode;
    gboolean has_dark_mode;
    gboolean finished;
    guint wait_ms;
    guint width;
    guint height;
    guint capture_height;
    guint capture_offset_y;
    guint tile_scroll_y;
    guint8 *capture_pixels;
    gsize capture_pixels_length;
    gdouble zoom;
    OutputFormat format;
    gint64 started_at_us;
    gchar *path;
    gchar *encoded_path;
    gchar *theme;
    gchar *language;
    gchar *last_readiness_state;
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
    guint recreate_view_source;
    guint idle_release_source;
    guint port;
    guint render_deadline_ms;
    guint dashboard_reuse_idle_seconds;
    guint webkit_process_memory_limit_mb;
    gboolean renderer_ready;
    gboolean renderer_idle_released;
    gboolean dashboard_loaded;
    gint64 dashboard_last_capture_at_us;
    gchar *dashboard_requested_path;
    gboolean fixture_mode;
    gboolean fixture_ha_readiness;
    guint fixture_load_count;
    guint fixture_readiness_error_count;
    guint fixture_view_recreate_failure_count;
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
    guint width;
    guint height;
    guint wait_ms;
    gboolean auto_height;
    gboolean dark_mode;
    gboolean has_dark_mode;
    gdouble zoom;
    OutputFormat format;
    gchar *theme;
    gchar *language;
} CaptureOptions;

static RenderRequest *request_ref(RenderRequest *request);
static void request_unref(RenderRequest *request);
static void disconnect_message_handlers(RenderRequest *request);
static void cancel_disconnected_request(RenderRequest *request);
static void app_recreate_view(App *app);
static void app_schedule_recreate_view(App *app);
static void app_schedule_idle_release(App *app);
static gboolean app_ensure_view(App *app);
static gboolean current_request(RenderRequest *request);
static gboolean same_origin(App *app, const gchar *candidate);
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
static void on_document_height_ready(GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static void start_tiled_scroll(RenderRequest *request);
static void on_tiled_scroll_ready(GObject *source, GAsyncResult *result,
                                  gpointer user_data);
static void on_tiled_scroll_restored(GObject *source, GAsyncResult *result,
                                     gpointer user_data);
static GBytes *encode_snapshot(RenderRequest *request, const guint8 *pixels,
                               guint stride, guint width, guint height,
                               gboolean *too_large);
static void encode_snapshot_async(RenderRequest *request, const guint8 *pixels,
                                  guint stride, guint width, guint height,
                                  GBytes *source_bytes, guint8 *owned_pixels,
                                  gboolean reset_view);
static void on_frontend_options_ready(GObject *source, GAsyncResult *result,
                                      gpointer user_data);
static void start_readiness_checks(RenderRequest *request);
static const gchar *output_format_mime(OutputFormat format);
static void on_message_terminated(SoupServerMessage *message, gpointer user_data);
static gboolean check_client_connection(gpointer user_data);
static gboolean normalize_dashboard_uri_path(const gchar *uri_path,
                                             gchar **normalized_path,
                                             gchar **encoded_path);

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
parse_config_uint(const gchar *name, const gchar *value, guint minimum,
                  guint maximum, guint *result)
{
    if (!value || !*value)
        return TRUE;
    if (parse_bounded_uint(name, value, minimum, maximum, result))
        return TRUE;

    g_printerr("configuration error: %s must be an integer from %u through %u\n",
               name, minimum, maximum);
    return FALSE;
}

static gboolean
valid_dashboard_path(const gchar *path)
{
    if (!path || !*path)
        return FALSE;
    gsize path_length = strnlen(path, MAX_DASHBOARD_PATH_BYTES + 1u);
    if (path_length > MAX_DASHBOARD_PATH_BYTES || path[0] == '/' ||
        path[path_length - 1] == '/' || !g_utf8_validate(path, -1, NULL) ||
        strstr(path, "://") || strstr(path, "//") ||
        strchr(path, '@') || strchr(path, '\\') || strchr(path, '?') ||
        strchr(path, '#'))
        return FALSE;

    const gchar *segment = path;
    for (const gchar *p = path;; ++p) {
        if (*p == '/' || *p == '\0') {
            gsize length = (gsize) (p - segment);
            if (!length || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return FALSE;
            for (const gchar *c = segment; c < p; c = g_utf8_next_char(c)) {
                gunichar character = g_utf8_get_char(c);
                if (character < 0x20 || character == 0x7f)
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
    const gchar *fixture_path = NULL;
    const gchar *port_text = g_getenv("PORT");
    const gchar *deadline_text = g_getenv("RENDER_DEADLINE_SECONDS");
    const gchar *reuse_idle_text = g_getenv("DASHBOARD_REUSE_IDLE_SECONDS");
    const gchar *webkit_memory_limit_text = g_getenv("WEBKIT_PROCESS_MEMORY_LIMIT_MB");
    guint deadline_seconds = DEFAULT_RENDER_DEADLINE_MS / 1000u;

    app->fixture_mode = env_truthy("WPE_FIXTURE_MODE");
    if (app->fixture_mode) {
        fixture_path = g_getenv("FIXTURE_HTML_PATH");
        app->fixture_ha_readiness = env_truthy("FIXTURE_HA_READINESS");
    }
    const gchar *configured_token_file = token_file && *token_file ?
        token_file : "/run/secrets/ha_token";
    if (strnlen(configured_token_file, MAX_CONFIG_PATH_BYTES + 1u) >
        MAX_CONFIG_PATH_BYTES) {
        g_printerr("configuration error: HA token file path is too long\n");
        return FALSE;
    }
    app->token_file = g_strdup(configured_token_file);
    g_unsetenv("HA_TOKEN_FILE");

    if (!app->fixture_mode) {
        app->token = read_token_file_bounded(app->token_file);
        gboolean temporary_token =
            g_str_has_prefix(app->token_file, "/tmp/wpe-ha-token.");
        gboolean token_file_removed = !temporary_token ||
            g_unlink(app->token_file) == 0 || errno == ENOENT;
        if (!app->token) {
            g_printerr("configuration error: HA token file must be a readable regular file containing a bounded UTF-8 token\n");
            return FALSE;
        }
        if (!token_file_removed) {
            g_printerr("configuration error: unable to remove temporary Home Assistant token file\n");
            return FALSE;
        }
    }

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
    gchar *configured_uri_path = g_strdup_printf("/%s", configured_path);
    gchar *normalized_readiness_path = NULL;
    gchar *encoded_readiness_path = NULL;
    gboolean normalized_readiness = normalize_dashboard_uri_path(
        configured_uri_path, &normalized_readiness_path,
        &encoded_readiness_path);
    g_free(configured_uri_path);
    g_free(encoded_readiness_path);
    if (!normalized_readiness) {
        g_printerr("configuration error: DASHBOARD_PATH is not a safe relative dashboard route\n");
        return FALSE;
    }
    app->readiness_path = normalized_readiness_path;
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
    if (app->fixture_mode) {
        const gchar *configured_fixture_path = fixture_path && *fixture_path ?
            fixture_path : "/opt/wpe-engine/fixtures/dashboard.html";
        if (strnlen(configured_fixture_path, MAX_CONFIG_PATH_BYTES + 1u) >
            MAX_CONFIG_PATH_BYTES) {
            g_printerr("configuration error: fixture file path is too long\n");
            return FALSE;
        }
        app->fixture_html_path = g_strdup(configured_fixture_path);
    }

    app->port = DEFAULT_PORT;
    if (!parse_config_uint("PORT", port_text, 1, 65535, &app->port))
        return FALSE;
    if (!parse_config_uint("RENDER_DEADLINE_SECONDS", deadline_text, 1,
                           MAX_RENDER_DEADLINE_SECONDS, &deadline_seconds))
        return FALSE;
    app->render_deadline_ms = deadline_seconds * 1000;
    app->dashboard_reuse_idle_seconds = DEFAULT_DASHBOARD_REUSE_IDLE_SECONDS;
    if (!parse_config_uint("DASHBOARD_REUSE_IDLE_SECONDS", reuse_idle_text, 0,
                           MAX_DASHBOARD_REUSE_IDLE_SECONDS,
                           &app->dashboard_reuse_idle_seconds))
        return FALSE;
    app->webkit_process_memory_limit_mb = DEFAULT_WEBKIT_PROCESS_MEMORY_LIMIT_MB;
    if (webkit_memory_limit_text && *webkit_memory_limit_text &&
        !parse_bounded_uint("WEBKIT_PROCESS_MEMORY_LIMIT_MB", webkit_memory_limit_text,
            MIN_WEBKIT_PROCESS_MEMORY_LIMIT_MB,
            MAX_WEBKIT_PROCESS_MEMORY_LIMIT_MB,
            &app->webkit_process_memory_limit_mb)) {
        g_printerr("configuration error: WEBKIT_PROCESS_MEMORY_LIMIT_MB must be between 512 and 3072 MiB\n");
        return FALSE;
    }

    if (app->fixture_mode) {
        app->fixture_delay_ms = 350;
        app->fixture_first_delay_ms = 350;
        const gchar *delay = g_getenv("FIXTURE_DELAY_MS");
        const gchar *first_delay = g_getenv("FIXTURE_FIRST_DELAY_MS");
        const gchar *readiness_errors = g_getenv("FIXTURE_READINESS_ERRORS");
        const gchar *view_recreate_failures =
            g_getenv("FIXTURE_VIEW_RECREATE_FAILURES");
        if (!parse_config_uint("FIXTURE_DELAY_MS", delay, 0, 30000,
                               &app->fixture_delay_ms))
            return FALSE;
        if (!parse_config_uint("FIXTURE_FIRST_DELAY_MS", first_delay, 0, 30000,
                               &app->fixture_first_delay_ms))
            return FALSE;
        if (!parse_config_uint("FIXTURE_READINESS_ERRORS", readiness_errors, 0, 10,
                               &app->fixture_readiness_error_count))
            return FALSE;
        if (!parse_config_uint("FIXTURE_VIEW_RECREATE_FAILURES", view_recreate_failures,
                               0, 10, &app->fixture_view_recreate_failure_count))
            return FALSE;
    }

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
    g_free(request->encoded_path);
    g_free(request->theme);
    g_free(request->language);
    g_free(request->last_readiness_state);
    g_free(request->capture_pixels);
    g_free(request);
}

static void
disconnect_message_handlers(RenderRequest *request)
{
    gulong *handlers[] = {
        &request->message_disconnected_handler,
        &request->message_finished_handler,
    };
    for (guint i = 0; i < G_N_ELEMENTS(handlers); ++i) {
        if (!*handlers[i])
            continue;
        gulong handler = *handlers[i];
        *handlers[i] = 0;
        if (g_signal_handler_is_connected(request->message, handler))
            g_signal_handler_disconnect(request->message, handler);
        request_unref(request);
    }
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
    disconnect_message_handlers(request);
    remove_source(&request->timeout_source);
    remove_source(&request->readiness_source);
    remove_source(&request->wait_source);
    remove_source(&request->tile_source);
    remove_source(&request->disconnect_source);
    g_cancellable_cancel(request->cancellable);

    if (image && status == 200) {
        gboolean has_frontend_overrides = request->language || request->theme ||
                                          request->has_dark_mode;
        app->dashboard_loaded = !has_frontend_overrides && same_origin(
            app, webkit_web_view_get_uri(app->view));
        app->dashboard_last_capture_at_us = app->dashboard_loaded ?
            g_get_monotonic_time() : 0;
        g_free(app->dashboard_requested_path);
        app->dashboard_requested_path = app->dashboard_loaded ?
            g_strdup(request->path) : NULL;
        gsize length = 0;
        gchar *bytes = (gchar *) g_bytes_unref_to_data(image, &length);
        gint64 elapsed_us = g_get_monotonic_time() - request->started_at_us;
        guint64 elapsed_ms = elapsed_us > 0 ? (guint64) elapsed_us / 1000u : 0;
        g_message("Screenshot complete: request_id=%llu route=/%s status=200 elapsed_ms=%llu"
                  " image_bytes=%zu", (unsigned long long) request->id,
                  request->path, (unsigned long long) elapsed_ms, length);
        soup_server_message_set_status(request->message, 200, NULL);
        const gchar *mime_type = output_format_mime(request->format);
        soup_server_message_set_response(request->message, mime_type, SOUP_MEMORY_TAKE,
                                         bytes, length);
        set_response_headers(request->message, mime_type, "no-store", length);
        if (request->auto_height_truncated)
            soup_message_headers_replace(
                soup_server_message_get_response_headers(request->message),
                "X-WPE-Screenshot-Truncated", "true");
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
    if (request->auto_height || request->tiled_auto_height)
        reset_view = TRUE;
    if (owns_active)
        app->active = NULL;
    if (owns_active)
        remove_source(&app->idle_release_source);
    if (reset_view)
        app_schedule_recreate_view(app);
    if (owns_active)
        app_schedule_idle_release(app);
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
    disconnect_message_handlers(request);
    remove_source(&request->timeout_source);
    remove_source(&request->readiness_source);
    remove_source(&request->wait_source);
    remove_source(&request->tile_source);
    remove_source(&request->disconnect_source);
    g_cancellable_cancel(request->cancellable);
    soup_server_message_unpause(request->message);
    app->active = NULL;
    remove_source(&app->idle_release_source);
    if (!request->auto_height && !request->tiled_auto_height &&
        app->dashboard_loaded &&
        !g_strcmp0(app->dashboard_requested_path, request->path) &&
        same_origin(app, webkit_web_view_get_uri(app->view))) {
        g_message("Screenshot client disconnected; keeping loaded dashboard: request_id=%llu route=/%s",
                  (unsigned long long) request->id, request->path);
    } else {
        app_schedule_recreate_view(app);
    }
    app_schedule_idle_release(app);
    request_unref(request); /* Release the app->active reference. */
}

static void
on_message_terminated(SoupServerMessage *message, gpointer user_data)
{
    (void) message;
    RenderRequest *request = request_ref(user_data);
    cancel_disconnected_request(request);
    request_unref(request); /* Release the temporary callback reference. */
}

static guint
add_request_timeout(guint interval, GSourceFunc callback, RenderRequest *request)
{
    return g_timeout_add_full(G_PRIORITY_DEFAULT, interval, callback,
                              request_ref(request), (GDestroyNotify) request_unref);
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
    if (socket_fd < 0) {
        request->disconnect_source = 0;
        cancel_disconnected_request(request);
        return G_SOURCE_REMOVE;
    }

    gchar byte;
    ssize_t received = recv(socket_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    /* A peer FIN closes only its sending direction. It may still be waiting
     * for this response, so let libsoup report a full disconnect separately. */
    if (received == 0) {
        request->disconnect_source = 0;
        return G_SOURCE_REMOVE;
    }
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        request->disconnect_source = 0;
        cancel_disconnected_request(request);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
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
dashboard_page_can_be_reused(App *app, const gchar *requested_path)
{
    if (!app->dashboard_loaded || !app->dashboard_reuse_idle_seconds ||
        app->dashboard_last_capture_at_us <= 0)
        return FALSE;

    gint64 idle_us = g_get_monotonic_time() - app->dashboard_last_capture_at_us;
    if (idle_us < 0 || idle_us >
        (gint64) app->dashboard_reuse_idle_seconds * G_USEC_PER_SEC)
        return FALSE;

    return !g_strcmp0(app->dashboard_requested_path, requested_path) &&
           same_origin(app, webkit_web_view_get_uri(app->view));
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

    /* WebKit may expose its initial empty document briefly during navigation. */
    if (!strcmp(uri_text, "about:blank"))
        return TRUE;

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

    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        view == app->view && app->active) {
        WebKitNavigationPolicyDecision *navigation =
            WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(navigation);
        WebKitURIRequest *request = action ?
            webkit_navigation_action_get_request(action) : NULL;
        const gchar *uri = request ? webkit_uri_request_get_uri(request) : NULL;
        if (uri && !is_same_origin_uri(app, uri)) {
            webkit_policy_decision_ignore(decision);
            app->dashboard_loaded = FALSE;
            app->dashboard_last_capture_at_us = 0;
            g_clear_pointer(&app->dashboard_requested_path, g_free);
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
    wpe_view_resized(wpe_view, DEFAULT_IMAGE_WIDTH, DEFAULT_IMAGE_HEIGHT);
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
    remove_source(&app->idle_release_source);
    if (app->recreate_view_source) {
        g_source_remove(app->recreate_view_source);
        app->recreate_view_source = 0;
    }
    app->renderer_ready = FALSE;
    app->renderer_idle_released = FALSE;
    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;
    g_clear_pointer(&app->dashboard_requested_path, g_free);
    app->wpe_view = NULL;
    if (app->view) {
        g_signal_handlers_disconnect_by_data(app->view, app);
        g_clear_object(&app->view);
    }
    if (app->fixture_mode && app->fixture_view_recreate_failure_count) {
        app->fixture_view_recreate_failure_count--;
        g_warning("Fixture injected a WPE view recreation failure");
        return;
    }
    app->view = create_view(app);
}

static gboolean
app_recreate_view_idle(gpointer user_data)
{
    App *app = user_data;
    app->recreate_view_source = 0;
    app_recreate_view(app);
    app_schedule_idle_release(app);
    return G_SOURCE_REMOVE;
}

static void
app_schedule_recreate_view(App *app)
{
    remove_source(&app->idle_release_source);
    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;
    g_clear_pointer(&app->dashboard_requested_path, g_free);
    app->renderer_ready = FALSE;
    if (!app->recreate_view_source)
        app->recreate_view_source = g_idle_add_full(
            G_PRIORITY_HIGH, app_recreate_view_idle, app, NULL);
}

static gboolean
app_ensure_view(App *app)
{
    if (app->recreate_view_source)
        app_recreate_view(app);
    if (app->renderer_ready && app->view && app->wpe_view) {
        app_schedule_idle_release(app);
        return TRUE;
    }

    if (app->renderer_idle_released)
        g_message("Renderer was released while idle; recreating it on demand");
    else
        g_warning("WPE renderer is unavailable; attempting to recreate the view");
    app_recreate_view(app);
    gboolean ready = app->renderer_ready && app->view && app->wpe_view;
    if (ready)
        app_schedule_idle_release(app);
    return ready;
}

static gboolean
app_release_idle_renderer(gpointer user_data)
{
    App *app = user_data;
    app->idle_release_source = 0;
    if (app->active || !app->view || !app->renderer_ready)
        return G_SOURCE_REMOVE;

    app->renderer_ready = FALSE;
    app->renderer_idle_released = TRUE;
    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;
    g_clear_pointer(&app->dashboard_requested_path, g_free);
    app->wpe_view = NULL;
    app->view_generation++;

    /* Drop the heavy web process after its page-reuse window expires. The
     * listener and WebKit context stay alive; a later capture recreates a view. */
    g_signal_handlers_disconnect_by_data(app->view, app);
    webkit_web_view_terminate_web_process(app->view);
    g_clear_object(&app->view);
    g_message("Renderer released after %u idle seconds; HTTP service remains available",
              app->dashboard_reuse_idle_seconds);
    return G_SOURCE_REMOVE;
}

static void
app_schedule_idle_release(App *app)
{
    if (app->active || app->idle_release_source || !app->renderer_ready || !app->view)
        return;

    if (app->dashboard_reuse_idle_seconds) {
        app->idle_release_source = g_timeout_add_seconds_full(
            G_PRIORITY_LOW, app->dashboard_reuse_idle_seconds,
            app_release_idle_renderer, app, NULL);
    } else {
        app->idle_release_source = g_idle_add_full(
            G_PRIORITY_LOW, app_release_idle_renderer, app, NULL);
    }
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
    gchar *base_uri = g_strdup_printf("%s/%s", app->ha_url,
                                      request->encoded_path);
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
    const gchar *fixture = app->fixture_mode && !app->fixture_ha_readiness &&
        !g_strcmp0(request->path, app->readiness_path) ?
        "if(!document.querySelector('[data-fixture-ready=true]'))return 'wait-fixture';" :
        "";
    const gchar *home_assistant_readiness =
        app->fixture_mode && !app->fixture_ha_readiness ? "" :
        "if(location.pathname.indexOf('/auth/authorize')===0)return 'auth';"
        "let panelReady=false;const ha=document.querySelector('home-assistant');"
        "if(ha){const main=ha.shadowRoot&&ha.shadowRoot.querySelector('home-assistant-main');"
        "const resolver=main&&main.shadowRoot&&main.shadowRoot.querySelector('partial-panel-resolver');"
        "if(!main||!resolver)return 'wait-panel';const panel=resolver.children[0];"
        "if(!panel)return 'wait-panel';panelReady=true;}"
        "if(ha&&!panelReady)return 'wait-panel';"
        "if(!panelReady&&(document.readyState!=='complete'||!document.body))return 'wait-document';"
        "if(!panelReady&&!document.body.textContent.trim()&&!document.body.children.length)return 'wait-content';";
    gchar *layout_ready = request->reused_page ? g_strdup_printf(
        "if(window.__wpeReadyRequest!==%llu){window.__wpeReadyRequest=%llu;"
        "window.__wpeReadyFrames=0;requestAnimationFrame(()=>requestAnimationFrame(()=>{"
        "window.__wpeReadyFrames=2;}));return 'wait-layout';}"
        "if(window.__wpeReadyFrames<2)return 'wait-layout';",
        (unsigned long long) request->id, (unsigned long long) request->id) :
        g_strdup("");
    GString *options_ready = g_string_new("");
    if (request->frontend_options_applied &&
        !request->frontend_options_verified) {
        g_string_append(options_ready,
            "const optionHa=document.querySelector('home-assistant');"
            "if(!optionHa||!optionHa.hass)return 'wait-options';");
        if (request->language) {
            gchar *language = js_quote(request->language);
            g_string_append_printf(options_ready,
                "if(optionHa.hass.language!==%s||document.documentElement.lang!==%s)"
                "return 'wait-language';", language, language);
            g_free(language);
        }
        if (request->theme || request->has_dark_mode) {
            g_string_append(options_ready,
                "if(!optionHa.hass.selectedTheme)return 'wait-theme';");
            if (request->theme) {
                gchar *theme = js_quote(request->theme);
                g_string_append_printf(options_ready,
                    "if(optionHa.hass.selectedTheme.theme!==%s)"
                    "return 'wait-theme';", theme);
                g_free(theme);
            }
            if (request->has_dark_mode)
                g_string_append_printf(options_ready,
                    "if(optionHa.hass.selectedTheme.dark!==%s)"
                    "return 'wait-theme';",
                    request->dark_mode ? "true" : "false");
        }
    }
    if (!expected_text[0]) {
        *script_out = g_strdup_printf(
            "(()=>{%s%s%s%sif(document.fonts&&document.fonts.status!=='loaded')return 'wait-fonts';"
            "return 'ready';})()",
            layout_ready, fixture, home_assistant_readiness, options_ready->str);
        g_string_free(options_ready, TRUE);
        g_free(layout_ready);
        return TRUE;
    }

    gchar *expected = js_quote(expected_text);
    *script_out = g_strdup_printf(
        "(()=>{%s%s%s%sif(document.fonts&&document.fonts.status!=='loaded')return 'wait-fonts';"
        "let text='';const walk=root=>{if(!root)return;text+=' '+(root.textContent||'');"
        "for(const e of root.querySelectorAll('*'))if(e.shadowRoot)walk(e.shadowRoot);};"
        "walk(document.body);return text.toLocaleLowerCase().includes(%s.toLocaleLowerCase())?"
        "'ready':'wait';})()",
        layout_ready, fixture, home_assistant_readiness, options_ready->str,
        expected);
    g_string_free(options_ready, TRUE);
    g_free(layout_ready);
    secure_free_string(expected);
    return TRUE;
}

static gboolean
start_snapshot(RenderRequest *request, WebKitSnapshotRegion region)
{
    if (!current_request(request))
        return FALSE;
    webkit_web_view_get_snapshot(request->app->view, region,
                                 WEBKIT_SNAPSHOT_OPTIONS_NONE,
                                 request->cancellable, on_snapshot_ready,
                                 request_ref(request));
    return TRUE;
}

static gboolean
capture_timer(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (!current_request(request))
        return G_SOURCE_REMOVE;
    request->wait_source = 0;
    if (request->auto_height) {
        g_message("Full-page capture starting: request_id=%llu route=/%s",
                  (unsigned long long) request->id, request->path);
        gchar zoom_text[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(zoom_text, sizeof(zoom_text), request->zoom);
        gchar *script = g_strdup_printf(
            "(()=>{const visit=root=>{for(const element of root.querySelectorAll('*')){"
            "const position=getComputedStyle(element).position;"
            "if(position==='fixed'||position==='sticky')"
            "element.style.setProperty('position','static','important');"
            "if(element.shadowRoot)visit(element.shadowRoot);}};visit(document);"
            "const scrollRoot=document.scrollingElement||document.documentElement;"
            "let innerScroll=false;const inspect=root=>{for(const element of root.querySelectorAll('*')){"
            "if(element!==scrollRoot&&element!==document.body&&element!==document.documentElement&&"
            "element.scrollHeight>element.clientHeight+2&&"
            "/auto|scroll/.test(getComputedStyle(element).overflowY))innerScroll=true;"
            "if(element.shadowRoot)inspect(element.shadowRoot);}};inspect(document);"
            "if(innerScroll)return 'inner-scroll';"
            "return Math.min(4001,Math.ceil(Math.max(document.documentElement.scrollHeight,"
            "document.body ? document.body.scrollHeight : 0)*%s));})()", zoom_text);
        webkit_web_view_evaluate_javascript(
            request->app->view, script, -1, NULL, NULL, request->cancellable,
            on_document_height_ready, request_ref(request));
        g_free(script);
    } else {
        request->capture_height = request->height;
        start_snapshot(request, WEBKIT_SNAPSHOT_REGION_VISIBLE);
    }
    return G_SOURCE_REMOVE;
}

static void
schedule_capture(RenderRequest *request)
{
    request->capture_scheduled = TRUE;
    request->wait_source = add_request_timeout(request->wait_ms ? request->wait_ms : 1,
                                               capture_timer, request);
}

static gchar *
frontend_options_script(RenderRequest *request)
{
    gchar *language = request->language ? js_quote(request->language) : NULL;
    gchar *theme = request->theme ? js_quote(request->theme) : NULL;
    GString *script = g_string_new(
        "(()=>{const ha=document.querySelector('home-assistant');"
        "if(!ha||!ha.hass)throw new Error('Home Assistant frontend is not ready');");
    if (language || theme || request->has_dark_mode) {
        g_string_append(script,
            "const conn=ha.hass&&ha.hass.connection;"
            "if(!conn||typeof conn.sendMessagePromise!=='function')"
            "throw new Error('Cannot safely apply temporary frontend options');"
            "const send=conn.sendMessagePromise;"
            "const descriptor=Object.getOwnPropertyDescriptor(conn,'sendMessagePromise');"
            "const saveStorage=storage=>{const values={};for(let i=0;i<storage.length;i++){"
            "const key=storage.key(i);values[key]=storage.getItem(key);}return values;};"
            "const restoreStorage=(storage,values)=>{storage.clear();"
            "for(const key of Object.keys(values))storage.setItem(key,values[key]);};"
            "const localState=saveStorage(window.localStorage);"
            "const sessionState=saveStorage(window.sessionStorage);"
            "try{conn.sendMessagePromise=function(message){"
            "if(message&&message.type==='frontend/set_user_data'&&"
            "(message.key==='theme'||message.key==='language'))"
            "return Promise.resolve();return send.apply(this,arguments);};"
            "if(conn.sendMessagePromise===send)"
            "throw new Error('Cannot safely apply temporary frontend options');");
        if (language)
            g_string_append_printf(script,
                "ha.dispatchEvent(new CustomEvent('hass-language-select',{detail:%s}));",
                language);
        if (theme || request->has_dark_mode) {
            g_string_append(script, "const detail={};");
            if (theme)
                g_string_append_printf(script, "detail.theme=%s;", theme);
            if (request->has_dark_mode)
                g_string_append_printf(script, "detail.dark=%s;",
                                       request->dark_mode ? "true" : "false");
            g_string_append(script,
                "ha.dispatchEvent(new CustomEvent('settheme',{detail}));");
        }
        g_string_append(script,
            "}finally{try{if(descriptor)Object.defineProperty(conn,'sendMessagePromise',descriptor);"
            "else delete conn.sendMessagePromise;}finally{try{restoreStorage(window.localStorage,localState);"
            "restoreStorage(window.sessionStorage,sessionState);}catch(error){"
            "throw new Error('Could not restore temporary frontend storage');}}}");
    }
    g_string_append(script, "return 'applied';})()");
    g_free(language);
    g_free(theme);
    return g_string_free(script, FALSE);
}

static void
on_frontend_options_ready(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    if (!current_request(request)) {
        g_clear_object(&value);
        g_clear_error(&error);
        request_unref(request);
        return;
    }
    if (error || !value) {
        g_clear_error(&error);
        g_clear_object(&value);
        request_finish(request, 502,
                       "Home Assistant screenshot options could not be applied.\n",
                       NULL, TRUE);
    } else {
        g_object_unref(value);
        request->frontend_options_pending = FALSE;
        start_readiness_checks(request);
    }
    request_unref(request);
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
        if (request->readiness_error_count++ == 0)
            g_warning("Dashboard readiness evaluation failed temporarily; retrying until the render deadline");
        g_clear_error(&error);
        g_clear_object(&value);
        request_unref(request);
        return;
    }
    request->readiness_error_count = 0;
    gchar *state = jsc_value_to_string(value);
    g_object_unref(value);
    if (g_strcmp0(request->last_readiness_state, state)) {
        g_message("Dashboard readiness: request_id=%llu route=/%s state=%s",
                  (unsigned long long) request->id, request->path,
                  state ? state : "unknown");
        g_free(request->last_readiness_state);
        request->last_readiness_state = g_strdup(state);
    }
    if (state && !strcmp(state, "auth")) {
        g_free(state);
        request_finish(request, 502, "Home Assistant authentication failed.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    gboolean ready = state && !strcmp(state, "ready");
    g_free(state);
    if (ready && !request->frontend_options_applied) {
        remove_source(&request->readiness_source);
        if (request->language || request->theme || request->has_dark_mode) {
            /* Keep request-scoped frontend settings from leaking into later
             * captures, including when this capture fails. */
            request->app->dashboard_loaded = FALSE;
            request->app->dashboard_last_capture_at_us = 0;
            request->frontend_options_applied = TRUE;
            request->frontend_options_pending = TRUE;
            gchar *script = frontend_options_script(request);
            webkit_web_view_evaluate_javascript(
                request->app->view, script, -1, NULL, NULL,
                request->cancellable, on_frontend_options_ready,
                request_ref(request));
            g_free(script);
        } else {
            request->frontend_options_applied = TRUE;
            request->frontend_options_verified = TRUE;
            schedule_capture(request);
        }
    } else if (ready && !request->frontend_options_verified) {
        remove_source(&request->readiness_source);
        request->frontend_options_verified = TRUE;
        schedule_capture(request);
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
    if (request->app->fixture_mode &&
        request->app->fixture_readiness_error_count) {
        request->app->fixture_readiness_error_count--;
        script = g_strdup("(() => {");
    } else {
        ready_script(request->app, request, &script);
    }
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
    if (request->readiness_source || request->frontend_options_pending ||
        request->capture_scheduled)
        return;

    request->readiness_source = add_request_timeout(200, readiness_tick, request);
    readiness_tick(request);
}

static void
on_document_height_ready(GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    if (!current_request(request)) {
        g_clear_object(&value);
        g_clear_error(&error);
        request_unref(request);
        return;
    }

    gchar *height_text = value ? jsc_value_to_string(value) : NULL;
    guint document_height = 0;
    if (!error && height_text && !strcmp(height_text, "inner-scroll")) {
        g_free(height_text);
        g_clear_object(&value);
        g_clear_error(&error);
        request_finish(request, 422,
                       "Full-page capture does not support vertically scrolling inner containers.\n",
                       NULL, TRUE);
        request_unref(request);
        return;
    }
    gboolean valid_height = !error && height_text &&
        parse_bounded_uint("document height", height_text, 0, G_MAXUINT,
                           &document_height);
    g_free(height_text);
    g_clear_object(&value);
    g_clear_error(&error);
    if (!valid_height) {
        request_finish(request, 503,
                       "WPE could not determine the dashboard height.\n",
                       NULL, TRUE);
        request_unref(request);
        return;
    }

    request->auto_height_truncated = document_height > MAX_AUTO_HEIGHT;
    request->capture_height = MIN(document_height ? document_height : 1u,
                                  MAX_AUTO_HEIGHT);
    g_message("Full-page height measured: request_id=%llu document_height=%u capture_height=%u",
              (unsigned long long) request->id, document_height,
              request->capture_height);
    gsize row_bytes = (gsize) request->width * 4u;
    if ((gsize) request->capture_height > MAX_RAW_IMAGE_BYTES / row_bytes) {
        request_finish(request, 413,
                       "Rendered image exceeds the 60 MiB raw image limit.\n",
                       NULL, FALSE);
    } else if (document_height > MAX_FULL_DOCUMENT_SNAPSHOT_HEIGHT + 2u) {
        request->capture_pixels_length = row_bytes * request->capture_height;
        request->capture_pixels = g_try_malloc0(
            request->capture_pixels_length);
        if (!request->capture_pixels) {
            request_finish(request, 503,
                           "WPE could not allocate the full-page image.\n",
                           NULL, FALSE);
        } else {
            request->tiled_auto_height = TRUE;
            request->capture_offset_y = 0;
            start_tiled_scroll(request);
        }
    } else {
        start_snapshot(request, WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT);
    }
    request_unref(request);
}

static void
start_tiled_scroll(RenderRequest *request)
{
    gchar zoom_text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(zoom_text, sizeof(zoom_text), request->zoom);
    gchar *script = g_strdup_printf(
        "(()=>{const root=document.scrollingElement||document.documentElement;"
        "const behavior=root.style.scrollBehavior;root.style.scrollBehavior='auto';"
        "window.scrollTo(0,%u/(%s));const offset=Math.round(window.scrollY*(%s));"
        "root.style.scrollBehavior=behavior;return offset;})()",
        request->capture_offset_y, zoom_text, zoom_text);
    g_message("Full-page tile scroll requested: request_id=%llu offset_y=%u",
              (unsigned long long) request->id, request->capture_offset_y);
    webkit_web_view_evaluate_javascript(
        request->app->view, script, -1, NULL, NULL, request->cancellable,
        on_tiled_scroll_ready, request_ref(request));
    g_free(script);
}

static gboolean
capture_tiled_viewport(gpointer user_data)
{
    RenderRequest *request = user_data;
    request->tile_source = 0;
    if (current_request(request))
        start_snapshot(request, WEBKIT_SNAPSHOT_REGION_VISIBLE);
    return G_SOURCE_REMOVE;
}

static void
on_tiled_scroll_ready(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    if (!current_request(request)) {
        g_clear_object(&value);
        g_clear_error(&error);
        request_unref(request);
        return;
    }

    gchar *offset_text = value ? jsc_value_to_string(value) : NULL;
    guint offset = 0;
    gboolean valid_offset = !error && offset_text &&
        parse_bounded_uint("scroll offset", offset_text, 0,
                           MAX_AUTO_HEIGHT, &offset);
    g_free(offset_text);
    g_clear_object(&value);
    g_clear_error(&error);
    if (!valid_offset) {
        request_finish(request, 503,
                       "WPE could not scroll the dashboard for capture.\n",
                       NULL, TRUE);
    } else if (request->capture_offset_y > 0 &&
               offset <= request->tile_scroll_y) {
        request_finish(request, 422,
                       "Full-page capture requires a page that scrolls vertically.\n",
                       NULL, TRUE);
    } else {
        request->tile_scroll_y = offset;
        g_message("Full-page tile scrolled: request_id=%llu offset_y=%u scroll_y=%u",
                  (unsigned long long) request->id,
                  request->capture_offset_y, offset);
        request->tile_source = add_request_timeout(
            50, capture_tiled_viewport, request);
    }
    request_unref(request);
}

static void
on_tiled_scroll_restored(GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
    RenderRequest *request = user_data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(source), result, &error);
    if (!current_request(request)) {
        g_clear_object(&value);
        g_clear_error(&error);
        request_unref(request);
        return;
    }

    gchar *offset_text = value ? jsc_value_to_string(value) : NULL;
    guint offset = 1;
    gboolean restored = !error && offset_text &&
        parse_bounded_uint("restored scroll offset", offset_text, 0,
                           MAX_AUTO_HEIGHT, &offset) && offset == 0;
    g_free(offset_text);
    g_clear_object(&value);
    g_clear_error(&error);

    if (!restored)
        g_warning("Could not reset full-page scroll before encoding: request_id=%llu",
                  (unsigned long long) request->id);
    guint8 *pixels = request->capture_pixels;
    request->capture_pixels = NULL;
    request->capture_pixels_length = 0;
    if (!pixels) {
        request_finish(request, 503,
                       "WPE could not encode the full-page image.\n", NULL, TRUE);
    } else {
        encode_snapshot_async(request, pixels, request->width * 4u,
                              request->width, request->capture_height, NULL,
                              pixels, TRUE);
    }
    request_unref(request);
}

static void
bgra_to_rgba(const guint8 *source, guint8 *destination)
{
    guint8 alpha = source[3];
    guint red = source[2];
    guint green = source[1];
    guint blue = source[0];
    if (alpha && alpha < 255) {
        red = MIN(255u, (red * 255u + alpha / 2u) / alpha);
        green = MIN(255u, (green * 255u + alpha / 2u) / alpha);
        blue = MIN(255u, (blue * 255u + alpha / 2u) / alpha);
    } else if (!alpha) {
        red = green = blue = 0;
    }
    destination[0] = (guint8) red;
    destination[1] = (guint8) green;
    destination[2] = (guint8) blue;
    destination[3] = alpha;
}

static void
bgra_to_rgb_white(const guint8 *source, guint8 *destination)
{
    guint alpha_complement = 255u - source[3];
    destination[0] = (guint8) MIN(255u, (guint) source[2] + alpha_complement);
    destination[1] = (guint8) MIN(255u, (guint) source[1] + alpha_complement);
    destination[2] = (guint8) MIN(255u, (guint) source[0] + alpha_complement);
}

static gboolean
image_is_opaque(const guint8 *pixels, guint stride, guint width, guint height)
{
    for (guint y = 0; y < height; ++y) {
        const guint8 *row = pixels + (gsize) y * stride;
        for (guint x = 0; x < width; ++x) {
            if (row[(gsize) x * 4u + 3u] != 255)
                return FALSE;
        }
    }
    return TRUE;
}

typedef struct {
    guint8 *data;
    gsize length;
    gsize capacity;
    gboolean too_large;
    gboolean allocation_failed;
} PngWriter;

static void
png_write_bounded(png_structp png, png_bytep data, png_size_t length)
{
    PngWriter *writer = png_get_io_ptr(png);
    gsize chunk_length = (gsize) length;
    if (chunk_length > MAX_OUTPUT_IMAGE_BYTES - writer->length) {
        writer->too_large = TRUE;
        png_error(png, "PNG output exceeds the size limit");
        return;
    }

    gsize required = writer->length + chunk_length;
    if (required > writer->capacity) {
        gsize capacity = writer->capacity ? writer->capacity : 65536u;
        while (capacity < required) {
            capacity = MIN(capacity * 2u, (gsize) MAX_OUTPUT_IMAGE_BYTES);
            if (capacity < required && capacity == MAX_OUTPUT_IMAGE_BYTES)
                break;
        }
        guint8 *resized = g_try_realloc(writer->data, capacity);
        if (!resized) {
            writer->allocation_failed = TRUE;
            png_error(png, "could not allocate PNG output buffer");
            return;
        }
        writer->data = resized;
        writer->capacity = capacity;
    }
    memcpy(writer->data + writer->length, data, chunk_length);
    writer->length = required;
}

static void
png_flush_bounded(png_structp png)
{
    (void) png;
}

static GBytes *
encode_png(const guint8 *source, guint source_stride, guint width, guint height,
          gboolean opaque, gboolean *too_large)
{
    PngWriter *writer = g_try_new0(PngWriter, 1);
    gsize channels = opaque ? 3u : 4u;
    guint8 *row = g_try_malloc((gsize) width * channels);
    if (!writer || !row) {
        g_free(writer);
        g_free(row);
        return NULL;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) {
        if (png)
            png_destroy_write_struct(&png, NULL);
        g_free(writer);
        g_free(row);
        return NULL;
    }
    if (setjmp(png_jmpbuf(png))) {
        *too_large = writer->too_large;
        png_destroy_write_struct(&png, &info);
        g_free(writer->data);
        g_free(writer);
        g_free(row);
        return NULL;
    }

    png_set_write_fn(png, writer, png_write_bounded, png_flush_bounded);
    png_set_IHDR(png, info, width, height, 8,
                 opaque ? PNG_COLOR_TYPE_RGB : PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 1);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    png_write_info(png, info);
    for (guint y = 0; y < height; ++y) {
        const guint8 *source_row = source + (gsize) y * source_stride;
        if (opaque) {
            for (guint x = 0; x < width; ++x)
                bgra_to_rgb_white(source_row + (gsize) x * 4u,
                                  row + (gsize) x * 3u);
        } else {
            for (guint x = 0; x < width; ++x)
                bgra_to_rgba(source_row + (gsize) x * 4u,
                             row + (gsize) x * 4u);
        }
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    g_free(row);
    if (!writer->length) {
        g_free(writer->data);
        g_free(writer);
        return NULL;
    }
    guint8 *encoded = writer->data;
    gsize encoded_length = writer->length;
    g_free(writer);
    return g_bytes_new_take(encoded, encoded_length);
}

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} JpegErrorManager;

typedef struct {
    struct jpeg_destination_mgr base;
    guint8 *buffer;
    size_t capacity;
    size_t output_length;
    gboolean overflow;
} JpegBoundedDestination;

typedef struct {
    struct jpeg_compress_struct compressor;
    JpegErrorManager error;
    JpegBoundedDestination destination;
    guint8 *row;
    gboolean created;
} JpegEncoder;

static void
jpeg_error_exit(j_common_ptr compressor)
{
    JpegErrorManager *error = (JpegErrorManager *) compressor->err;
    longjmp(error->jump, 1);
}

static void
jpeg_destination_init(j_compress_ptr compressor)
{
    JpegBoundedDestination *destination =
        (JpegBoundedDestination *) compressor->dest;
    destination->base.next_output_byte = destination->buffer;
    destination->base.free_in_buffer = destination->capacity;
}

static boolean
jpeg_destination_full(j_compress_ptr compressor)
{
    JpegBoundedDestination *destination =
        (JpegBoundedDestination *) compressor->dest;
    if (destination->capacity >= MAX_OUTPUT_IMAGE_BYTES) {
        destination->overflow = TRUE;
        ERREXIT(compressor, JERR_BUFFER_SIZE);
        return FALSE;
    }
    size_t written = destination->capacity -
                     destination->base.free_in_buffer;
    size_t capacity = MIN(destination->capacity * 2u,
                          (size_t) MAX_OUTPUT_IMAGE_BYTES);
    guint8 *buffer = g_try_realloc(destination->buffer, capacity);
    if (!buffer)
        ERREXIT(compressor, JERR_OUT_OF_MEMORY);
    destination->buffer = buffer;
    destination->capacity = capacity;
    destination->base.next_output_byte = buffer + written;
    destination->base.free_in_buffer = capacity - written;
    return TRUE;
}

static void
jpeg_destination_finish(j_compress_ptr compressor)
{
    JpegBoundedDestination *destination =
        (JpegBoundedDestination *) compressor->dest;
    destination->output_length = destination->capacity -
                                 destination->base.free_in_buffer;
}

static GBytes *
encode_jpeg(const guint8 *source, guint source_stride, guint width, guint height,
            gboolean *too_large)
{
    JpegEncoder *encoder = g_try_new0(JpegEncoder, 1);
    if (!encoder)
        return NULL;
    encoder->destination.capacity = 64u * 1024u;
    encoder->destination.buffer = g_try_malloc(encoder->destination.capacity);
    encoder->row = g_try_malloc((gsize) width * 3u);
    if (!encoder->destination.buffer || !encoder->row) {
        g_free(encoder->destination.buffer);
        g_free(encoder->row);
        g_free(encoder);
        return NULL;
    }

    encoder->compressor.err = jpeg_std_error(&encoder->error.base);
    encoder->error.base.error_exit = jpeg_error_exit;
    if (setjmp(encoder->error.jump)) {
        *too_large = encoder->destination.overflow;
        if (encoder->created)
            jpeg_destroy_compress(&encoder->compressor);
        g_free(encoder->destination.buffer);
        g_free(encoder->row);
        g_free(encoder);
        return NULL;
    }

    jpeg_create_compress(&encoder->compressor);
    encoder->created = TRUE;
    encoder->destination.base.init_destination = jpeg_destination_init;
    encoder->destination.base.empty_output_buffer = jpeg_destination_full;
    encoder->destination.base.term_destination = jpeg_destination_finish;
    encoder->compressor.dest = &encoder->destination.base;
    encoder->compressor.image_width = width;
    encoder->compressor.image_height = height;
    encoder->compressor.input_components = 3;
    encoder->compressor.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder->compressor);
    jpeg_set_quality(&encoder->compressor, 85, TRUE);
    jpeg_start_compress(&encoder->compressor, TRUE);

    while (encoder->compressor.next_scanline < encoder->compressor.image_height) {
        guint y = encoder->compressor.next_scanline;
        const guint8 *src = source + (gsize) y * source_stride;
        for (guint x = 0; x < width; ++x)
            bgra_to_rgb_white(src + (gsize) x * 4u,
                              encoder->row + (gsize) x * 3u);
        JSAMPROW row = encoder->row;
        if (jpeg_write_scanlines(&encoder->compressor, &row, 1) != 1)
            ERREXIT(&encoder->compressor, JERR_CANT_SUSPEND);
    }
    jpeg_finish_compress(&encoder->compressor);
    size_t output_length = encoder->destination.output_length;
    jpeg_destroy_compress(&encoder->compressor);
    encoder->created = FALSE;
    g_free(encoder->row);
    if (!output_length) {
        g_free(encoder->destination.buffer);
        g_free(encoder);
        return NULL;
    }
    guint8 *encoded = g_try_realloc(encoder->destination.buffer, output_length);
    if (encoded)
        encoder->destination.buffer = encoded;
    encoded = encoder->destination.buffer;
    g_free(encoder);
    return g_bytes_new_take(encoded, output_length);
}

typedef struct {
    guint8 *buffer;
    size_t length;
    size_t capacity;
    gboolean overflow;
} WebPBoundedOutput;

static int
webp_bounded_writer(const uint8_t *data, size_t data_size,
                    const WebPPicture *picture)
{
    WebPBoundedOutput *output = picture->custom_ptr;
    if (data_size > MAX_OUTPUT_IMAGE_BYTES - output->length) {
        output->overflow = TRUE;
        return 0;
    }
    size_t required = output->length + data_size;
    if (required > output->capacity) {
        size_t capacity = MAX(output->capacity, (size_t) 64u * 1024u);
        while (capacity < required && capacity < MAX_OUTPUT_IMAGE_BYTES)
            capacity = MIN(capacity * 2u, (size_t) MAX_OUTPUT_IMAGE_BYTES);
        guint8 *buffer = g_try_realloc(output->buffer, capacity);
        if (!buffer)
            return 0;
        output->buffer = buffer;
        output->capacity = capacity;
    }
    memcpy(output->buffer + output->length, data, data_size);
    output->length += data_size;
    return 1;
}

static GBytes *
encode_webp(const guint8 *source, guint source_stride, guint width, guint height,
            gboolean *too_large)
{
    WebPBoundedOutput output = {
        .buffer = g_try_malloc(64u * 1024u),
        .capacity = 64u * 1024u,
    };
    if (!output.buffer) {
        g_free(output.buffer);
        return NULL;
    }

    WebPConfig config;
    WebPPicture picture;
    gboolean picture_initialized = WebPPictureInit(&picture);
    gboolean encoded = FALSE;
    if (picture_initialized) {
        if (WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, 85.0f)) {
            picture.width = (int) width;
            picture.height = (int) height;
            picture.use_argb = 1;
            picture.writer = webp_bounded_writer;
            picture.custom_ptr = &output;
            if (WebPPictureAlloc(&picture)) {
                for (guint y = 0; y < height; ++y) {
                    const guint8 *src = source + (gsize) y * source_stride;
                    uint32_t *dst = picture.argb +
                        (size_t) y * (size_t) picture.argb_stride;
                    for (guint x = 0; x < width; ++x) {
                        guint8 rgba[4];
                        bgra_to_rgba(src + (gsize) x * 4u, rgba);
                        dst[x] = ((uint32_t) rgba[3] << 24) |
                                 ((uint32_t) rgba[0] << 16) |
                                 ((uint32_t) rgba[1] << 8) |
                                 (uint32_t) rgba[2];
                    }
                }
                encoded = WebPEncode(&config, &picture);
            }
        }
        WebPPictureFree(&picture);
    }
    if (!encoded || !output.length) {
        *too_large = output.overflow;
        g_free(output.buffer);
        return NULL;
    }
    guint8 *shrunk = g_try_realloc(output.buffer, output.length);
    if (shrunk)
        output.buffer = shrunk;
    return g_bytes_new_take(output.buffer, output.length);
}

static void
write_le16(guint8 *destination, guint16 value)
{
    destination[0] = (guint8) (value & 0xffu);
    destination[1] = (guint8) (value >> 8);
}

static void
write_le32(guint8 *destination, guint32 value)
{
    for (guint i = 0; i < 4; ++i)
        destination[i] = (guint8) (value >> (i * 8u));
}

static GBytes *
encode_bmp(const guint8 *source, guint source_stride, guint width, guint height,
          gboolean *too_large)
{
    gsize row_length = ((gsize) width * 3u + 3u) & ~(gsize) 3u;
    if (height > (MAX_OUTPUT_IMAGE_BYTES - 54u) / row_length) {
        *too_large = TRUE;
        return NULL;
    }
    gsize output_length = 54u + row_length * height;
    guint8 *bmp = g_try_malloc0(output_length);
    if (!bmp)
        return NULL;
    bmp[0] = 'B';
    bmp[1] = 'M';
    write_le32(bmp + 2, (guint32) output_length);
    write_le32(bmp + 10, 54u);
    write_le32(bmp + 14, 40u);
    write_le32(bmp + 18, width);
    write_le32(bmp + 22, height);
    write_le16(bmp + 26, 1u);
    write_le16(bmp + 28, 24u);
    write_le32(bmp + 34, (guint32) (row_length * height));
    write_le32(bmp + 38, 2835u);
    write_le32(bmp + 42, 2835u);
    for (guint y = 0; y < height; ++y) {
        const guint8 *src = source + (gsize) (height - 1u - y) * source_stride;
        guint8 *dst = bmp + 54u + (gsize) y * row_length;
        for (guint x = 0; x < width; ++x) {
            guint8 rgb[3];
            bgra_to_rgb_white(src + (gsize) x * 4u, rgb);
            dst[(gsize) x * 3u + 0u] = rgb[2];
            dst[(gsize) x * 3u + 1u] = rgb[1];
            dst[(gsize) x * 3u + 2u] = rgb[0];
        }
    }
    return g_bytes_new_take(bmp, output_length);
}

static const gchar *
output_format_mime(OutputFormat format)
{
    switch (format) {
    case OUTPUT_FORMAT_JPEG: return "image/jpeg";
    case OUTPUT_FORMAT_WEBP: return "image/webp";
    case OUTPUT_FORMAT_BMP: return "image/bmp";
    case OUTPUT_FORMAT_PNG:
    default: return "image/png";
    }
}

static GBytes *
encode_snapshot(RenderRequest *request, const guint8 *pixels, guint stride,
                guint width, guint height, gboolean *too_large)
{
    switch (request->format) {
    case OUTPUT_FORMAT_JPEG:
        return encode_jpeg(pixels, stride, width, height, too_large);
    case OUTPUT_FORMAT_WEBP:
        return encode_webp(pixels, stride, width, height, too_large);
    case OUTPUT_FORMAT_BMP:
        return encode_bmp(pixels, stride, width, height, too_large);
    case OUTPUT_FORMAT_PNG:
        return encode_png(pixels, stride, width, height,
                          image_is_opaque(pixels, stride, width, height),
                          too_large);
    default:
        return encode_png(pixels, stride, width, height,
                          image_is_opaque(pixels, stride, width, height),
                          too_large);
    }
}

typedef struct {
    RenderRequest *request;
    const guint8 *pixels;
    GBytes *source_bytes;
    guint8 *owned_pixels;
    guint stride;
    guint width;
    guint height;
    gboolean reset_view;
} EncodeTaskData;

typedef struct {
    GBytes *image;
    gboolean too_large;
    gboolean reset_view;
} EncodeTaskResult;

static void
encode_task_data_free(gpointer user_data)
{
    EncodeTaskData *data = user_data;
    g_clear_pointer(&data->source_bytes, g_bytes_unref);
    g_free(data->owned_pixels);
    request_unref(data->request);
    g_free(data);
}

static void
encode_task_result_free(gpointer user_data)
{
    EncodeTaskResult *result = user_data;
    g_clear_pointer(&result->image, g_bytes_unref);
    g_free(result);
}

static void
encode_snapshot_worker(GTask *task, gpointer source_object, gpointer task_data,
                       GCancellable *cancellable)
{
    (void) source_object;
    EncodeTaskData *data = task_data;
    EncodeTaskResult *result = g_new0(EncodeTaskResult, 1);
    result->reset_view = data->reset_view;
    if (!g_cancellable_is_cancelled(cancellable))
        result->image = encode_snapshot(data->request, data->pixels,
                                        data->stride, data->width,
                                        data->height, &result->too_large);
    g_task_return_pointer(task, result, encode_task_result_free);
}

static void
on_encoded_snapshot_ready(GObject *source, GAsyncResult *async_result,
                          gpointer user_data)
{
    (void) source;
    RenderRequest *request = user_data;
    GError *error = NULL;
    EncodeTaskResult *result = g_task_propagate_pointer(
        G_TASK(async_result), &error);
    if (!current_request(request)) {
        encode_task_result_free(result);
        g_clear_error(&error);
        request_unref(request);
        return;
    }
    if (error || !result || !result->image) {
        gboolean too_large = result && result->too_large;
        gboolean reset_view = result && result->reset_view;
        request_finish(request, too_large ? 413 : 503,
                       too_large ? "Rendered image exceeds the 20 MiB image limit.\n" :
                                   "WPE could not encode the requested image format.\n",
                       NULL, reset_view);
    } else {
        GBytes *image = result->image;
        result->image = NULL;
        request_finish(request, 200, NULL, image, result->reset_view);
    }
    encode_task_result_free(result);
    g_clear_error(&error);
    request_unref(request);
}

static void
encode_snapshot_async(RenderRequest *request, const guint8 *pixels,
                      guint stride, guint width, guint height,
                      GBytes *source_bytes, guint8 *owned_pixels,
                      gboolean reset_view)
{
    EncodeTaskData *data = g_new0(EncodeTaskData, 1);
    data->request = request_ref(request);
    data->pixels = pixels;
    data->source_bytes = source_bytes;
    data->owned_pixels = owned_pixels;
    data->stride = stride;
    data->width = width;
    data->height = height;
    data->reset_view = reset_view;

    g_message("Screenshot encoding started: request_id=%llu format=%s dimensions=%ux%u",
              (unsigned long long) request->id,
              output_format_mime(request->format), width, height);

    GTask *task = g_task_new(NULL, request->cancellable,
                             on_encoded_snapshot_ready, request_ref(request));
    g_task_set_task_data(task, data, encode_task_data_free);
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, encode_snapshot_worker);
    g_object_unref(task);
}

static void
restore_tiled_scroll(RenderRequest *request)
{
    gchar zoom_text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(zoom_text, sizeof(zoom_text), request->zoom);
    gchar *restore_script = g_strdup_printf(
        "(()=>{const root=document.scrollingElement||document.documentElement;"
        "const behavior=root.style.scrollBehavior;root.style.scrollBehavior='auto';"
        "window.scrollTo(0,0);const offset=Math.round(window.scrollY*(%s));"
        "root.style.scrollBehavior=behavior;return offset;})()", zoom_text);
    webkit_web_view_evaluate_javascript(
        request->app->view, restore_script, -1, NULL, NULL,
        request->cancellable, on_tiled_scroll_restored, request_ref(request));
    g_free(restore_script);
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

    gint source_width = webkit_image_get_width(image);
    gint source_height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    /* The returned GBytes is owned by WebKitImage; keep image alive below. */
    GBytes *source_bytes = webkit_image_as_bytes(image);
    gsize source_length = 0;
    if (!source_bytes || source_width <= 0 || source_height <= 0 ||
        (guint) source_width < request->width) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }

    const guint8 *source_pixels = g_bytes_get_data(source_bytes, &source_length);
    const gsize source_row_bytes = (gsize) source_width * 4u;
    const gsize stride_bytes = (gsize) stride;
    if (stride_bytes < source_row_bytes) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    if (stride_bytes > (gsize) MAX_RAW_IMAGE_BYTES / (gsize) source_height) {
        g_object_unref(image);
        request_finish(request, 413,
                       "Rendered image exceeds the 60 MiB raw image limit.\n",
                       NULL, TRUE);
        request_unref(request);
        return;
    }

    const gsize frame_bytes = stride_bytes * (gsize) source_height;
    if (request->tiled_auto_height) {
        if (request->tile_scroll_y > request->capture_offset_y) {
            g_object_unref(image);
            request_finish(request, 503,
                           "WPE scroll position skipped rows during full-page capture.\n",
                           NULL, TRUE);
            request_unref(request);
            return;
        }
        guint source_y = request->capture_offset_y - request->tile_scroll_y;
        guint remaining = request->capture_height - request->capture_offset_y;
        if (frame_bytes > source_length ||
            (source_y >= (guint) source_height && !request->capture_offset_y)) {
            g_object_unref(image);
            request_finish(request, 503,
                           "WPE returned an unexpected full-page tile.\n",
                           NULL, TRUE);
            request_unref(request);
            return;
        }

        if (source_y >= (guint) source_height) {
            /* Height rounding can request one unreachable final pixel. Keep
             * the complete rows already captured and finish at that boundary. */
            request->capture_height = request->capture_offset_y;
            g_message("Full-page tile clamped to reachable height: request_id=%llu capture_height=%u",
                      (unsigned long long) request->id,
                      request->capture_height);
            g_object_unref(image);
            restore_tiled_scroll(request);
            request_unref(request);
            return;
        }

        guint rows = MIN(remaining, (guint) source_height - source_y);
        if (!rows) {
            g_object_unref(image);
            request_finish(request, 503,
                           "WPE could not reach the next full-page section.\n",
                           NULL, TRUE);
            request_unref(request);
            return;
        }
        for (guint row = 0; row < rows; ++row) {
            const guint8 *source_row = source_pixels +
                (gsize) (source_y + row) * stride_bytes;
            guint8 *destination_row = request->capture_pixels +
                (gsize) (request->capture_offset_y + row) * request->width * 4u;
            memcpy(destination_row, source_row, (gsize) request->width * 4u);
        }
        request->capture_offset_y += rows;
        g_message("Full-page tile captured: request_id=%llu source_y=%u rows=%u next_offset_y=%u",
                  (unsigned long long) request->id, source_y, rows,
                  request->capture_offset_y);
        g_object_unref(image);

        if (request->capture_offset_y < request->capture_height) {
            start_tiled_scroll(request);
        } else {
            restore_tiled_scroll(request);
        }
        request_unref(request);
        return;
    }

    guint output_height = request->auto_height ?
        MIN(request->capture_height, (guint) source_height) : request->height;
    if (frame_bytes > source_length || !output_height ||
        (gsize) request->width * output_height * 4u > MAX_RAW_IMAGE_BYTES) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    if (!request->auto_height && (guint) source_height != request->height) {
        g_object_unref(image);
        request_finish(request, 503, "WPE returned an unexpected frame size.\n", NULL, TRUE);
        request_unref(request);
        return;
    }
    g_bytes_ref(source_bytes);
    g_object_unref(image);
    encode_snapshot_async(request, source_pixels, stride, request->width,
                          output_height, source_bytes, NULL,
                          request->auto_height);
    request_unref(request);
}

static gboolean
render_deadline(gpointer user_data)
{
    RenderRequest *request = user_data;
    if (current_request(request)) {
        request->timeout_source = 0;
        request_finish(request, 504, "Dashboard render deadline exceeded.\n", NULL, TRUE);
    } else {
        request->timeout_source = 0;
        if (!request->finished && request->app->active == request)
            request_finish(request, 503,
                           "Screenshot request became stale before capture completed.\n",
                           NULL, TRUE);
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
        g_message("Dashboard load finished: request_id=%llu route=/%s uri=%s",
                  (unsigned long long) request->id, request->path,
                  webkit_web_view_get_uri(view) ? webkit_web_view_get_uri(view) : "(none)");
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
    if (uri && !is_same_origin_uri(app, uri)) {
        app->dashboard_loaded = FALSE;
        app->dashboard_last_capture_at_us = 0;
        g_clear_pointer(&app->dashboard_requested_path, g_free);
        RenderRequest *request = app->active;
        request_finish(request, 502,
                       "Home Assistant redirected outside the configured origin.\n",
                       NULL, TRUE);
        webkit_web_view_stop_loading(view);
    }
}

static gboolean
on_load_failed(WebKitWebView *view, WebKitLoadEvent event, const gchar *uri,
               GError *error, gpointer user_data)
{
    (void) event;
    (void) uri;
    (void) error;
    App *app = user_data;
    if (view == app->view && app->active) {
        app->dashboard_loaded = FALSE;
        app->dashboard_last_capture_at_us = 0;
        g_clear_pointer(&app->dashboard_requested_path, g_free);
        request_finish(app->active, 502, "Home Assistant page load failed.\n", NULL, TRUE);
    }
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
        app_schedule_recreate_view(app);
}

static gboolean
query_text_has_non_whitespace(const gchar *value)
{
    if (!value || !*value)
        return FALSE;

    for (const gchar *cursor = value; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (!g_unichar_isspace(g_utf8_get_char(cursor)))
            return TRUE;
    }
    return FALSE;
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
    gchar *form_value = g_strdup(value);
    for (gchar *cursor = form_value; *cursor; ++cursor) {
        if (*cursor == '+')
            *cursor = ' ';
    }
    *decoded = g_uri_unescape_string(form_value, NULL);
    g_free(form_value);
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
decode_path_segment(const gchar *value, gchar **decoded)
{
    for (const gchar *p = value; *p; ++p) {
        if (*p == '%') {
            if (!p[1] || !p[2] || !g_ascii_isxdigit(p[1]) ||
                !g_ascii_isxdigit(p[2]) ||
                (g_ascii_xdigit_value(p[1]) == 0 &&
                 g_ascii_xdigit_value(p[2]) == 0))
                return FALSE;
            p += 2;
        }
    }

    *decoded = g_uri_unescape_string(value, NULL);
    if (!*decoded || !g_utf8_validate(*decoded, -1, NULL)) {
        g_clear_pointer(decoded, g_free);
        return FALSE;
    }
    for (const gchar *cursor = *decoded; *cursor;
         cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (character < 0x20 || character == 0x7f) {
            g_clear_pointer(decoded, g_free);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
normalize_dashboard_uri_path(const gchar *uri_path, gchar **normalized_path,
                             gchar **encoded_path)
{
    *normalized_path = NULL;
    *encoded_path = NULL;
    const gsize max_encoded_length = 3u * MAX_DASHBOARD_PATH_BYTES + 1u;
    if (!uri_path || uri_path[0] != '/' ||
        strnlen(uri_path, max_encoded_length + 1u) > max_encoded_length ||
        strstr(uri_path, "//"))
        return FALSE;

    GString *decoded = g_string_sized_new(MAX_DASHBOARD_PATH_BYTES);
    GString *encoded = g_string_sized_new(MAX_DASHBOARD_PATH_BYTES);
    const gchar *segment_start = uri_path + 1;
    gboolean valid = TRUE;
    while (valid && *segment_start) {
        const gchar *segment_end = strchr(segment_start, '/');
        if (!segment_end)
            segment_end = segment_start + strlen(segment_start);
        gchar *raw_segment = g_strndup(segment_start,
                                       (gsize) (segment_end - segment_start));
        gchar *decoded_segment = NULL;
        if (!decode_path_segment(raw_segment, &decoded_segment) ||
            strchr(decoded_segment ? decoded_segment : "", '/')) {
            valid = FALSE;
        } else if (strcmp(decoded_segment, ".")) {
            gchar *escaped_segment = g_uri_escape_string(decoded_segment, NULL,
                                                          FALSE);
            if (!escaped_segment) {
                valid = FALSE;
            } else {
                if (decoded->len) {
                    g_string_append_c(decoded, '/');
                    g_string_append_c(encoded, '/');
                }
                g_string_append(decoded, decoded_segment);
                g_string_append(encoded, escaped_segment);
                g_free(escaped_segment);
            }
        }
        g_free(decoded_segment);
        g_free(raw_segment);

        if (!valid || !*segment_end)
            break;
        segment_start = segment_end + 1;
    }

    if (valid && decoded->len && valid_dashboard_path(decoded->str) &&
        encoded->len <= max_encoded_length) {
        *normalized_path = g_string_free(decoded, FALSE);
        *encoded_path = g_string_free(encoded, FALSE);
        return TRUE;
    }

    g_string_free(decoded, TRUE);
    g_string_free(encoded, TRUE);
    return FALSE;
}

static gboolean
decimal_query_value(const gchar *value)
{
    if (!value || !*value)
        return FALSE;
    for (const gchar *p = value; *p; ++p) {
        if (!g_ascii_isdigit(*p))
            return FALSE;
    }
    return TRUE;
}

static gboolean
parse_capture_query(GUri *uri, CaptureOptions *options, guint *status,
                    const gchar **error_text)
{
    options->width = DEFAULT_IMAGE_WIDTH;
    options->height = DEFAULT_IMAGE_HEIGHT;
    options->wait_ms = DEFAULT_WAIT_MS;
    options->zoom = 1.0;
    options->format = OUTPUT_FORMAT_PNG;
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
        GList *keys = g_list_sort(g_hash_table_get_keys(seen),
                                  (GCompareFunc) g_strcmp0);
        for (GList *key_node = keys; valid && key_node;
             key_node = key_node->next) {
            const gchar *key = key_node->data;
            const gchar *value = g_hash_table_lookup(seen, key);
            if (!strcmp(key, "viewport")) {
                const gchar *separator = strchr(value, 'x');
                if (!separator || separator == value || !separator[1] ||
                    strchr(separator + 1, 'x')) {
                    *status = 400;
                    *error_text = "viewport must be WIDTHxHEIGHT or WIDTHxauto.\n";
                    valid = FALSE;
                    break;
                }
                gchar *width_text = g_strndup(value, (gsize) (separator - value));
                const gchar *height_text = separator + 1;
                guint parsed_width = 0;
                if (!decimal_query_value(width_text)) {
                    *status = 400;
                    *error_text = "Malformed viewport width.\n";
                    valid = FALSE;
                } else if (!parse_bounded_uint("viewport width", width_text,
                                               0, G_MAXUINT, &parsed_width) ||
                           parsed_width < 64u ||
                           parsed_width > MAX_VIEWPORT_DIMENSION) {
                    *status = 422;
                    *error_text = "viewport width must be from 64 through 4096.\n";
                    valid = FALSE;
                } else if (!strcmp(height_text, "auto")) {
                    options->width = parsed_width;
                    options->height = DEFAULT_IMAGE_HEIGHT;
                    options->auto_height = TRUE;
                } else {
                    guint parsed_height = 0;
                    if (!decimal_query_value(height_text)) {
                        *status = 400;
                        *error_text = "Malformed viewport height.\n";
                        valid = FALSE;
                    } else if (!parse_bounded_uint("viewport height", height_text,
                                                   0, G_MAXUINT, &parsed_height) ||
                               parsed_height < 64u ||
                               parsed_height > MAX_VIEWPORT_DIMENSION) {
                        *status = 422;
                        *error_text = "viewport height must be from 64 through 4096.\n";
                        valid = FALSE;
                    } else {
                        options->width = parsed_width;
                        options->height = parsed_height;
                        options->auto_height = FALSE;
                    }
                }
                g_free(width_text);
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
                } else if (zoom < 0.1 || zoom > 5.0) {
                    *status = 422;
                    *error_text = "zoom must be from 0.1 through 5.0.\n";
                    valid = FALSE;
                } else {
                    options->zoom = zoom;
                }
            } else if (!strcmp(key, "wait")) {
                guint parsed = 0;
                if (!parse_bounded_uint("wait", value, 0, MAX_WAIT_MS, &parsed)) {
                    *status = decimal_query_value(value) ? 422 : 400;
                    *error_text = "wait must be an integer from 0 through 30000.\n";
                    valid = FALSE;
                } else {
                    options->wait_ms = parsed;
                }
            } else if (!strcmp(key, "format")) {
                if (!strcmp(value, "png")) {
                    options->format = OUTPUT_FORMAT_PNG;
                } else if (!strcmp(value, "jpeg")) {
                    options->format = OUTPUT_FORMAT_JPEG;
                } else if (!strcmp(value, "webp")) {
                    options->format = OUTPUT_FORMAT_WEBP;
                } else if (!strcmp(value, "bmp")) {
                    options->format = OUTPUT_FORMAT_BMP;
                } else {
                    *status = 422;
                    *error_text = "format must be png, jpeg, webp, or bmp.\n";
                    valid = FALSE;
                }
            } else if (!strcmp(key, "theme")) {
                if (!query_text_has_non_whitespace(value) ||
                    strnlen(value, MAX_THEME_BYTES + 1u) > MAX_THEME_BYTES) {
                    *status = 422;
                    *error_text = "theme must be a non-empty name up to 1024 bytes.\n";
                    valid = FALSE;
                } else {
                    options->theme = g_strdup(value);
                }
            } else if (!strcmp(key, "dark")) {
                options->has_dark_mode = TRUE;
                if (!*value || !g_ascii_strcasecmp(value, "true") ||
                    !strcmp(value, "1") || !g_ascii_strcasecmp(value, "yes")) {
                    options->dark_mode = TRUE;
                } else if (!g_ascii_strcasecmp(value, "false") ||
                           !strcmp(value, "0") ||
                           !g_ascii_strcasecmp(value, "no")) {
                    options->dark_mode = FALSE;
                } else {
                    *status = 422;
                    *error_text = "dark must be a boolean value.\n";
                    valid = FALSE;
                }
            } else if (!strcmp(key, "lang")) {
                if (!query_text_has_non_whitespace(value) ||
                    strnlen(value, MAX_LANGUAGE_BYTES + 1u) > MAX_LANGUAGE_BYTES) {
                    *status = 422;
                    *error_text = "lang must be a non-empty code up to 64 bytes.\n";
                    valid = FALSE;
                } else {
                    options->language = g_strdup(value);
                }
            } else {
                *status = 400;
                *error_text = "Unknown query parameter.\n";
                valid = FALSE;
            }
        }
        g_list_free(keys);
    }

    g_strfreev(pairs);
    g_hash_table_unref(seen);
    if (!valid) {
        g_clear_pointer(&options->theme, g_free);
        g_clear_pointer(&options->language, g_free);
    }
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
        if (app_ensure_view(app))
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
    const gsize max_encoded_path_length = 3u * MAX_DASHBOARD_PATH_BYTES + 1u;
    gsize raw_path_length = raw_path ?
        strnlen(raw_path, max_encoded_path_length + 1u) : 0;
    gsize query_length = query ? strnlen(query, MAX_QUERY_BYTES + 1u) : 0;
    if (raw_path_length > max_encoded_path_length ||
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
    gchar *normalized_path = NULL;
    gchar *encoded_path = NULL;
    gboolean valid_route = normalize_dashboard_uri_path(
        raw_path, &normalized_path, &encoded_path);
    gsize callback_path_length = path ?
        strnlen(path, max_encoded_path_length + 1u) : 0;
    gchar *normalized_route = normalized_path ?
        g_strdup_printf("/%s", normalized_path) : NULL;
    gchar *encoded_route = encoded_path ?
        g_strdup_printf("/%s", encoded_path) : NULL;
    valid_route = valid_route && path && callback_path_length <=
        max_encoded_path_length &&
        (!strcmp(path, raw_path) || !strcmp(path, normalized_route) ||
         !strcmp(path, encoded_route));
    g_free(normalized_route);
    g_free(encoded_route);
    if (!valid_route) {
        g_free(normalized_path);
        g_free(encoded_path);
        respond_error(message, 400,
                      "Dashboard path must be a safe relative Home Assistant route.\n");
        return;
    }
    if (!strcmp(normalized_path, "healthz") ||
        !strcmp(normalized_path, "readyz")) {
        const gchar *health_path = !strcmp(normalized_path, "healthz") ?
            "/healthz" : "/readyz";
        handle_health(message, app, health_path);
        g_free(normalized_path);
        g_free(encoded_path);
        return;
    }
    if (!strchr(normalized_path, '/') &&
        (!g_strcmp0(normalized_path, "favicon.ico") ||
         !g_strcmp0(normalized_path, "robots.txt"))) {
        g_free(normalized_path);
        g_free(encoded_path);
        respond_error(message, 404, "Not found.\n");
        return;
    }
    if (!app_ensure_view(app)) {
        g_free(normalized_path);
        g_free(encoded_path);
        respond_error(message, 503, "WPE renderer is unavailable.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Retry-After", "1");
        return;
    }
    if (app->active) {
        g_free(normalized_path);
        g_free(encoded_path);
        respond_error(message, 503, "Renderer is busy; retry shortly.\n");
        soup_message_headers_replace(soup_server_message_get_response_headers(message), "Retry-After", "1");
        return;
    }

    CaptureOptions options = { 0 };
    guint error_status = 400;
    const gchar *error_text = "Malformed capture request.\n";
    if (!uri || !parse_capture_query(uri, &options, &error_status, &error_text)) {
        g_free(normalized_path);
        g_free(encoded_path);
        g_free(options.theme);
        g_free(options.language);
        respond_error(message, error_status, error_text);
        return;
    }
    if (!options.auto_height &&
        (gsize) options.width * options.height > MAX_RAW_IMAGE_BYTES / 4u) {
        g_free(normalized_path);
        g_free(encoded_path);
        g_free(options.theme);
        g_free(options.language);
        respond_error(message, 413,
                      "Rendered image exceeds the 60 MiB raw image limit.\n");
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
    request->width = options.width;
    request->height = options.height;
    request->auto_height = options.auto_height;
    request->zoom = options.zoom;
    request->format = options.format;
    request->dark_mode = options.dark_mode;
    request->has_dark_mode = options.has_dark_mode;
    request->started_at_us = g_get_monotonic_time();
    request->path = normalized_path;
    request->encoded_path = encoded_path;
    request->theme = options.theme;
    request->language = options.language;
    app->active = request;
    request->message_disconnected_handler = g_signal_connect(
        message, "disconnected", G_CALLBACK(on_message_terminated), request_ref(request));
    request->message_finished_handler = g_signal_connect(
        message, "finished", G_CALLBACK(on_message_terminated), request_ref(request));
    request->disconnect_source = add_request_timeout(100, check_client_connection,
                                                      request);
    guint initial_height = request->auto_height ? DEFAULT_IMAGE_HEIGHT : request->height;
    wpe_view_resized(app->wpe_view, request->width, initial_height);
    webkit_web_view_set_zoom_level(app->view, request->zoom);
    g_message("Screenshot started: request_id=%llu route=/%s viewport=%ux%u%s deadline_ms=%u",
              (unsigned long long) request->id, request->path,
              request->width, initial_height,
              request->auto_height ? " auto-height" : "",
              app->render_deadline_ms);
    soup_server_message_pause(message);
    request->timeout_source = add_request_timeout(app->render_deadline_ms,
                                                   render_deadline, request);

    if (dashboard_page_can_be_reused(app, request->path)) {
        request->reused_page = TRUE;
        start_readiness_checks(request);
        return;
    }

    app->dashboard_loaded = FALSE;
    app->dashboard_last_capture_at_us = 0;
    g_clear_pointer(&app->dashboard_requested_path, g_free);

    if (app->fixture_mode) {
        if (!fixture_load(app, request))
            return;
    } else {
        gchar *uri_to_load = g_strdup_printf("%s/%s", app->ha_url,
                                             request->encoded_path);
        webkit_web_view_load_uri(app->view, uri_to_load);
        g_free(uri_to_load);
    }
}

static gboolean
init_app(App *app)
{
    app->loop = g_main_loop_new(NULL, FALSE);
    app->display = wpe_display_headless_new();
    if (!app->display) {
        g_printerr("startup error: WPE headless display could not be created\n");
        return FALSE;
    }
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

    app->server = soup_server_new("server-header", "wpe-screenshot-engine/0.2.1",
                                  "raw-paths", TRUE, NULL);
    soup_server_add_early_handler(app->server, NULL, handle_early_request, NULL, NULL);
    soup_server_add_handler(app->server, NULL, handle_request, app, NULL);
    GError *error = NULL;
    if (!soup_server_listen_all(app->server, app->port, SOUP_SERVER_LISTEN_IPV4_ONLY, &error)) {
        g_printerr("startup error: HTTP listener failed\n");
        g_clear_error(&error);
        return FALSE;
    }
    app_schedule_idle_release(app);
    return TRUE;
}

static gboolean
on_shutdown_signal(gpointer user_data)
{
    App *app = user_data;
    g_message("Shutdown signal received; releasing renderer resources");
    if (app->active)
        g_cancellable_cancel(app->active->cancellable);
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static void
app_clear(App *app)
{
    remove_source(&app->idle_release_source);
    remove_source(&app->recreate_view_source);
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
    g_free(app->dashboard_requested_path);
    if (app->token_file &&
        g_str_has_prefix(app->token_file, "/tmp/wpe-ha-token.") &&
        g_unlink(app->token_file) < 0 && errno != ENOENT)
        g_warning("Unable to remove temporary Home Assistant token file during shutdown");
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

    g_unix_signal_add(SIGTERM, on_shutdown_signal, &app);
    g_unix_signal_add(SIGINT, on_shutdown_signal, &app);

    g_message("Screenshot API ready: internal_port=%u renderer=ready", app.port);
    g_message("Capture routes may target any safe relative path on the configured Home Assistant origin");
    if (app.expected_text[0])
        g_message("Readiness text check enabled for route=/%s", app.readiness_path);
    g_main_loop_run(app.loop);
    app_clear(&app);
    return 0;
}
