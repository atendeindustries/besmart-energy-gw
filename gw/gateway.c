/*
 * Copyright 2022 Atende Industries
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
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/msg.h>

#include <imxrt-multi.h>


// TODO: fix building and remove this
void test() {
    socket(0,0,0);
    select(0, 0, 0, 0, 0);
}

const int CURRENT_RATIO = 1;
const int VOLTAGE_RATIO = 1;

static char endpoint[128];
static char data[200*8];
static char response[1280];
static char SERIAL[18];
static int PHASES;
static char METER_TYPE_NAME[6];
static unsigned long long stateSince;
static unsigned long long lastCap;

struct sensorId {
    int client_cid;
    int sensor_mid;
};

static void modemReset(void)
{
	oid_t oid;
	msg_t msg;
	multi_i_t *imsg = NULL;

	if (lookup("/dev/gpio4", NULL, &oid) < 0) {
		fprintf(stderr, "usbacm_powerReset: lookup failed\n");
		return;
	}

	msg.type = mtDevCtl;
	msg.i.data = NULL;
	msg.i.size = 0;
	msg.o.data = NULL;
	msg.o.size = 0;

	imsg = (multi_i_t *)msg.i.raw;

	imsg->id = oid.id;
	imsg->gpio.type = gpio_set_port;
	imsg->gpio.port.val = 0 << 18;
	imsg->gpio.port.mask = 1 << 18;

	msgSend(oid.port, &msg);
	sleep(1);
	imsg->gpio.port.val = 1 << 18;
	msgSend(oid.port, &msg);

	fprintf(stderr, "gateway: Modem not responding. Power down USB.\n");
}


/* Implementation of hardware entropy source function for Phoenix-RTOS imxrt1064 platform*/
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    msg_t msg = { 0 };
    oid_t oid;
    multi_i_t *imsg;
    int ret;

    if (lookup("/dev/trng", NULL, &oid) < 0) {
        fprintf(stderr, "gateway: Fail to open entropy source\n");
        return -1;
    }

    imsg = (multi_i_t *)msg.i.raw;
    imsg->id = oid.id;
    msg.type = mtRead;
    msg.o.size = len;
    msg.o.data = output;

    if ((ret = msgSend(oid.port, &msg)) < 0) {
        fprintf(stderr, "gateway: Fail to retrieve entropy source\n");
        return -1;
    }

    *olen = len;

    return 0;
}


void reopen_connection(HTTP_INFO* hi) {
    int ret;
    do {
        http_close(hi);
        ret = http_open(hi);
        if (ret < 0) sleep(5);
    } while (ret < 0);
}

void open_connection(HTTP_INFO* hi) {
    int ret = http_open(hi);
    if (ret < 0) {
        reopen_connection(hi);
    }
}

time_t getTime(HTTP_INFO* hi) {
    int ret;
    open_connection(hi);

    do {
        ret = http_get(hi, "/api/time?unit=s", response, sizeof(response));
        if (ret < 200 || ret >= 400) sleep(5);
        if (ret < 0) reopen_connection(hi);
    } while (ret < 200 || ret >= 400);
    http_close(hi);
    time_t time = strtoul(response, NULL, 0);
    if (DEBUG) printf("Returned unixtimestamp: %llu.\n", time);
    return time;
}

struct sensorId identify(HTTP_INFO* hi) {
    int res;

    sprintf(endpoint, "%s?meter_dev=%s&meter_type_name=%s", FIND_STATE_ENDPOINT, SERIAL, METER_TYPE_NAME);
    if (DEBUG) printf("%s - ", endpoint);
    fflush(stdout);

    open_connection(hi);
    res = http_get(hi, endpoint, response, sizeof(response));
    http_close(hi);
    if (DEBUG) printf("%d\nresponse: %s\n", res, response);

