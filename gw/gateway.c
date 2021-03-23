/*
 * Copyright 2021 Atende Industries
 */

#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>

#define h_addr h_addr_list[0]

/* config */
const uint16_t PORT = 7890;
const char ENV_LOCAL[] = "127.0.0.1";
const char ENV_DEV[] = "127.0.0.1";
const char TOKEN[] = "";
const char METER_DEV[] = "";
const char METER_TYPE_NAME[] = "";

const char PUT_POST_TEMPLATE[] = "%s %s%s HTTP/1.1\r\n\
Authorization: Bearer %s\r\n\
Content-Type: application/json\r\n\
Content-Length: %d\r\n\r\n%s\r\n\r\n";
const char GET_TEMPLATE[] = "GET %s%s HTTP/1.1\n\
accept: application/json\n\
Authorization: Bearer %s\r\n\r\n";

void error(const char *msg) { 
    printf("%s\n", msg);
    exit(0);
}

uint8_t startsWith(const char *str, const char *pre)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
    return lenstr < lenpre ? 0 : memcmp(pre, str, lenpre) == 0;
}

struct strelem {
    char* e;
    int len;
    struct strelem* next;
};

struct strlist {
    struct strelem* begin;
    struct strelem* end;
    int totalLen;
};

struct strlist* newList(void) {
    struct strlist* lst = malloc(sizeof(struct strlist));
    lst->begin = NULL;
    lst->end = NULL;
    lst->totalLen = 0;
    return lst;
}

void destroyList(struct strlist* lst) {
    struct strelem* curr = lst->begin;
    struct strelem* next;
    while (curr != NULL) {
        next = curr->next;
        free(curr->e);
        free(curr);
        curr = next;
    }
    free(lst);
}

void printList(struct strlist* lst) {
    int i = 0;
    struct strelem* tmp = lst->begin;
    printf("[\n");
    while (tmp != NULL) {
        printf("  %d: %s,\n", i++, tmp->e);
        tmp = tmp->next;
    }
    printf("]\n");
}

void appendList(struct strlist* lst, char* e) {
    struct strelem* elem = malloc(sizeof(struct strelem));
    elem->e = e;
    elem->len = strlen(e);
    lst->totalLen += elem->len;
    elem->next = NULL;
    if (lst->begin == NULL) {
        lst->begin = lst->end = elem;
    } else {
        lst->end->next = elem;
        lst->end = elem;
    }
}

struct response {
    uint16_t status;
    struct strlist* headers;
    char* content;
};

void destroyResponse(struct response* r) {
    destroyList(r->headers);
    free(r->content);
    free(r);
}

char* getLine(char* str) {
    int len = 0;
    int size = 200;
    char* line = malloc(size);
    while (*str != '\n' && *str != '\0') {
        line[len++] = *str;
        if (len == 100) {
            size *= 1.5;
            line = realloc(line, size);
        }
        str++;
    }
    line = realloc(line, len + 1);
    line[len] = '\0';
    return line;
}

char* getValFromLine(char* line) {
    int size = 200;
    int len = 0;
    char* val = malloc(size);
    while (*line != ':' && *line != '\0') line++;
    while (*line == ':' || *line == ' ' || *line == '"') line++;
    while (*line != '\n' && *line != '\0') {
        val[len++] = *line;
        line++;
    }
    if (val[len - 1] == ',') len--;
    if (val[len - 1] == '"') len--;
    val = realloc(val, len + 1);
    val[len] = '\0';
    return val;
}

char* getVarFromFormattedJson(char* json, char* var) {
    char* line;
    char* tmp;
    do {
        line = tmp = getLine(json);
        if (line[0] != '\0') {
            while(*tmp == ' ' || *tmp == '"') {
                tmp++;
            }
            if (startsWith(tmp, var)) {
                return getValFromLine(tmp);
            }

            json += strlen(line) + 1;
        }
    } while (line[0] != '\0');

    return "";
}

