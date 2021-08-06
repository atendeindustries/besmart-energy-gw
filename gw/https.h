#ifndef HTTPS_CLIENT_HTTPS_H
#define HTTPS_CLIENT_HTTPS_H

#include "mbedtls/include/mbedtls/net_sockets.h"
#include "mbedtls/include/mbedtls/entropy.h"
#include "mbedtls/include/mbedtls/ctr_drbg.h"
#include "mbedtls/include/mbedtls/error.h"

typedef char bool;
typedef struct {
    mbedtls_net_context         server_fd;
    mbedtls_entropy_context     entropy;
    mbedtls_ctr_drbg_context    ctr_drbg;
    mbedtls_ssl_context         ssl;
    mbedtls_ssl_config          conf;
    mbedtls_x509_crt            cacert;
} HTTP_SSL;

typedef struct {
    char* userAgent;
    char* contentType;
    char* bearerToken;
} HTTP_REQUEST_HEADERS;

typedef struct {
    int status;
    int contentLength;
} HTTP_RESPONSE_HEADERS;
typedef struct {
    HTTP_REQUEST_HEADERS requestHeaders;
    HTTP_RESPONSE_HEADERS responseHeaders;
    HTTP_SSL httpSSL;
    char* addr;
    char* host;
    char* port;
    bool ssl;
} HTTP_INFO;

void http_set_host(HTTP_INFO* hi, char* host, char* port, bool ssl);
void http_set_token(HTTP_INFO* hi, char* token);
int http_get(HTTP_INFO* hi, char* endpoint, char* buffer, int size);
int http_put(HTTP_INFO* hi, char* endpoint, char* data, char* buffer, int size);
int http_setup(HTTP_INFO* hi);
void http_destroy(HTTP_INFO* hi);
int http_open(HTTP_INFO* hi);
int http_close(HTTP_INFO* hi);

#endif //HTTPS_CLIENT_HTTPS_H

