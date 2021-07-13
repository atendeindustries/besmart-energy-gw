/*
 * Copyright 2021 Atende Industries
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/msg.h>
#include <errno.h>
#include "../../libmeter/meterbasic.h"
#include "../../metersrv/metersrv/sensor.h"
#include "https.h"

/* config */
const uint16_t PORT = 443;
const char TIME_API[] = "213.188.196.246";
const char ENV_LOCAL[] = "127.0.0.1";
const char ENV_DEV[] = "127.0.0.1";
const char ENV_DEV_NAME[] = "localhost";
const char TOKEN[] = "";
const char METER_TYPE_NAME[] = "";
const char FIND_STATE_ENDPOINT[] = "";
const char PUT_SIGNALS_DATA_ENDPOINT[] = "";
// ------

void error(const char *msg) { 
    printf("%s\n", msg);
    exit(0);
}

static char ip[15];
static char endpoint[128];
static char data[1024];
static char response[512];
static char SERIAL[18];

struct sensorId {
    int client_cid;
    int sensor_mid;
};

unsigned long long getTime(HTTP_INFO* hi) {
    if(http_open(hi, TIME_API) < 0)
    {
        http_strerror(data, 1024);
        http_close(hi);
        error(data);
    }
    int result = http_get(hi, "http://worldtimeapi.org/api/timezone/Europe/Warsaw", response, sizeof(response));
    http_close(hi);
    char* timeptr = strstr(response, "unixtime") + 10;
    unsigned long long time = strtoull(timeptr, NULL, 0);
    return time;
}

struct sensorId identify(HTTP_INFO* hi) {
    sprintf(endpoint, "https://%s:%d%s?meter_dev=%s&meter_type_name=%s", ENV_DEV_NAME, PORT, FIND_STATE_ENDPOINT, SERIAL, METER_TYPE_NAME);
    printf("%s - ", endpoint);
    fflush(stdout);
    strcpy(hi->request.authorization, TOKEN);
    int return_code = http_get(hi, endpoint, response, sizeof(response));
    printf("%d\n", return_code);
    printf("response: %s\n", response);

    struct sensorId sid;
    char* client_cid = strstr(response, "client_cid");
    sid.client_cid = client_cid != NULL ? atoi(client_cid + 13) : 0;
    char* sensor_mid = strstr(response, "sensor_mid");
    sid.sensor_mid = sensor_mid != NULL ? atoi(sensor_mid + 13) : 0;
    
    return sid;
}

void open_connection(HTTP_INFO* hi, char* ip) {
    if(http_open(hi, ip) < 0)
    {
        http_strerror(data, 1024);
        http_close(hi);
        error(data);
    }
}

int main(int argc, char **argv)
{
    srand(time(NULL));

    if (argc < 2) {
        error("Usage: /bin/sockettest [env]");
    }

    msg_t msg;
    int res;
    oid_t oid;
    MeterBasicResult_t *result;
    meter_state_t state;

    if (lookup("/dev/metersrv", NULL, &oid) < 0) {
		printf("Cannot connect to metersrv\n");
		return EOK;
	}

    msg.type = 5;
    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        printf("Could not get meter status (%d)", res);
    }
    result = *(MeterBasicResult_t **)msg.o.raw;
    state = *(meter_state_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) + sizeof(MeterConf_t *));
    strcpy(SERIAL, state.serial);

    /* Setting host */

    char* env = argv[1];

    if (strcmp("local", env) == 0) {
        strcpy(ip, ENV_LOCAL);
    } else if (strcmp("dev", env) == 0) {
        strcpy(ip, ENV_DEV);
    } else {
        error("Invalid env. Available: local/dev");
    }

    int return_code;
    HTTP_INFO hi1;
    http_init(&hi1, FALSE);
    unsigned long long currentTimestamp = getTime(&hi1);

    msg.type = 7;
    memcpy(msg.o.raw, &currentTimestamp, sizeof(currentTimestamp));
    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        printf("Could not set meter time (%d)", res);
    }

    open_connection(&hi1, ip);
    struct sensorId sid = identify(&hi1);
    http_close(&hi1);

    if (sid.client_cid > 0 && sid.sensor_mid > 0) {
        printf("Identified (cid: %d, mid: %d)\n\n", sid.client_cid, sid.sensor_mid);
    } else {
        error("Couldn't identify meter.");
    }

    while (1) {
        msg.type = 1;
        unsigned int arg = 0;
        memcpy(msg.o.raw, &arg, sizeof(unsigned int));
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get info from metersrv (%d)", res);
            continue;
        }

        result = *(MeterBasicResult_t **)msg.o.raw;
        time_t timestamp = *(time_t *)(msg.o.raw +
            sizeof(MeterBasicResult_t *));
        res = *(uint8_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
            sizeof(time_t));

        sprintf(data,
                "[{\n"
                "    \"client_cid\": %d,\n"
                "    \"sensor_mid\": %d,\n"
                "    \"signal_type_moid\": 32,\n"
                "    \"data\": [{\n"
                "        \"time\": %llu,\n"
                "        \"value\": %f,\n"
                "        \"type\": \"DBL\",\n"
                "        \"origin\": 1\n"
                "    }]\n"
                "}]\r\n",
                sid.client_cid,
                sid.sensor_mid,
                timestamp * 1000,
                ((float)result->eactive_plus_sum.value) / 1000.0 / 3600.0
        );
        printf("req: %s\n\n", data);

        open_connection(&hi1, ip);
        strcpy(hi1.request.authorization, TOKEN);
        sprintf(endpoint, "https://%s:%d%s", ENV_DEV_NAME, PORT, PUT_SIGNALS_DATA_ENDPOINT);
        return_code = http_put_post("PUT", &hi1, endpoint, data, response, sizeof(response));
        http_close(&hi1);
        printf("Status: %d\n", return_code);
        printf("return body: %s \n", response);
        printf("\n\nSleeping 15m...\n\n");
        sleep(900);
    }

	return 0;
}
