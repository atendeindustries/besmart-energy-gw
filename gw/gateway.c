/*
 * Copyright 2021 Atende Industries
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "https.h"

/* config */
const uint16_t PORT = 443;
const char ENV_LOCAL[] = "127.0.0.1";
const char ENV_DEV[] = "127.0.0.1";
const char ENV_DEV_NAME[] = "localhost";
const char TOKEN[] = "";
const char METER_DEV[] = "";
const char METER_TYPE_NAME[] = "";
const char FIND_STATE_ENDPOINT[] = "";
const char PUT_SIGNALS_DATA_ENDPOINT[] = "";
// ------

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

int main(int argc, char **argv)
{
    srand(time(NULL));

    time_t add_time = 0;

    if (argc < 2) {
        error("Usage: /bin/sockettest [env]");
    }

    char* cid;
    char* mid;

    /* Setting host */

    char* env = argv[1];
    char ip[15];
    char endpoint[1024];
    char data[1024];
    char response[4096];

    if (strcmp("local", env) == 0) {
        strcpy(ip, ENV_LOCAL);
    } else if (strcmp("dev", env) == 0) {
        strcpy(ip, ENV_DEV);
    } else {
        error("Invalid env. Available: local/dev");
    }

    int return_code, size;
    HTTP_INFO hi1;
    http_init(&hi1, FALSE);

    if(http_open(&hi1, ip) < 0)
    {
        http_strerror(data, 1024);
        http_close(&hi1);
        error(data);
    }

    sprintf(endpoint, "https://%s:%d%s?meter_dev=%s&meter_type_name=%s", ENV_DEV_NAME, PORT, FIND_STATE_ENDPOINT, METER_DEV, METER_TYPE_NAME);
    printf("%s - ", endpoint);
    fflush(stdout);
    strcpy(hi1.request.authorization, TOKEN);
    return_code = http_get(&hi1, endpoint, response, sizeof(response));
    printf("%d\n", return_code);
    printf("response: %s\n", response);

    /* Processing response */
    cid = getVarFromFormattedJson(response, "client_cid");
    mid = getVarFromFormattedJson(response, "sensor_mid");

    if (strlen(mid) > 0) {
        printf("Identified (cid: %s, mid: %s)\n\n", cid, mid);
    } else {
        error("Couldn't identify meter.");
    }
    float value = (float)rand() / (float)RAND_MAX;
    while (1) {
        value += (float)rand() / (float)RAND_MAX;

        size = sprintf(data,
                    "[{\n"
                    "    \"client_cid\": %s,\n"
                    "    \"sensor_mid\": %s,\n"
                    "    \"signal_type_moid\": 32,\n"
                    "    \"data\": [{\n"
                    "        \"time\": %llu,\n"
                    "        \"value\": %f,\n"
                    "        \"type\": \"DBL\",\n"
                    "        \"origin\": 1\n"
                    "    }]\n"
                    "}]\r\n",
                    cid, mid, time(NULL) * 1000, value
                    );
        printf("req: %s\n\n", data);

        strcpy(hi1.request.authorization, TOKEN);
        sprintf(endpoint, "https://%s:%d%s", ENV_DEV_NAME, PORT, PUT_SIGNALS_DATA_ENDPOINT);
        return_code = http_put_post("PUT", &hi1, endpoint, data, response, sizeof(response));
        printf("Status: %d\n", return_code);
        printf("return body: %s \n", response);
        printf("\n\nSleeping 1min...\n\n");
        sleep(60);
    }

	return 0;
}

