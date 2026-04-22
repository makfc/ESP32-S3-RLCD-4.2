#include "news_service.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#define TAG        "news"
#define RSS_URL    "https://news.google.com/rss?pz=1&cf=all&hl=zh-HK&gl=HK&ceid=HK:zh-Hant"
#define BUF_INIT_SIZE  (32 * 1024)
#define BUF_MAX_SIZE   (256 * 1024)
#define NEWS_INIT_CAP 16

static char  **s_titles = NULL;
static size_t  s_count = 0;
static size_t  s_cap = 0;

/* ── HTTP receive buffer (allocated in PSRAM) ── */
static char *s_buf     = NULL;
static int   s_buf_len = 0;
static int   s_buf_cap = 0;
static bool  s_buf_truncated = false;

static void log_heap_state(const char *stage)
{
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t spi_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t spi_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    ESP_LOGI(
        TAG,
        "heap %s: internal free=%u largest=%u, spiram free=%u largest=%u",
        (stage != NULL) ? stage : "unknown",
        (unsigned int)int_free,
        (unsigned int)int_largest,
        (unsigned int)spi_free,
        (unsigned int)spi_largest
    );
}

static void clear_titles(void)
{
    if (s_titles != NULL) {
        for (size_t i = 0; i < s_count; i++) {
            free(s_titles[i]);
        }
        free(s_titles);
    }
    s_titles = NULL;
    s_count = 0;
    s_cap = 0;
}

static bool reserve_titles(size_t need_cap)
{
    if (need_cap <= s_cap) {
        return true;
    }
    size_t new_cap = (s_cap == 0) ? NEWS_INIT_CAP : s_cap;
    while (new_cap < need_cap) {
        new_cap *= 2;
    }
    char **new_arr = (char **)realloc(s_titles, new_cap * sizeof(char *));
    if (new_arr == NULL) {
        return false;
    }
    s_titles = new_arr;
    s_cap = new_cap;
    return true;
}

static bool ensure_http_buf_capacity(int need_len)
{
    if (need_len <= s_buf_cap) {
        return true;
    }

    int new_cap = (s_buf_cap > 0) ? s_buf_cap : BUF_INIT_SIZE;
    while (new_cap < need_len && new_cap < BUF_MAX_SIZE) {
        new_cap *= 2;
    }
    if (new_cap < need_len) {
        return false;
    }

    char *new_buf = (char *)heap_caps_realloc(s_buf, new_cap, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        return false;
    }
    s_buf = new_buf;
    s_buf_cap = new_cap;
    return true;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_buf && evt->data_len > 0) {
            const int need_len = s_buf_len + evt->data_len + 1;
            if (ensure_http_buf_capacity(need_len)) {
                memcpy(s_buf + s_buf_len, evt->data, evt->data_len);
                s_buf_len += evt->data_len;
            } else {
                s_buf_truncated = true;
            }
        }
    }
    return ESP_OK;
}

