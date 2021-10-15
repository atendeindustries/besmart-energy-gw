#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "config.h"
#include "https.h"
#include "ca_cert.h"
#define BUFFER_SIZE 1280
#define GET_TEMPLATE "GET %s HTTP/1.1\r\n"\
                     "User-Agent: %s\r\n"\
                     "Host: %s\r\n"\
                     "Content-Type: %s\r\n"\
                     "Authorization: Bearer %s\r\n\r\n"
#define PUT_TEMPLATE "PUT %s HTTP/1.1\r\n"\
                     "User-Agent: %s\r\n"\
                     "Host: %s\r\n"\
                     "Content-Type: %s\r\n"\
                     "Content-Length: %d\r\n"\
                     "Authorization: Bearer %s\r\n\r\n"\
                     "%s"


int http_write(HTTP_INFO* hi, char* buf, size_t len);
int http_read(HTTP_INFO* hi, char* body, char* buf, int size);

// ---------------------------------------------------------

void http_set_host(HTTP_INFO* hi, char* host, char* port, bool ssl) {
    hi->host = host;
    hi->port = port;
    hi->ssl = ssl;
}

void http_set_token(HTTP_INFO* hi, char* token) {
    hi->requestHeaders.bearerToken = token;
}

int http_setup(HTTP_INFO* hi) {
    int ret;
    mbedtls_x509_crt_init(&hi->httpSSL.cacert);
    mbedtls_ctr_drbg_init(&hi->httpSSL.ctr_drbg);

    mbedtls_entropy_init(&hi->httpSSL.entropy);
    ret = mbedtls_ctr_drbg_seed( &hi->httpSSL.ctr_drbg, mbedtls_entropy_func, &hi->httpSSL.entropy, NULL, 0);
    if (ret != 0) {
        // if (DEBUG) printf( " failed! mbedtls_ctr_drbg_seed returned -0x%x\n", abs(ret));
        return ret;
    }

    ret = mbedtls_x509_crt_parse(&hi->httpSSL.cacert, ca_crt_rsa, sizeof(ca_crt_rsa));
    if( ret != 0 )
    {
        // if (DEBUG) printf(" failed!  mbedtls_x509_crt_parse returned -0x%x\n\n", abs(ret));
    }
    return ret;
}

void http_destroy(HTTP_INFO* hi) {
    mbedtls_x509_crt_free(&hi->httpSSL.cacert);
    mbedtls_ctr_drbg_free(&hi->httpSSL.ctr_drbg);
    mbedtls_entropy_free(&hi->httpSSL.entropy);
}

static int http_init(HTTP_INFO* hi) {
    hi->requestHeaders.contentType = "application/json";
    hi->requestHeaders.userAgent = "Gateway/1.0";
    mbedtls_net_init(&hi->httpSSL.server_fd);
    if (hi->ssl > 0) {
        mbedtls_ssl_init(&hi->httpSSL.ssl);
        mbedtls_ssl_config_init(&hi->httpSSL.conf);
    }

    return 0;
}

int http_open(HTTP_INFO* hi) {
    int ret;

    http_init(hi);

    ret = mbedtls_net_connect(
        &hi->httpSSL.server_fd,
        hi->host,
        hi->port,
        MBEDTLS_NET_PROTO_TCP
    );
    if (ret != 0) {
        // fprintf(stderr, "GATEWAY: failed! mbedtls_net_connect returned -0x%x\n\n", abs(ret) );
        return ret;
    }

    if (hi->ssl > 0) {

        ret = mbedtls_ssl_config_defaults(
            &hi->httpSSL.conf,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT
        );
        if(ret != 0) {
            // fprintf(stderr, "GATEWAY: failed! mbedtls_ssl_config_defaults returned -0x%x\n\n", abs(ret));
            return ret;
        }

        mbedtls_ssl_conf_authmode(&hi->httpSSL.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_ca_chain( &hi->httpSSL.conf, &hi->httpSSL.cacert, NULL );
        mbedtls_ssl_conf_rng(&hi->httpSSL.conf, mbedtls_ctr_drbg_random, &hi->httpSSL.ctr_drbg);

        ret = mbedtls_ssl_setup( &hi->httpSSL.ssl, &hi->httpSSL.conf );
        if( ret != 0 ) {
            // fprintf(stderr, "GATEWAY: failed! mbedtls_ssl_setup returned -0x%x\n\n", abs(ret));
            return ret;
        }

        ret = mbedtls_ssl_set_hostname( &hi->httpSSL.ssl, hi->host );
        if( ret != 0 ) {
            // fprintf(stderr, "GATEWAY: failed! mbedtls_ssl_set_hostname returned -0x%x\n\n", abs(ret));
            return ret;
        }
        mbedtls_ssl_set_bio( &hi->httpSSL.ssl, &hi->httpSSL.server_fd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

        while ((ret = mbedtls_ssl_handshake(&hi->httpSSL.ssl)) != 0)
        {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                // fprintf(stderr, "GATEWAY: failed! mbedtls_ssl_handshake returned -0x%x\n\n", (unsigned int) -ret );
                return ret;
            }
        }
    }

    return 0;
}

int http_close(HTTP_INFO* hi) {
    int ret;

    if (hi->ssl > 0) {
        do {
            ret = mbedtls_ssl_close_notify(&hi->httpSSL.ssl);
        } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);
        mbedtls_ssl_free(&hi->httpSSL.ssl);
        mbedtls_ssl_config_free(&hi->httpSSL.conf);
    }

    mbedtls_net_free(&hi->httpSSL.server_fd);

    return 0;
}

