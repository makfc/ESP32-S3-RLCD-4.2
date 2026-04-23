#include "news_service.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

#define TAG        "news"
#define RSS_URL    "https://news.google.com/rss?pz=1&cf=all&hl=zh-HK&gl=HK&ceid=HK:zh-Hant"
#define BUF_INIT_SIZE  (32 * 1024)
#define BUF_MAX_SIZE   (256 * 1024)
#define NEWS_INIT_CAP 16
#define HTTP_RX_BUF_SIZE 2048
#define HTTP_TX_BUF_SIZE 512

static char  **s_titles = NULL;
static time_t *s_pub_epochs = NULL;
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
    if (s_pub_epochs != NULL) {
        free(s_pub_epochs);
    }
    s_titles = NULL;
    s_pub_epochs = NULL;
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
    char **new_titles = (char **)malloc(new_cap * sizeof(char *));
    if (new_titles == NULL) {
        return false;
    }
    time_t *new_pub_epochs = (time_t *)malloc(new_cap * sizeof(time_t));
    if (new_pub_epochs == NULL) {
        free(new_titles);
        return false;
    }

    memset(new_titles, 0, new_cap * sizeof(char *));
    memset(new_pub_epochs, 0, new_cap * sizeof(time_t));
    if ((s_titles != NULL) && (s_count > 0)) {
        memcpy(new_titles, s_titles, s_count * sizeof(char *));
    }
    if ((s_pub_epochs != NULL) && (s_count > 0)) {
        memcpy(new_pub_epochs, s_pub_epochs, s_count * sizeof(time_t));
    }

    free(s_titles);
    free(s_pub_epochs);
    s_titles = new_titles;
    s_pub_epochs = new_pub_epochs;
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

static bool is_leap_year(int year)
{
    return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int k_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if ((month < 1) || (month > 12)) {
        return 0;
    }
    if ((month == 2) && is_leap_year(year)) {
        return 29;
    }
    return k_days[month - 1];
}

static int month_from_abbr(const char *abbr)
{
    if (abbr == NULL) {
        return -1;
    }
    if (strncmp(abbr, "Jan", 3) == 0) return 1;
    if (strncmp(abbr, "Feb", 3) == 0) return 2;
    if (strncmp(abbr, "Mar", 3) == 0) return 3;
    if (strncmp(abbr, "Apr", 3) == 0) return 4;
    if (strncmp(abbr, "May", 3) == 0) return 5;
    if (strncmp(abbr, "Jun", 3) == 0) return 6;
    if (strncmp(abbr, "Jul", 3) == 0) return 7;
    if (strncmp(abbr, "Aug", 3) == 0) return 8;
    if (strncmp(abbr, "Sep", 3) == 0) return 9;
    if (strncmp(abbr, "Oct", 3) == 0) return 10;
    if (strncmp(abbr, "Nov", 3) == 0) return 11;
    if (strncmp(abbr, "Dec", 3) == 0) return 12;
    return -1;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= (month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

static bool parse_pubdate_to_epoch(const char *pub_date, time_t *out_epoch)
{
    if ((pub_date == NULL) || (out_epoch == NULL)) {
        return false;
    }

    char wday[8] = {0};
    char mon[8] = {0};
    char tz[8] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;

    const int matched = sscanf(
        pub_date,
        "%7[^,], %d %7s %d %d:%d:%d %7s",
        wday,
        &day,
        mon,
        &year,
        &hour,
        &min,
        &sec,
        tz
    );
    if (matched != 8) {
        return false;
    }

    const int month = month_from_abbr(mon);
    if (month < 1) {
        return false;
    }
    if ((day < 1) || (day > days_in_month(year, month))) {
        return false;
    }
    if ((hour < 0) || (hour > 23) || (min < 0) || (min > 59) || (sec < 0) || (sec > 60)) {
        return false;
    }
    if ((strcmp(tz, "GMT") != 0) && (strcmp(tz, "UTC") != 0) && (strcmp(tz, "+0000") != 0)) {
        return false;
    }

    const int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    const int64_t epoch = days * 86400LL + (int64_t)hour * 3600LL + (int64_t)min * 60LL + (int64_t)sec;
    if (epoch < 0) {
        return false;
    }
    *out_epoch = (time_t)epoch;
    return true;
}

static bool add_title(const char *start, size_t len, bool need_decode, time_t pub_epoch)
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
    s_pub_epochs[s_count] = pub_epoch;
    ESP_LOGI(
        TAG,
        "[%u] %s (pub_epoch=%lld)",
        (unsigned int)s_count,
        s_titles[s_count],
        (long long)s_pub_epochs[s_count]
    );
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

        time_t pub_epoch = 0;
        char *pub_s = strstr(item, "<pubDate>");
        if ((pub_s != NULL) && (pub_s < item_end)) {
            pub_s += 9;
            char *pub_e = strstr(pub_s, "</pubDate>");
            if ((pub_e != NULL) && (pub_e <= item_end)) {
                char pub_buf[64] = {0};
                size_t pub_len = (size_t)(pub_e - pub_s);
                if (pub_len >= sizeof(pub_buf)) {
                    pub_len = sizeof(pub_buf) - 1;
                }
                memcpy(pub_buf, pub_s, pub_len);
                pub_buf[pub_len] = '\0';
                if (!parse_pubdate_to_epoch(pub_buf, &pub_epoch)) {
                    ESP_LOGW(TAG, "pubDate parse failed: %s", pub_buf);
                }
            }
        }

        const size_t len = (size_t)(te - ts);
        if (!add_title(ts, len, !cdata, pub_epoch)) {
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
        .buffer_size      = HTTP_RX_BUF_SIZE,
        .buffer_size_tx   = HTTP_TX_BUF_SIZE,
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
        if (s_buf_len > 0) {
            /* Even when TLS/HTTP ends with an error, the partial payload often still has usable headlines. */
            s_buf[s_buf_len] = '\0';
            ESP_LOGW(TAG, "Try partial RSS parse from %d bytes after HTTP error", s_buf_len);
            parse_rss();
            ESP_LOGW(TAG, "Partial parse recovered %u headline(s)", (unsigned int)s_count);
        }
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

bool news_service_get_relative_age(size_t idx, char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0) || (idx >= s_count)) {
        return false;
    }
    buf[0] = '\0';

    const time_t pub_epoch = s_pub_epochs[idx];
    if (pub_epoch <= 0) {
        return false;
    }

    time_t now = 0;
    time(&now);
    if (now <= 0) {
        return false;
    }

    int64_t age_sec = (int64_t)now - (int64_t)pub_epoch;
    if (age_sec < 0) {
        age_sec = 0;
    }

    if (age_sec < 60) {
        strlcpy(buf, "剛剛", len);
        return true;
    }

    if (age_sec < 3600) {
        const int mins = (int)(age_sec / 60);
        snprintf(buf, len, "%d分鐘前", mins);
        return true;
    }

    if (age_sec < 86400) {
        const int hours = (int)(age_sec / 3600);
        if (hours == 1) {
            strlcpy(buf, "一小時前", len);
        } else if (hours == 2) {
            strlcpy(buf, "兩小時前", len);
        } else {
            snprintf(buf, len, "%d小時前", hours);
        }
        return true;
    }

    const int days = (int)(age_sec / 86400);
    if (days == 1) {
        strlcpy(buf, "一天前", len);
    } else if (days == 2) {
        strlcpy(buf, "兩天前", len);
    } else {
        snprintf(buf, len, "%d天前", days);
    }
    return true;
}
