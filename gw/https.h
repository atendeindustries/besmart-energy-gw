//
// Created by HISONA on 2016. 2. 29..
//

#ifndef HTTPS_CLIENT_HTTPS_H
#define HTTPS_CLIENT_HTTPS_H

/*---------------------------------------------------------------------*/
#include "mbedtls/include/mbedtls/net.h"
#include "mbedtls/include/mbedtls/entropy.h"
#include "mbedtls/include/mbedtls/ctr_drbg.h"
#include "mbedtls/include/mbedtls/error.h"
#include "mbedtls/include/mbedtls/certs.h"

/*---------------------------------------------------------------------*/
#define H_FIELD_SIZE     128
#define H_READ_SIZE     512

#undef TRUE
#undef FALSE

#define TRUE    1
#define FALSE   0

typedef unsigned char BOOL;

typedef struct
{
    char method[8];
    int  status;
    char content_type[H_FIELD_SIZE];
    long content_length;
    BOOL chunked;
    BOOL close;
    char location[H_FIELD_SIZE];
    char referrer[H_FIELD_SIZE];
    char cookie[H_FIELD_SIZE];
    char boundary[H_FIELD_SIZE];
    char authorization[H_FIELD_SIZE];

} HTTP_HEADER;

typedef struct
{
    BOOL    verify;

    mbedtls_net_context         ssl_fd;
    mbedtls_entropy_context     entropy;
    mbedtls_ctr_drbg_context    ctr_drbg;
    mbedtls_ssl_context         ssl;
    mbedtls_ssl_config          conf;
    mbedtls_x509_crt            cacert;

} HTTP_SSL;

typedef struct {

    BOOL    https;
    char    host[256];
    char    port[8];
    char    path[H_FIELD_SIZE];

} HTTP_URL;

typedef struct
{
    HTTP_URL    url;

    HTTP_HEADER request;
    HTTP_HEADER response;
    HTTP_SSL    tls;

    long        length;
    char        r_buf[H_READ_SIZE];
    long        r_len;
    BOOL        header_end;
    char        *body;
    long        body_size;
    long        body_len;


} HTTP_INFO;


/*---------------------------------------------------------------------*/

char *strtoken(char *src, char *dst, int size);

int  http_init(HTTP_INFO *hi, BOOL verify);
int  http_close(HTTP_INFO *hi);
int  http_get(HTTP_INFO *hi, char *url, char *response, int size);
int  http_put_post(char* method, HTTP_INFO *hi, char *url, char *data, char *response, int size);

void http_strerror(char *buf, int len);
int  http_open(HTTP_INFO *hi, char *url);
int  http_write_header(HTTP_INFO *hi);
int  http_write(HTTP_INFO *hi, char *data, int len);
int  http_write_end(HTTP_INFO *hi);
int  http_read_chunked(HTTP_INFO *hi, char *response, int size);

#endif //HTTPS_CLIENT_HTTPS_H