struct strlist* createParamList(int cnt, char** params) {
    int i, len;
    char* tmp;
    struct strlist* paramList = newList();
    for (i = 0; i < cnt; i++) {
        len = strlen(params[i]);
        tmp = malloc((len + 2));
        if (paramList->begin == NULL) tmp[0] = '?';
        else tmp[0] = '&';
        memcpy(tmp + 1, params[i], len + 1);
        appendList(paramList, tmp);
    }
    return paramList;
}

char* readLine(int fd, char removeCRLF) {
    int size = 100;
    char* line = malloc(size);
    char currentChar = 0;
    int len = 0, bytes = 0;

    memset(line, 0, size);
    do {
        bytes = read(fd, &currentChar, 1);
        if (bytes < 0) error("Error while reading from socket.");
        if (bytes > 0) {
            line[len++] = currentChar;
        }
        if (len == size) {
            size *= 1.5;
            line = realloc(line, size);
        }
    } while (bytes > 0 && currentChar != '\n');
    size = len + 1 - (removeCRLF > 0) * 2;
    line = realloc(line, size);
    line[size - 1] = '\0';
    return line;
}

uint16_t getStatusCode(int sockfd) {
    char status[4];
    uint16_t statuscode;
    int i = 0;
    char* line = readLine(sockfd, 1);

    if (!startsWith(line, "HTTP/")) error("Couldn't parse HTTP response.");
    while (line[i++] != ' ') {}
    memcpy(status, line + i, 3);
    status[3] = '\0';
    statuscode = atoi(status);
    return statuscode;
}

struct strlist* getHeaders(int sockfd) {
    char isEmpty = 0;
    char* tmp;
    struct strlist* headers = newList();
    do {
        tmp = readLine(sockfd, 1);
        isEmpty = (tmp[0] == '\0');
        if (!isEmpty) appendList(headers, tmp);
    } while (!isEmpty);
    
    return headers;
}

int getContentLengthFromHeaders(struct strlist* headers) {
    struct strelem* elem = headers->begin;
    while (elem != NULL) {
        if (startsWith(elem->e, "Content-Length:")) {
            return atoi(elem->e + 16);
        }
        elem = elem->next;
    }
    return -1;
}

struct response* getResponse(int sockfd) {
    int contentLen, bytes, received = 0;
    char* content;

    uint16_t statuscode = getStatusCode(sockfd);
    struct strlist* headers = getHeaders(sockfd);
    contentLen = getContentLengthFromHeaders(headers);

    /* Reading response body */

    content = malloc(contentLen + 1);
    memset(content, 0, contentLen + 1);

    while (received < contentLen) {
        bytes = read(sockfd, content + received, contentLen - received);
        if (bytes < 0) error("ERROR reading content from socket");
        if (bytes == 0) break;
        received+=bytes;
    }
    close(sockfd);

    struct response* r = malloc(sizeof(content));
    r->status = statuscode;
    r->headers = headers;
    r->content = content;
    return r;
}

struct response* sendRequest(struct in_addr* host, char* method, char* endpoint, struct strlist* paramList, char* requestBody) {
    char* request;
    char* params = malloc(paramList->totalLen + 1);
    struct strelem* tmp = paramList->begin;
	struct sockaddr_in serv_addr;
    int sockfd, bytes, sent, total, bodyLen, contentLenDigits = 0, i = 0;

    /* Parsing params */

    memset(params, 0, paramList->totalLen);
    while (tmp != NULL) {
        memcpy(params + i, tmp->e, tmp->len);
        i += tmp->len;
        tmp = tmp->next;
    }
    params[i] = '\0';

    /* Building request */
    size_t requestLen = 0;
    const char* template;
    if (strcmp(method, "GET") == 0) template = GET_TEMPLATE;
    if (strcmp(method, "POST") == 0 ||
        strcmp(method, "PUT") == 0) template = PUT_POST_TEMPLATE;

