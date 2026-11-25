// net.c - all the networking. Every request is HTTPS (TLS) through libcurl,
// and the server's certificate is checked against cacert.pem (shipped in the VPK).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <time.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include "net.h"

#define USER_AGENT  "VitaGitHub/1.0 (PS Vita)"   // GitHub rejects requests without one
#define CA_BUNDLE      "app0:cacert.pem"
#define CA_BUNDLE_USER "ux0:data/VitaGitHub/cacert.pem"   // optional newer copy
#define NET_MEM     (1 * 1024 * 1024)
#define MAX_API_BODY (8 * 1024 * 1024)

static char g_err[256];
static const char *g_ca = CA_BUNDLE;

// Use ux0:data/VitaGitHub/cacert.pem if you put one there, else the built-in one.
static void pick_ca_bundle(void) {
    FILE *f = fopen(CA_BUNDLE_USER, "r");
    if (f) { fclose(f); g_ca = CA_BUNDLE_USER; }
}

/* ---- Root certificates ----
 * We read cacert.pem ourselves and hand each certificate to OpenSSL one by
 * one. (Letting OpenSSL open the file failed on the Vita, and one unreadable
 * certificate would make it reject the whole file.) Bad ones are skipped. */
#define MAX_CERTS 400
static X509 *g_certs[MAX_CERTS];
static int g_ncerts, g_nbad;

static int load_certs(void) {
    FILE *f = fopen(g_ca, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, size, f);
    fclose(f);
    buf[got] = 0;

    const char *BEGIN = "-----BEGIN CERTIFICATE-----";
    const char *END = "-----END CERTIFICATE-----";
    char *p = buf;
    while (g_ncerts < MAX_CERTS && (p = strstr(p, BEGIN))) {
        char *e = strstr(p, END);
        if (!e) break;
        e += strlen(END);
        BIO *b = BIO_new_mem_buf(p, (int)(e - p));
        X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
        if (b) BIO_free(b);
        if (x) g_certs[g_ncerts++] = x;
        else g_nbad++;
        p = e;
    }
    ERR_clear_error();
    free(buf);
    return g_ncerts;
}

// curl calls this for every new connection: add our roots to OpenSSL.
static CURLcode ssl_ctx_cb(CURL *c, void *sslctx, void *parm) {
    (void)c; (void)parm;
    X509_STORE *store = SSL_CTX_get_cert_store((SSL_CTX *)sslctx);
    for (int i = 0; i < g_ncerts; i++) X509_STORE_add_cert(store, g_certs[i]);
    ERR_clear_error();   // ignore "already in store" notices
    return CURLE_OK;
}
static void *g_netmem;
static char g_auth[300];   // "Authorization: Bearer <token>" or empty

void net_set_token(const char *token) {
    if (token && *token) snprintf(g_auth, sizeof(g_auth), "Authorization: Bearer %s", token);
    else g_auth[0] = 0;
}

int net_has_token(void) { return g_auth[0] != 0; }

// Builds the extra headers. The token is ONLY added for api.github.com.
// (When GitHub redirects a download to its storage servers, curl does not
// forward this header to the other host, so the token never leaks.)
static struct curl_slist *make_headers(const char *url, const char *accept) {
    struct curl_slist *h = NULL;
    if (accept) {
        char a[128];
        snprintf(a, sizeof(a), "Accept: %s", accept);
        h = curl_slist_append(h, a);
    }
    if (!strncmp(url, "https://api.github.com/", 23)) {
        h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
        if (g_auth[0]) h = curl_slist_append(h, g_auth);
    }
    return h;
}

const char *net_error(void) { return g_err; }

int net_init(void) {
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    g_netmem = malloc(NET_MEM);
    SceNetInitParam p;
    p.memory = g_netmem;
    p.size = NET_MEM;
    p.flags = 0;
    int ret = sceNetInit(&p);
    if (ret < 0 && ret != (int)0x80410201 /* already initialised */) {
        snprintf(g_err, sizeof(g_err), "sceNetInit failed: 0x%08X", ret);
        return -1;
    }
    sceNetCtlInit();
    curl_global_init(CURL_GLOBAL_ALL);
    pick_ca_bundle();
    if (load_certs() == 0) {
        snprintf(g_err, sizeof(g_err), "Couldn't read any certificates from %s (%d bad) - reinstall the app",
                 g_ca, g_nbad);
        return -1;
    }
    return 0;
}

void net_exit(void) {
    for (int i = 0; i < g_ncerts; i++) X509_free(g_certs[i]);
    g_ncerts = 0;
    curl_global_cleanup();
    sceNetCtlTerm();
    sceNetTerm();
    free(g_netmem);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
}

// Options every request uses.
static void common_opts(CURL *c, const char *url, char *errbuf) {
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // release files redirect to a CDN
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CAINFO, NULL);         // don't let OpenSSL open files itself
    curl_easy_setopt(c, CURLOPT_CAPATH, NULL);
    curl_easy_setopt(c, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);  // we add the roots instead
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);   // really check the certificate
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  // give up if stalled 30s
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
}

