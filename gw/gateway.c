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
#include "config.h"

void error(const char *msg) { 
    printf("%s\n", msg);
    exit(0);
}

static char ip[15];
static char endpoint[128];
static char data[1024];
static char response[512];
static char SERIAL[18];
static int PHASES;
static char METER_TYPE_NAME[6];

struct sensorId {
    int client_cid;
    int sensor_mid;
};

void open_connection(HTTP_INFO* hi, char* ip) {
    while (http_open(hi, ip) < 0)
    {
        sleep(5);
    }
}

unsigned long long getTime(HTTP_INFO* hi) {
    open_connection(hi, TIME_API);
    http_get(hi, "http://worldtimeapi.org/api/timezone/Europe/Warsaw", response, sizeof(response));
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

int sendMeasurement(HTTP_INFO* hi, int cid, int mid, int moid, time_t timestamp, float value) {
    int return_code;
    sprintf(data,
            "[{\n"
            "    \"client_cid\": %d,\n"
            "    \"sensor_mid\": %d,\n"
            "    \"signal_type_moid\": %d,\n"
            "    \"data\": [{\n"
            "        \"time\": %llu,\n"
            "        \"value\": %f,\n"
            "        \"type\": \"DBL\",\n"
            "        \"origin\": 1\n"
            "    }]\n"
            "}]\r\n",
            cid,
            mid,
            moid,
            timestamp,
            value
    );
    printf("req: %s\n\n", data);

    strcpy(hi->request.authorization, TOKEN);
    sprintf(endpoint, "https://%s:%d%s", ENV_DEV_NAME, PORT, PUT_SIGNALS_DATA_ENDPOINT);
    return_code = http_put_post("PUT", hi, endpoint, data, response, sizeof(response));

    printf("Status: %d\n", return_code);
    printf("return body: %s \n", response);

    return return_code;
}

unsigned long long getLastCap(HTTP_INFO* hi, int cid, int mid) {
    strcpy(hi->request.authorization, TOKEN);
    sprintf(endpoint, "https://%s:%d%s/%d.%d/cap", ENV_DEV_NAME, PORT, SENSORS_API, cid, mid);
    http_get(hi, endpoint, response, sizeof(response));
    
    char* p1 = strchr(response, '{');
    char* p2 = strchr(response, '}');
    unsigned long long last_timestamp = 0;
    while (last_timestamp == 0 && p1 != NULL) {
        *p2 = 0;
        if (strstr(response, "signal_origin_id\": 1") != NULL &&
            strstr(response, "signal_type_moid\": 32") != NULL) {
            
            p1 = strstr(response, "cap_max\":") + 10;
            last_timestamp = strtoull(p1, NULL, 0);
        } else {
            p1 = p2 + 1;
            p2 = strchr(p1, '}');
            if (p2 == NULL) p1 = NULL;
        }
    }
    return last_timestamp / 1000;
}

void fillGaps(HTTP_INFO* hi, oid_t* oid, int cid, int mid, unsigned long long current) {
    MeterBasicResult_t* result;
    time_t timestamp;
    unsigned long long lastCap = getLastCap(hi, cid, mid);
    int res;
    msg_t msg;
    current -= current % 900;
    lastCap += 900;
    while (lastCap <= current) {
        unsigned int diff = (int)(current - lastCap) / 900;
        msg.type = 1;
        memcpy(msg.o.raw, &diff, sizeof(unsigned int));
        if ((res = msgSend(oid->port, &msg)) < 0 || msg.o.io.err == -1) {
            return;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw +
            sizeof(MeterBasicResult_t *));
        sendMeasurement(hi, cid, mid, 32, timestamp * 1000, ((float)result->eactive_plus_sum.value) / 1000.0 / 3600.0);
        sendMeasurement(hi, cid, mid, 34, timestamp * 1000, ((float)result->eactive_minus_sum.value) / 1000.0 / 3600.0);
        sendMeasurement(hi, cid, mid, 44, timestamp * 1000, ((float)result->eapparent_plus_sum.value) / 1000.0 / 3600.0);
        sendMeasurement(hi, cid, mid, 46, timestamp * 1000, ((float)result->eapparent_minus_sum.value) / 1000.0 / 3600.0);

        lastCap += 900;
    }
}

int main(int argc, char **argv)
{
    srand(time(NULL));

    if (argc < 2) {
        error("Usage: /bin/sockettest [env]");
    }
    
    /* Setting host */

    char* env = argv[1];

    if (strcmp("local", env) == 0) {
        strcpy(ip, ENV_LOCAL);
    } else if (strcmp("dev", env) == 0) {
        strcpy(ip, ENV_DEV);
    } else {
        error("Invalid env. Available: local/dev");
    }

    msg_t msg;
    int res;
    unsigned int arg = 0;
    oid_t oid;
    MeterBasicResult_t *result;
    meter_state_t state;
    time_t timestamp;
    int result_code;

    while (lookup("/dev/metersrv", NULL, &oid) < 0) {
		sleep(1);
	}

    msg.type = 5;
    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        error("Could not get meter status");
    }
    result = *(MeterBasicResult_t **)msg.o.raw;
    state = *(meter_state_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) + sizeof(MeterConf_t *));
    strcpy(SERIAL, state.serial);

    if (SERIAL[5] == '3') {
        PHASES = 3;
        strcpy(METER_TYPE_NAME, "EM3Ph");
    } else {
        PHASES = 1;
        strcpy(METER_TYPE_NAME, "EM1Ph");
    }

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

    if (sid.client_cid > 0 && sid.sensor_mid > 0) {
        printf("Identified (cid: %d, mid: %d)\n\n", sid.client_cid, sid.sensor_mid);
    } else {
        error("Couldn't identify meter.");
    }

    fillGaps(&hi1, &oid, sid.client_cid, sid.sensor_mid, currentTimestamp);

    while (1) {
        msg.type = 5;
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get status from metersrv (%d)", res);
            sleep(1);
            continue;
        }

        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
            sizeof(MeterConf_t *) + sizeof(meter_state_t));
        // V1
        result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 53, timestamp * 1000, result->u_rms_avg[0]);
        // I1
        if (result_code > -1)
            result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 56, timestamp * 1000, result->i_rms_avg[0]);

        if (PHASES == 3) {
            // V2
            if (result_code > -1)
                result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 54, timestamp * 1000, result->u_rms_avg[0]);
            // I2
            if (result_code > -1)
                result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 57, timestamp * 1000, result->i_rms_avg[0]);
            // V3
            if (result_code > -1)
                result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 55, timestamp * 1000, result->u_rms_avg[0]);
            // I3
            if (result_code > -1)
                result_code = sendMeasurement(&hi1, sid.client_cid, sid.sensor_mid, 58, timestamp * 1000, result->i_rms_avg[0]);
        }

        if (result_code > -1 && (timestamp / 60) % 15 == 1) {
            msg.type = 1;
            memcpy(msg.o.raw, &arg, sizeof(unsigned int));
            if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                printf("Could not get info from metersrv (%d)", res);
                sleep(1);
                continue;
            }
            result = *(MeterBasicResult_t **)msg.o.raw;
            timestamp = *(time_t *)(msg.o.raw +
                sizeof(MeterBasicResult_t *));
            // A+
            result_code = sendMeasurement(
                &hi1, sid.client_cid, sid.sensor_mid, 32, timestamp * 1000,
                ((float)result->eactive_plus_sum.value) / 1000.0 / 3600.0
            );
            // A-
            if (result_code > -1)
                result_code = sendMeasurement(
                    &hi1, sid.client_cid, sid.sensor_mid, 34, timestamp * 1000,
                    ((float)result->eactive_minus_sum.value) / 1000.0 / 3600.0
                );
            // S+
            if (result_code > -1)
                result_code = sendMeasurement(
                    &hi1, sid.client_cid, sid.sensor_mid, 44, timestamp * 1000,
                    ((float)result->eapparent_plus_sum.value) / 1000.0 / 3600.0
                );
            // S-
            if (result_code > -1)
                result_code = sendMeasurement(
                    &hi1, sid.client_cid, sid.sensor_mid, 46, timestamp * 1000,
                    ((float)result->eapparent_minus_sum.value) / 1000.0 / 3600.0
                );
        }
        if (result_code == -1) {
            printf("Disconnected...\n");
            http_close(&hi1);
            open_connection(&hi1, ip);
            msg.type = 5;
            if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                printf("Could not get status from metersrv (%d)", res);
                sleep(1);
                continue;
            }

            result = *(MeterBasicResult_t **)msg.o.raw;
            currentTimestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
                sizeof(MeterConf_t *) + sizeof(meter_state_t));
            fillGaps(&hi1, &oid, sid.client_cid, sid.sensor_mid, currentTimestamp);
        }

        printf("\n\nSleeping 1m...\n\n");
        sleep(60);
    }

	return 0;
}