    if (strcmp(method, "GET") == 0) {
        requestLen = 65 + strlen(endpoint) + strlen(params) + strlen(TOKEN);
        request = malloc(requestLen + 1);
        sprintf(request, template, endpoint, params, TOKEN);
    } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
        bodyLen = strlen(requestBody);
        while (bodyLen > 0) { bodyLen /= 10; contentLenDigits++; }
        requestLen = 92 + strlen(method) + strlen(endpoint) + strlen(params) + strlen(TOKEN) + contentLenDigits + strlen(requestBody);
        request = malloc(requestLen + 1);
        sprintf(request, template, method, endpoint, params, TOKEN, strlen(requestBody), requestBody);
    } else {
        error("Invalid method");
    }
    request[requestLen] = '\0';

    free(params);

    /* Connecting to host */

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = (PORT >> 8) | (PORT << 8);
	memcpy(&serv_addr.sin_addr.s_addr, (char *)host, sizeof(*host));

    if (connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr)) < 0)
		error("Error connecting!");

    /* Sending data */

    total = strlen(request);
    sent = 0;
    do {
        bytes = write(sockfd,request+sent,total-sent);
        if (bytes < 0)
            error("ERROR writing message to socket");
        if (bytes == 0)
            break;
        sent+=bytes;
    } while (sent < total);

    free(request);

    return getResponse(sockfd);
}

int main(int argc, char **argv)
{
    srand(time(NULL));

    if (argc < 2) {
        error("Usage: /bin/sockettest [env]");
    }

    struct response* r;
    char* cid;
    char* mid;

    /* Setting host */

    char* env = argv[1];
    struct in_addr host;

    if (strcmp("local", env) == 0) {
        inet_aton(ENV_LOCAL, &host);
    } else if (strcmp("dev", env) == 0) {
        inet_aton(ENV_DEV, &host);
    } else {
        error("Invalid env. Available: local/dev");
    }

    /* Creating param list */

    char* meterDevParam = malloc(strlen(METER_DEV) + 12);
    sprintf(meterDevParam, "?meter_dev=%s", METER_DEV);
    char* meterTypeNameParam = malloc((strlen(METER_TYPE_NAME) + 18));
    sprintf(meterTypeNameParam, "&meter_type_name=%s", METER_TYPE_NAME);

    struct strlist* params = newList();
    appendList(params, meterDevParam);
    appendList(params, meterTypeNameParam);

    /* Sending request to get cid/mid */

    printf("Sending request to get cid/mid. GET /api/sensors/states/find?meter_dev=%s&meter_type_name=%s - ", METER_DEV, METER_TYPE_NAME);
    fflush(stdout);
    r = sendRequest(&host, "GET", "/api/sensors/states/find", params, NULL);
    destroyList(params);

    /* Processing response */

    printf("%d\n", r->status);

    cid = getVarFromFormattedJson(r->content, "client_cid");
    mid = getVarFromFormattedJson(r->content, "sensor_mid");

    destroyResponse(r);

    if (strlen(mid) > 0) {
        printf("Identified (cid: %s, mid: %s)\n", cid, mid);
    } else {
        error("Couldn't identify meter.");
    }



    char template[] = "[{\n\
    \"client_cid\": %s,\n\
    \"sensor_mid\": %s,\n\
    \"signal_type_moid\": 32,\n\
    \"data\": [{\n\
        \"time\": %ld,\n\
        \"value\": %d,\n\
        \"type\": \"DBL\",\n\
        \"origin\": 2\n\
    }]\n\
}]";

    int valueDigits;
    while (1) {
        valueDigits = 0;
        int value = rand() % (1500 + 1 - 500) + 500;
        while (value != 0) {value /= 10; valueDigits++;}
        // size_t size = snprintf(NULL, 0, template, cid, mid, time(NULL) * 1000, value) + 1;
        size_t size = strlen(template) - 9 + strlen(cid) + strlen(mid) + 13 + valueDigits + 1; 

        char* requestBody = malloc(size);
        sprintf(requestBody, template, cid, mid, time(NULL) * 1000, value);
        requestBody[size - 1] = '\0';

        /* Sending request to put data */
        params = newList();

        printf("Putting signals data - ");
        fflush(stdout);
        r = sendRequest(&host, "PUT", "/api/sensors/signals/data", params, requestBody);
        destroyList(params);
        free(requestBody);
        printf("%d\n", r->status);

        /* Processing response */

        printf("Body:\n%s\n", r->content);
        printf("\n\nSleeping 1min...\n\n");
        sleep(60);
    }

	return 0;
}