static void set_curl_error(CURLcode rc, const char *errbuf) {
    const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    if (rc == CURLE_SSL_CACERT_BADFILE) {
        snprintf(g_err, sizeof(g_err), "TLS certificate setup failed: %s (%d certs)", msg, g_ncerts);
    } else if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CONNECT_ERROR) {
        // Show the Vita's date too: a wrong clock is the usual cause
        char date[16] = "?";
        time_t now = time(NULL);
        struct tm *t = gmtime(&now);
        if (t) strftime(date, sizeof(date), "%Y-%m-%d", t);
        snprintf(g_err, sizeof(g_err), "TLS error %d: %s [Vita date %s, %d certs, %d bad]",
                 (int)rc, msg, date, g_ncerts, g_nbad);
    }
    else if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT)
        snprintf(g_err, sizeof(g_err), "No connection: %s (Wi-Fi on?)", msg);
    else
        snprintf(g_err, sizeof(g_err), "%s", msg);
}

/* ---------- download to memory ---------- */

typedef struct { char *data; size_t len, cap; } MemBuf;

static size_t mem_write(void *ptr, size_t size, size_t n, void *ud) {
    MemBuf *m = ud;
    size_t add = size * n;
    if (m->len + add + 1 > MAX_API_BODY) return 0;  // too big -> abort
    if (m->len + add + 1 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 64 * 1024;
        while (ncap < m->len + add + 1) ncap *= 2;
        char *nd = realloc(m->data, ncap);
        if (!nd) return 0;
        m->data = nd;
        m->cap = ncap;
    }
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = 0;
    return add;
}

int net_get(const char *url, char **out, size_t *out_len, long *http_code) {
    char errbuf[CURL_ERROR_SIZE] = {0};
    MemBuf m = {0};
    *out = NULL;
    if (out_len) *out_len = 0;
    *http_code = 0;

    CURL *c = curl_easy_init();
    if (!c) { snprintf(g_err, sizeof(g_err), "curl init failed"); return -1; }

    struct curl_slist *hdr = make_headers(url, "application/vnd.github+json");

    common_opts(c, url, errbuf);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http_code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        set_curl_error(rc, errbuf);
        free(m.data);
        return -1;
    }
    *out = m.data ? m.data : strdup("");
    if (out_len) *out_len = m.len;
    return 0;
}

/* ---------- download to file ---------- */

typedef struct { net_progress_fn cb; void *ud; int cancelled; } Prog;

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ult, curl_off_t uln) {
    (void)ult; (void)uln;
    Prog *p = ud;
    if (p->cb && p->cb(p->ud, (double)dlnow, (double)dltotal)) {
        p->cancelled = 1;
        return 1;   // tells curl to stop
    }
    return 0;
}

int net_download(const char *url, const char *accept, const char *path,
                 net_progress_fn cb, void *userdata) {
    char errbuf[CURL_ERROR_SIZE] = {0};
    char part[512];
    snprintf(part, sizeof(part), "%s.part", path);   // write to .part, rename when done

    FILE *f = fopen(part, "wb");
    if (!f) { snprintf(g_err, sizeof(g_err), "Can't create %s", part); return -1; }
    // Big write buffer: the Vita memory card is slow with lots of small writes.
    static char iobuf[512 * 1024];
    setvbuf(f, iobuf, _IOFBF, sizeof(iobuf));

    CURL *c = curl_easy_init();
    if (!c) { fclose(f); remove(part); snprintf(g_err, sizeof(g_err), "curl init failed"); return -1; }

    Prog p = { cb, userdata, 0 };
    struct curl_slist *hdr = make_headers(url, accept);
    common_opts(c, url, errbuf);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);     // 404 etc. = error, not a saved error page
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &p);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024L);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    int write_err = fclose(f);

    if (p.cancelled) { remove(part); return 1; }
    if (rc == CURLE_HTTP_RETURNED_ERROR) {
        if (code == 401 || code == 403)
            snprintf(g_err, sizeof(g_err), "HTTP %ld - token missing, expired, or lacks permission", code);
        else if (code == 410)
            snprintf(g_err, sizeof(g_err), "HTTP 410 - this artifact has expired");
        else
            snprintf(g_err, sizeof(g_err), "HTTP %ld from server", code);
        remove(part);
        return -1;
    }
    if (rc != CURLE_OK) { set_curl_error(rc, errbuf); remove(part); return -1; }
    if (write_err) { snprintf(g_err, sizeof(g_err), "Write failed (memory card full?)"); remove(part); return -1; }

    remove(path);   // replace an older copy if there is one
    if (rename(part, path) != 0) {
        snprintf(g_err, sizeof(g_err), "Couldn't rename %s", part);
        return -1;
    }
    return 0;
}