/* ── HTML entity decoder ── */
static void decode_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if      (strncmp(r, "&amp;",  5) == 0) { *w++ = '&';  r += 5; }
            else if (strncmp(r, "&lt;",   4) == 0) { *w++ = '<';  r += 4; }
            else if (strncmp(r, "&gt;",   4) == 0) { *w++ = '>';  r += 4; }
            else if (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; }
            else if (strncmp(r, "&#39;",  5) == 0) { *w++ = '\''; r += 5; }
            else if (strncmp(r, "&nbsp;", 6) == 0) { *w++ = ' ';  r += 6; }
            else if (r[1] == '#') {
                /* numeric entity &#DDD; or &#xHHH; — encode as UTF-8 */
                char *semi = strchr(r + 2, ';');
                if (semi) {
                    long cp = (r[2] == 'x' || r[2] == 'X')
                              ? strtol(r + 3, NULL, 16)
                              : strtol(r + 2, NULL, 10);
                    if (cp < 0x80) {
                        *w++ = (char)cp;
                    } else if (cp < 0x800) {
                        *w++ = (char)(0xC0 | (cp >> 6));
                        *w++ = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        *w++ = (char)(0xE0 | (cp >> 12));
                        *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                        *w++ = (char)(0x80 | (cp & 0x3F));
                    }
                    r = semi + 1;
                } else {
                    *w++ = *r++;
                }
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool add_title(const char *start, size_t len, bool need_decode)
{
    if (start == NULL) {
        return false;
    }
    if (len >= NEWS_TITLE_LEN) {
        len = NEWS_TITLE_LEN - 1;
    }
    if (!reserve_titles(s_count + 1)) {
        ESP_LOGE(TAG, "Out of memory while growing titles array");
        return false;
    }

    char *title = (char *)malloc(len + 1);
    if (title == NULL) {
        ESP_LOGE(TAG, "Out of memory while allocating title");
        return false;
    }

    memcpy(title, start, len);
    title[len] = '\0';
    if (need_decode) {
        decode_entities(title);
    }

    s_titles[s_count] = title;
    ESP_LOGI(TAG, "[%u] %s", (unsigned int)s_count, s_titles[s_count]);
    s_count++;
    return true;
}

/* ── RSS parser: extract <item><title> text ── */
static void parse_rss(void)
{
    char *ptr = s_buf;
    while (1) {
        char *item = strstr(ptr, "<item>");
        if (!item) break;
        char *item_end = strstr(item, "</item>");
        if (!item_end) break;

        char *ts = strstr(item, "<title>");
        if (!ts || ts > item_end) { ptr = item_end + 7; continue; }
        ts += 7;

        char *te;
        bool cdata = (strncmp(ts, "<![CDATA[", 9) == 0);
        if (cdata) {
            ts += 9;
            te = strstr(ts, "]]>");
        } else {
            te = strstr(ts, "</title>");
        }
        if (!te || te > item_end) { ptr = item_end + 7; continue; }

        const size_t len = (size_t)(te - ts);
        if (!add_title(ts, len, !cdata)) {
            break;
        }
        ptr = item_end + 7;
    }
}

/* ── Public API ── */

void news_service_fetch(void)
{
    log_heap_state("before_fetch");
    clear_titles();
    s_buf_len = 0;
    s_buf_cap = 0;
    s_buf_truncated = false;

    s_buf = (char *)heap_caps_malloc(BUF_INIT_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) { ESP_LOGE(TAG, "PSRAM alloc failed"); return; }
    memset(s_buf, 0, BUF_INIT_SIZE);
    s_buf_cap = BUF_INIT_SIZE;

    esp_http_client_config_t cfg = {
        .url              = RSS_URL,
        .event_handler    = http_event_cb,
        .timeout_ms       = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 4096,
        .buffer_size_tx   = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        heap_caps_free(s_buf);
        s_buf = NULL;
        s_buf_cap = 0;
        return;
    }
    /* Request uncompressed response so we can parse it directly */
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    ESP_LOGI(TAG, "HTTP start: %s", RSS_URL);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        const int http_status = esp_http_client_get_status_code(client);
        s_buf[s_buf_len] = '\0';
        ESP_LOGI(TAG, "HTTP ok status=%d bytes=%d cap=%d", http_status, s_buf_len, s_buf_cap);
        if (http_status != 200) {
            ESP_LOGW(TAG, "Unexpected HTTP status=%d (feed may be redirected/blocked)", http_status);
        }
        parse_rss();
        if (s_count == 0) {
            ESP_LOGW(TAG, "RSS parse completed but no <item><title> found");
        }
        if (s_buf_truncated) {
            ESP_LOGW(TAG, "HTTP body truncated at %d bytes (cap=%d)", s_buf_len, s_buf_cap);
        }
    } else {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
        log_heap_state("http_error");
    }

    esp_http_client_cleanup(client);
    heap_caps_free(s_buf);
    s_buf = NULL;
    s_buf_cap = 0;

    log_heap_state("after_fetch");
    ESP_LOGI(TAG, "fetched %u headline(s)", (unsigned int)s_count);
}

size_t news_service_count(void) { return s_count; }

bool news_service_get(size_t idx, char *buf, size_t len)
{
    if (idx >= s_count) return false;
    strlcpy(buf, s_titles[idx], len);
    return true;
}