    struct sensorId sid;
    if (res < 0) {
        sid.client_cid = sid.sensor_mid = 0;
    } else {
        char* client_cid = strstr(response, "client_cid");
        sid.client_cid = client_cid != NULL ? atoi(client_cid + 13) : 0;
        char* sensor_mid = strstr(response, "sensor_mid");
        sid.sensor_mid = sensor_mid != NULL ? atoi(sensor_mid + 13) : 0;
        char* since = strstr(response, "since");
        stateSince = since != NULL ? strtoull(since + 8, NULL, 0) : 0;
    }
    return sid;
}

int addDataToRequest(char* data, int cid, int mid, int moid, time_t timestamp, float value) {
    return snprintf(data, 256,
        "{"
        "\"client_cid\":%d,"
        "\"sensor_mid\":%d,"
        "\"signal_type_moid\":%d,"
        "\"data\":{"
        "\"time\":[%llu],"
        "\"value\":[%f],"
        "\"type\":[\"DBL\"],"
        "\"origin\":[1]"
        "}"
        "},",
        cid,
        mid,
        moid,
        timestamp,
        value
    );
}

unsigned long long getLastCap(HTTP_INFO* hi, int cid, int mid) {
    int ret;
    sprintf(endpoint, "%s/%d.%d/signals/cap?signal_type_moid=32&signal_origin_id=1", SENSORS_API, cid, mid);
    ret = http_get(hi, endpoint, response, sizeof(response));
    while(ret < 0) {
        reopen_connection(hi);
        ret = http_get(hi, endpoint, response, sizeof(response));
    };
    
    char* p1 = strchr(response, '{');
    char* p2 = strchr(response, '}');
    unsigned long long lastTimestamp = 0;
    while (lastTimestamp == 0 && p1 != NULL) {
        *p2 = 0;
        if (strstr(response, "signal_origin_id\": 1") != NULL &&
            strstr(response, "signal_type_moid\": 32") != NULL) {
            
            p1 = strstr(response, "cap_max\":") + 10;
            lastTimestamp = strtoull(p1, NULL, 0);
            break;
        } else {
            p1 = p2 + 1;
            p2 = strchr(p1, '}');
            if (p2 == NULL) p1 = NULL;
        }
    }
    lastTimestamp = lastTimestamp > 0 ? lastTimestamp : stateSince;
    if (DEBUG) printf("Last cap: %llu\n", lastTimestamp);
    return lastTimestamp;
}

int sendRequest(HTTP_INFO* hi) {
    int return_code;
    if (DEBUG) printf("req: %s\n\n", data);

    return_code = http_put(hi, PUT_SIGNALS_DATA_ENDPOINT, data, response, sizeof(response));

    if (DEBUG) printf("Status: %d\n", return_code);
    if (DEBUG) printf("return body: %s \n", response);

    return return_code;
}

void prepareCurrentData(struct sensorId* sid, time_t timestamp, MeterBasicResult_t* result) {
    int offset = 0;
    data[offset++] = '[';

    offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 103, timestamp, result->frequency); // freq
    offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 53, timestamp, result->u_rms_avg[0] * VOLTAGE_RATIO); // V1
    offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 56, timestamp, result->i_rms_avg[0] * CURRENT_RATIO); // I1
    if (PHASES == 3) {
        offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 54, timestamp, result->u_rms_avg[1] * VOLTAGE_RATIO); // V2
        offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 57, timestamp, result->i_rms_avg[1] * CURRENT_RATIO); // I2
        offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 55, timestamp, result->u_rms_avg[2] * VOLTAGE_RATIO); // V3
        offset += addDataToRequest(data+offset, sid->client_cid, sid->sensor_mid, 58, timestamp, result->i_rms_avg[2] * CURRENT_RATIO); // I3
    }
    data[offset - 1] = ']';
    data[offset] = 0;
}

