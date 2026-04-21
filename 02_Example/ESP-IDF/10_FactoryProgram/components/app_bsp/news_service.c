#include "news_service.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#define TAG        "news"
#define RSS_URL    "https://news.google.com/rss?pz=1&cf=all&hl=zh-HK&gl=HK&ceid=HK:zh-Hant"
#define BUF_SIZE   (56 * 1024)

static char    s_titles[NEWS_MAX_COUNT][NEWS_TITLE_LEN];
static uint8_t s_count = 0;

/* ── HTTP receive buffer (allocated in PSRAM) ── */
static char *s_buf     = NULL;
static int   s_buf_len = 0;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_buf && s_buf_len + evt->data_len < BUF_SIZE - 1) {
            memcpy(s_buf + s_buf_len, evt->data, evt->data_len);
            s_buf_len += evt->data_len;
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

/* ── RSS parser: extract <item><title> text ── */
static void parse_rss(void)
{
    char *ptr = s_buf;
    while (s_count < NEWS_MAX_COUNT) {
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

        size_t len = (size_t)(te - ts);
        if (len >= NEWS_TITLE_LEN) len = NEWS_TITLE_LEN - 1;
        memcpy(s_titles[s_count], ts, len);
        s_titles[s_count][len] = '\0';

        if (!cdata) decode_entities(s_titles[s_count]);

        ESP_LOGI(TAG, "[%u] %s", s_count, s_titles[s_count]);
        s_count++;
        ptr = item_end + 7;
    }
}

/* ── Public API ── */

void news_service_fetch(void)
{
    s_count = 0;

    s_buf = (char *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) { ESP_LOGE(TAG, "PSRAM alloc failed"); return; }
    memset(s_buf, 0, BUF_SIZE);
    s_buf_len = 0;

    esp_http_client_config_t cfg = {
        .url              = RSS_URL,
        .event_handler    = http_event_cb,
        .timeout_ms       = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size      = 4096,
        .buffer_size_tx   = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    /* Request uncompressed response so we can parse it directly */
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        s_buf[s_buf_len] = '\0';
        parse_rss();
    } else {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    heap_caps_free(s_buf);
    s_buf = NULL;

    ESP_LOGI(TAG, "fetched %u headline(s)", s_count);
}

uint8_t news_service_count(void) { return s_count; }

bool news_service_get(uint8_t idx, char *buf, size_t len)
{
    if (idx >= s_count) return false;
    strlcpy(buf, s_titles[idx], len);
    return true;
}
