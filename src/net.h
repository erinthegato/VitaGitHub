// net.h - HTTPS helpers built on libcurl + OpenSSL (TLS).
#ifndef NET_H
#define NET_H

#include <stddef.h>

// Called often during a download. Return non-zero to cancel.
typedef int (*net_progress_fn)(void *userdata, double done, double total);

int  net_init(void);
void net_exit(void);

// Personal access token for private repos / Actions. NULL or "" = signed out.
// It is only ever sent to api.github.com.
void net_set_token(const char *token);
int  net_has_token(void);

// Downloads a URL into memory (for API calls). *out must be freed by caller.
// Returns 0 on success, -1 on network/TLS error (see net_error()).
int net_get(const char *url, char **out, size_t *out_len, long *http_code);

// Downloads a URL straight into a file. accept = Accept header or NULL.
// Returns 0 = OK, 1 = cancelled by user, -1 = error (see net_error()).
int net_download(const char *url, const char *accept, const char *path,
                 net_progress_fn cb, void *userdata);

const char *net_error(void);

#endif
