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


static char endpoint[128];
static char data[1280];
static char response[1280];
static char SERIAL[18];
static int PHASES;
static char METER_TYPE_NAME[6];

struct sensorId {
    int client_cid;
    int sensor_mid;
};

time_t getTime(HTTP_INFO* hi) {
    int ret;
    http_set_host(hi, TIME_API_NAME, "80", 0);
    http_set_token(hi, "");
    do {
        sleep(5);
        ret = http_get(hi, "/api/timezone/Europe/Warsaw", response, sizeof(response));
    } while (ret < 200 || ret >= 400);
    char* timeptr = strstr(response, "unixtime") + 10;
    time_t time = strtoul(timeptr, NULL, 0);
    if (DEBUG) printf("Returned unixtimestamp: %llu\n", time);
    return time;
}

struct sensorId identify(HTTP_INFO* hi) {
    sprintf(endpoint, "%s?meter_dev=%s&meter_type_name=%s", FIND_STATE_ENDPOINT, SERIAL, METER_TYPE_NAME);
    if (DEBUG) printf("%s - ", endpoint);
    fflush(stdout);
    int return_code = http_get(hi, endpoint, response, sizeof(response));
    if (DEBUG) printf("%d\nresponse: %s\n", return_code, response);

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
            "[{"
            "\"client_cid\":%d,"
            "\"sensor_mid\":%d,"
            "\"signal_type_moid\":%d,"
            "\"data\":{"
            "\"time\":[%llu],"
            "\"value\":[%f],"
            "\"type\":[\"DBL\"],"
            "\"origin\":[1]"
            "}"
            "}]",
            cid,
            mid,
            moid,
            timestamp,
            value
    );
    if (DEBUG) printf("req: %s\n\n", data);

    do {
        return_code = http_put(hi, PUT_SIGNALS_DATA_ENDPOINT, data, response, sizeof(response));
        sleep(2);
    } while (return_code < 0);

    if (DEBUG) printf("Status: %d\n", return_code);
    if (DEBUG) printf("return body: %s \n", response);

    return return_code;
}

unsigned long long getLastCap(HTTP_INFO* hi, int cid, int mid) {
    int ret;
    sprintf(endpoint, "%s/%d.%d/cap", SENSORS_API, cid, mid);
    ret = http_get(hi, endpoint, response, sizeof(response));
    while(ret < 0) {
        sleep(1);
        ret = http_get(hi, endpoint, response, sizeof(response));
    };
    
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
    if (DEBUG) printf("Last cap: %llu\n", last_timestamp);
    return last_timestamp / 1000;
}

void sendProfileData(HTTP_INFO* hi, oid_t* oid, int cid, int mid, unsigned long long current) {
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
            if (DEBUG) printf("Could not fill gap (-d %u)\n", diff);
            lastCap += 900;
            continue;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw +
            sizeof(MeterBasicResult_t *));
        if (timestamp < lastCap) {
            if (DEBUG) printf("Bad time in profile (-d %u)\n", diff);
            lastCap += 900;
            continue;
        }
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

    msg_t msg;
    int res;
    oid_t oid;
    MeterBasicResult_t *result;
    meter_state_t state;
    time_t timestamp;
    int result_code;

    while (lookup("/dev/metersrv", NULL, &oid) < 0) {
        sleep(5);
    }

    msg.type = 5;
    while ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        printf("Could not get meter status...\n");
        sleep(15);
    }
    timestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
        sizeof(MeterConf_t *) + sizeof(meter_state_t));
    state = *(meter_state_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) + sizeof(MeterConf_t *));
    strcpy(SERIAL, state.serial);

    if (SERIAL[5] == '3') {
        PHASES = 3;
        strcpy(METER_TYPE_NAME, "EM3Ph");
    } else {
        PHASES = 1;
        strcpy(METER_TYPE_NAME, "EM1Ph");
    }

    HTTP_INFO hi;
    timestamp = getTime(&hi);
    if (timestamp > 1e9) {
        msg.type = 7;
        memcpy(msg.o.raw, &timestamp, sizeof(timestamp));
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not set meter time (%d)", res);
        }
    }

    http_setup(&hi);
    http_set_host(&hi, API_NAME, PORT, 1);
    http_set_token(&hi, TOKEN);
    struct sensorId sid = identify(&hi);
    while (sid.client_cid == 0 && sid.sensor_mid == 0) {
        printf("Couldn't identify meter in besmart.energy. Waiting 5m...\n");
        sleep(300);
        sid = identify(&hi);
    }
    if (DEBUG) printf("Identified (cid: %d, mid: %d)\n\n", sid.client_cid, sid.sensor_mid);

    sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);

    while (1) {
        msg.type = 5;
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get status from metersrv (%d)", res);
            sleep(60);
            continue;
        }

        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
            sizeof(MeterConf_t *) + sizeof(meter_state_t));
        // V1
        result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 53, timestamp * 1000, result->u_rms_avg[0]);
        // I1
        if (result_code > -1)
            result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 56, timestamp * 1000, result->i_rms_avg[0]);

        if (PHASES == 3) {
            // V2
            if (result_code > -1)
                result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 54, timestamp * 1000, result->u_rms_avg[0]);
            // I2
            if (result_code > -1)
                result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 57, timestamp * 1000, result->i_rms_avg[0]);
            // V3
            if (result_code > -1)
                result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 55, timestamp * 1000, result->u_rms_avg[0]);
            // I3
            if (result_code > -1)
                result_code = sendMeasurement(&hi, sid.client_cid, sid.sensor_mid, 58, timestamp * 1000, result->i_rms_avg[0]);
        }
        if (result_code > -1 && (timestamp / 60) % 15 == 1) {
            sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);
        }

        if (DEBUG) printf("\n\nSleeping 1m...\n\n");
        sleep(58);
    }
    http_destroy(&hi);
    return 0;
}