int http_get(HTTP_INFO* hi, char* endpoint, char* buffer, int size) {
    int ret;
    char tmp[BUFFER_SIZE];

    size_t len = snprintf(
        tmp,
        BUFFER_SIZE,
        GET_TEMPLATE,
        endpoint,
        hi->requestHeaders.userAgent,
        hi->host,
        hi->requestHeaders.contentType,
        hi->requestHeaders.bearerToken
    );
    ret = http_write(hi, tmp, len);
    if (ret != len) {
        mbedtls_strerror(ret, tmp, BUFFER_SIZE);
        snprintf(buffer, BUFFER_SIZE, "socket error while writing: %s(%d)", tmp, ret);
        return -1;
    }

    ret = http_read(hi, buffer, tmp, len);
    if (ret != 0 || strstr(tmp, "HTTP/1.1") != tmp) {
        return -1;
    }
    
    return hi->responseHeaders.status;
}

int http_put(HTTP_INFO* hi, char* endpoint, char* data, char* buffer, int size) {
    int ret;
    char tmp[BUFFER_SIZE];

    size_t len = snprintf(
        tmp,
        BUFFER_SIZE,
        PUT_TEMPLATE,
        endpoint,
        hi->requestHeaders.userAgent,
        hi->host,
        hi->requestHeaders.contentType,
        strlen(data),
        hi->requestHeaders.bearerToken,
        data
    );

    ret = http_write(hi, tmp, len);
    if (ret != len) {
        mbedtls_strerror(ret, tmp, BUFFER_SIZE);
        snprintf(buffer, BUFFER_SIZE, "socket error while writing: %s(%d)", tmp, ret);
        return -1;
    }

    ret = http_read(hi, buffer, tmp, len);
    if (ret != 0 || strstr(tmp, "HTTP/1.1") != tmp) {
        return -1;
    }
    
    return hi->responseHeaders.status;
}

int http_write(HTTP_INFO* hi, char* buf, size_t len) {
    int sent = 0;
    int ret;
    while (sent < len) {
        if (hi->ssl == 0)
            ret = mbedtls_net_send(&hi->httpSSL.server_fd, (unsigned char*)(buf + sent), (size_t)(len - sent));
        else
            ret = mbedtls_ssl_write(&hi->httpSSL.ssl, (unsigned char*)(buf + sent), len - sent);

        if(ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        else if(ret <= 0) return ret;

        sent += ret;
    }
    return sent;
}

int http_read(HTTP_INFO* hi, char* body, char* buf, int size) {
    int ret;
    int received = 0;
    while (1) {
        if (hi->ssl == 0) {
            ret = mbedtls_net_recv_timeout(&hi->httpSSL.server_fd, (unsigned char*)(buf + received), (size_t)size, 15000);
        } else {
            ret = mbedtls_ssl_read(
                &hi->httpSSL.ssl, (unsigned char*)(buf + received), size
            );
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) continue;
        else if (ret < 0) {
            mbedtls_strerror(ret, buf, BUFFER_SIZE);
            snprintf(body, BUFFER_SIZE, "socket error while reading: %s(%d)", buf, ret);
            return ret;
        } else if (ret == 0) {
            break;
        }

        received += ret;
        buf[received] = 0;

        char* contentLenPtr = strcasestr(buf, "Content-Length:");
        char* bodyPtr = strstr(buf, "\r\n\r\n");

        if (contentLenPtr != NULL && bodyPtr != NULL) {
            hi->responseHeaders.status = atoi(buf + 9);
            hi->responseHeaders.contentLength = atoi(contentLenPtr + 16);
            bodyPtr += 4;
            if (hi->responseHeaders.contentLength > strlen(bodyPtr))
                continue;
            strcpy(body, bodyPtr);
            break;
        }
    }
    return 0;
}