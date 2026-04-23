#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEWS_TITLE_LEN   192   /* UTF-8 bytes; one TC char = 3 bytes */

/* Fetch RSS headlines (blocking, call while Wi-Fi has IP). */
void    news_service_fetch(void);

/* Number of headlines successfully fetched. */
size_t  news_service_count(void);

/* Copy headline at idx (0-based) into buf. Returns false if idx out of range. */
bool    news_service_get(size_t idx, char *buf, size_t len);

/* Build relative publish age text for headline idx, e.g. "20分鐘前", "兩小時前".
 * Returns false if idx invalid or publish time unavailable. */
bool    news_service_get_relative_age(size_t idx, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