void prepareProfileData(int cid, int mid, time_t timestamp, MeterBasicResult_t* result) {
    int offset = 0;
    data[offset++] = '[';

    offset += addDataToRequest(data+offset, cid, mid, 32, timestamp,
        ((float)result->eactive_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 34, timestamp,
        ((float)result->eactive_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 44, timestamp,
        ((float)result->eapparent_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 46, timestamp,
        ((float)result->eapparent_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 36, timestamp,
        ((float)result->ereactive_sum[0].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 38, timestamp,
        ((float)result->ereactive_sum[1].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 40, timestamp,
        ((float)result->ereactive_sum[2].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);
    offset += addDataToRequest(data+offset, cid, mid, 42, timestamp,
        ((float)result->ereactive_sum[3].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO);

    data[offset - 1] = ']';
    data[offset] = 0;
}

void sendProfileData(HTTP_INFO* hi, oid_t* oid, int cid, int mid, unsigned long long current) {
    MeterBasicResult_t* result;
    time_t timestamp;
    unsigned long long tmp = lastCap / 1000;
    int res;
    msg_t msg;
    current -= current % 900;
    tmp -= tmp % 900;
    while (tmp <= current) {
        unsigned int diff = (int)(current - tmp) / 900;
        msg.type = 1;
        memcpy(msg.o.raw, &diff, sizeof(unsigned int));
        if ((res = msgSend(oid->port, &msg)) < 0 || msg.o.io.err == -1) {
            tmp += 900;
            continue;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw +
            sizeof(MeterBasicResult_t *));

        if (timestamp * 1000 <= lastCap) {
            if (DEBUG) printf("Timestamp in profile lower or equal to lastCap (-d %u)\n", diff);
            tmp += 900;
            continue;
        }
        prepareProfileData(cid, mid, timestamp * 1000, result);
        res = sendRequest(hi);
        if (res < 200) {
            http_close(hi);
            res = http_open(hi);
            if (res < 0) return;
            res = sendRequest(hi);
        }
        if (res >= 200 && res < 300) {
            lastCap = timestamp * 1000;
        }

        tmp += 900;
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
    struct sensorId sid;
    sid.client_cid = 0;
    sid.sensor_mid = 0;

    while (lookup("/dev/metersrv", NULL, &oid) < 0) {
        sleep(5);
    }

    HTTP_INFO hi;

    http_setup(&hi);
    http_set_host(&hi, API_NAME, PORT, 1);
    http_set_token(&hi, TOKEN);

    timestamp = getTime(&hi);
    msg.type = 7;
    memcpy(msg.o.raw, &timestamp, sizeof(timestamp));
    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        printf("Could not set meter time (%d)", res);
    }

    do {
        msg.type = 5;
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get status from metersrv (%d)", res);
            sleep(5);
            continue;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
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

        sid = identify(&hi);
        if (sid.client_cid == 0 && sid.sensor_mid == 0) {
            printf("Couldn't identify meter in besmart.energy. Waiting 1m...\n");
            sleep(60);
        }
    } while (sid.client_cid == 0 && sid.sensor_mid == 0);

    if (DEBUG) printf("Identified (cid: %d, mid: %d)\n\n", sid.client_cid, sid.sensor_mid);


    open_connection(&hi);
    lastCap = getLastCap(&hi, sid.client_cid, sid.sensor_mid);
    sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);
    http_close(&hi);

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

        res = http_open(&hi);
        if (res != 0) {
            http_close(&hi);
            modemReset();
            sleep(60);
            continue;
        }
        prepareCurrentData(&sid, timestamp * 1000, result);
        res = sendRequest(&hi);
        if (res < 0) {
            http_close(&hi);
            modemReset();
            sleep(60);
            continue;
        }

        if (res > -1 && (timestamp / 60) % 15 == 1) {
            sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);
        }

        http_close(&hi);

        if (DEBUG) printf("\n\nSleeping 1m...\n\n");
        sleep(60);
    }
    http_destroy(&hi);
    return 0;
}
