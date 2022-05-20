/*
 * Copyright 2022 Atende Industries
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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
#include <math.h>

#include <imxrt-multi.h>

#include "azure.h"
#include "ca_cert.h"

// TODO: fix building and remove this
void test(void) {
    socket(0,0,0);
    select(0, 0, 0, 0, 0);
}

static char endpoint[128];
static char data[230 * 16];
unsigned int dataOffset = 0;
static char response[1280];
static char SERIAL[18];
static int PHASES;
static char METER_TYPE_NAME[6];
static unsigned long long stateSince;
static unsigned long long lastCap;
static int decimal;
static int fraction;
static int fractionLen;
static bool connectionOpened = 0;

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

void reOpenConnection(HTTP_INFO *hi) {
    int ret;
    unsigned int count = 1;

    connectionOpened = 0;

    do {
        http_close(hi);
        if (count % 10 == 0) {
            modemReset();
            sleep(30);
        }
        ret = http_open(hi);
        if (ret < 0) sleep(5);
        count++;
    } while (ret < 0);

    connectionOpened = 1;
}

void openConnection(HTTP_INFO *hi) {
    if (!connectionOpened) {
        int ret = http_open(hi);

        if (ret < 0) {
            reOpenConnection(hi);
        }
        else {
            connectionOpened = 1;
        }
    }
}

void closeConnection(HTTP_INFO *hi) {
    if (connectionOpened) {
        http_close(hi);
        connectionOpened = 0;
    }
}

time_t getTime(HTTP_INFO *hi) {
    int ret;

    do {
        ret = http_get(hi, "/api/time?unit=s", response, sizeof(response));
        if (ret < 200 || ret >= 400) sleep(5);
        if (ret < 0) reOpenConnection(hi);
    } while (ret < 200 || ret >= 400);
    time_t time = strtoul(response, NULL, 0);
    if (DEBUG) printf("Returned unixtimestamp: %llu.\n", time);
    return time;
}

struct sensorId identify(HTTP_INFO *hi) {
    int res;

    sprintf(endpoint, "%s?meter_dev=%s&meter_type_name=%s", FIND_STATE_ENDPOINT, SERIAL, METER_TYPE_NAME);
    if (DEBUG) printf("%s - ", endpoint);
    fflush(stdout);

    res = http_get(hi, endpoint, response, sizeof(response));
    if (res < 0) reOpenConnection(hi);
    if (DEBUG) printf("%d\nresponse: %s\n", res, response);

    struct sensorId sid;
    if (res < 0) {
        sid.client_cid = sid.sensor_mid = 0;
    } 
    else {
        char *client_cid = strstr(response, "client_cid");
        sid.client_cid = client_cid != NULL ? atoi(client_cid + 13) : 0;
        char *sensor_mid = strstr(response, "sensor_mid");
        sid.sensor_mid = sensor_mid != NULL ? atoi(sensor_mid + 13) : 0;
        char *since = strstr(response, "since");
        stateSince = since != NULL ? strtoull(since + 8, NULL, 0) : 0;
    }
    return sid;
}

int count_digits_of_integer(int integer) {
    int count = 1;
    int limit;

    while (1) {
        limit = pow(10, count);
        if (integer < limit) break;
        count++;
    }

    return count;
}

void addDataToRequest(int cid, int mid, int moid, time_t timestamp, double value) {
    decimal = (int)value;
    fraction = (int)round((value - ((int)value)) * 100000000);
    fractionLen =  8 - count_digits_of_integer(fraction);

    dataOffset += snprintf(data + dataOffset, 140,
        "{"
        "\"client_cid\":%d,"
        "\"sensor_mid\":%d,"
        "\"signal_type_moid\":%d,"
        "\"data\":{"
        "\"time\":[%llu],"
        "\"value\":[%d.",
        cid,
        mid,
        moid,
        timestamp,
        decimal
    );
    while (fractionLen-- != 0) {
        dataOffset += snprintf(data + dataOffset, 2, "0");
    }
    dataOffset += snprintf(data + dataOffset, 80,
        "%d],"
        "\"type\":[\"DBL\"],"
        "\"origin\":[1]"
        "}"
        "},",
        fraction
    );
}

unsigned long long getLastCap(HTTP_INFO *hi, int cid, int mid) {
    int ret;
    sprintf(endpoint, "%s/%d.%d/signals/cap?signal_type_moid=32&signal_origin_id=1", SENSORS_API, cid, mid);
    ret = http_get(hi, endpoint, response, sizeof(response));
    while(ret < 0) {
        reOpenConnection(hi);
        ret = http_get(hi, endpoint, response, sizeof(response));
    };

    char *p1 = strchr(response, '{');
    char *p2 = strchr(response, '}');
    unsigned long long lastTimestamp = 0;
    while (lastTimestamp == 0 && p1 != NULL) {
        *p2 = 0;
        if (strstr(response, "signal_origin_id\": 1") != NULL &&
            strstr(response, "signal_type_moid\": 32") != NULL) {

            p1 = strstr(response, "cap_max\":") + 10;
            lastTimestamp = strtoull(p1, NULL, 0);
            break;
        } 
        else {
            p1 = p2 + 1;
            p2 = strchr(p1, '}');
            if (p2 == NULL) p1 = NULL;
        }
    }
    lastTimestamp = lastTimestamp > 0 ? lastTimestamp : stateSince;
    if (DEBUG) printf("Last cap: %llu\n", lastTimestamp);
    return lastTimestamp;
}

int sendRequest(HTTP_INFO *hi) {
    int return_code;
    if (DEBUG) printf("req: %s\n\n", data);

    return_code = http_put(hi, PUT_SIGNALS_DATA_ENDPOINT, data, response, sizeof(response));

    if (DEBUG || return_code < 200 || return_code >= 300) {
        if (!DEBUG) printf("req: %s\n\n", data);
        printf("status: %d\n", return_code);
        printf("return body: %s \n", response);
    }

    return return_code;
}

void sendData(HTTP_INFO *hi, time_t timestamp, int *res) {
    *res = sendRequest(hi);
    if (*res < 200) {
        reOpenConnection(hi);
        return;
    }
    else if (*res >= 200 && *res < 300) {
        lastCap = timestamp * 1000;
    }
}

void prepareCurrentData(struct sensorId *sid, time_t timestamp, MeterBasicResult_t *result) {
    addDataToRequest(sid->client_cid, sid->sensor_mid, 103, timestamp, (double)(result->frequency)); // freq
    addDataToRequest(sid->client_cid, sid->sensor_mid, 53, timestamp, (double)(result->u_rms_avg[0] * VOLTAGE_RATIO)); // V1
    addDataToRequest(sid->client_cid, sid->sensor_mid, 56, timestamp, (double)(result->i_rms_avg[0] * CURRENT_RATIO)); // I1
    if (PHASES == 3) {
        addDataToRequest(sid->client_cid, sid->sensor_mid, 54, timestamp, (double)(result->u_rms_avg[1] * VOLTAGE_RATIO)); // V2
        addDataToRequest(sid->client_cid, sid->sensor_mid, 57, timestamp, (double)(result->i_rms_avg[1] * CURRENT_RATIO)); // I2
        addDataToRequest(sid->client_cid, sid->sensor_mid, 55, timestamp, (double)(result->u_rms_avg[2] * VOLTAGE_RATIO)); // V3
        addDataToRequest(sid->client_cid, sid->sensor_mid, 58, timestamp, (double)(result->i_rms_avg[2] * CURRENT_RATIO)); // I3
    }
}

void prepareEnergyData(int cid, int mid, time_t timestamp, MeterBasicResult_t *result) {
    addDataToRequest(cid, mid, 32, timestamp,
        (double)(((double)result->eactive_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 34, timestamp,
        (double)(((double)result->eactive_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 44, timestamp,
        (double)(((double)result->eapparent_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 46, timestamp,
        (double)(((double)result->eapparent_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 36, timestamp,
        (double)(((double)result->ereactive_sum[0].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 38, timestamp,
        (double)(((double)result->ereactive_sum[1].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 40, timestamp,
        (double)(((double)result->ereactive_sum[2].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(cid, mid, 42, timestamp,
        (double)(((double)result->ereactive_sum[3].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
}

void startDataBlock(void) {
    dataOffset = 0;
    data[dataOffset++] = '[';
}

void endDataBlock(void) {
    data[dataOffset - 1] = ']';
    data[dataOffset] = 0;
    dataOffset = 0;
}

void sendProfileData(HTTP_INFO *hi, oid_t *oid, int cid, int mid, unsigned long long current) {
    MeterBasicResult_t *result;
    time_t timestamp;
    unsigned long long tmp = lastCap / 1000;
    int res;
    msg_t msg;
    unsigned int diff;

    current -= current % ((int)METER_PROFILE_FREQ_S);
    tmp -= tmp % ((int)METER_PROFILE_FREQ_S);

    while (tmp <= current) {
        diff = (current - tmp) / ((int)METER_PROFILE_FREQ_S);
        msg.type = 1;
        memcpy(msg.o.raw, &diff, sizeof(unsigned int));
        if ((res = msgSend(oid->port, &msg)) < 0 || msg.o.io.err == -1) {
            tmp += (int)METER_PROFILE_FREQ_S;
            continue;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw +
            sizeof(MeterBasicResult_t *));

        if (timestamp * 1000 <= lastCap) {
            if (DEBUG) printf("Timestamp in profile lower or equal to lastCap (-d %u)\n", diff);
            tmp += (int)METER_PROFILE_FREQ_S;
            continue;
        }
        startDataBlock();
        prepareEnergyData(cid, mid, timestamp * 1000, result);
        endDataBlock();
        sendData(hi, timestamp, &res);

        tmp += (int)METER_PROFILE_FREQ_S;
    }
}

void calculateWaitUS(struct timespec t1, struct timespec t2, unsigned int freq, long long *diff) {
    *diff = (freq * 1000000 - (long long)((t2.tv_nsec - t1.tv_nsec) / 1000) - (long long)(t2.tv_sec - t1.tv_sec) * 1000000);
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
    time_t newTimestamp;
    struct sensorId sid;
    sid.client_cid = 0;
    sid.sensor_mid = 0;
    unsigned int freq = 1;
	struct timespec start;
    struct timespec finish;
    long long wait_us;
    long long lastTimeSync = 0;

    HTTP_INFO hi;
    IOTHUB_DEVICE_CLIENT_LL_HANDLE devhandle;

    // Find highest common frequency
    for(unsigned int gcd = 1; gcd <= ENERGY_FREQ_S && gcd <= CURRENT_FREQ_S; ++gcd) {
        if (ENERGY_FREQ_S % gcd == 0 && CURRENT_FREQ_S % gcd == 0)
            freq = gcd;
    }

    while (lookup("/dev/metersrv", NULL, &oid) < 0) {
        sleep(5);
    }

    if (SEND_TO_BESMART_ENERGY) {
        http_setup(&hi);
        http_set_host(&hi, API_NAME, PORT, 1);
        http_set_token(&hi, TOKEN);
        openConnection(&hi);
        timestamp = getTime(&hi);
    }

    if (SEND_TO_AZURE_IOTHUB) {
        /* Temporary solution, where time is set by passing current epoch in the first argument */
        timestamp = strtoull(argv[1], NULL, 10);
        azure_init();
        azure_open(AZURE_CONNECTION_STRING, ca_crt_rsa_azure, &devhandle);
    }

    msg.type = 7;
    memcpy(msg.o.raw, &timestamp, sizeof(timestamp));
    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
        printf("Could not set meter time (%d)\n", res);
    } 
    else {
        lastTimeSync = (long long)timestamp;
    }

    do {
        msg.type = 5;
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get status from metersrv (%d)\n", res);
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

        if (SEND_TO_BESMART_ENERGY)
            sid = identify(&hi);
        /* TODO: add identifying azure iothub devices */
        if (SEND_TO_AZURE_IOTHUB) {
            sid.client_cid = 1;
            sid.sensor_mid = 1;
        }

        if (sid.client_cid == 0 && sid.sensor_mid == 0) {
            printf("Couldn't identify meter in besmart.energy. Waiting 1m...\n");
            sleep(60);
        }
    } while (sid.client_cid == 0 && sid.sensor_mid == 0);

    if (DEBUG) printf("Identified (cid: %d, mid: %d)\n\n", sid.client_cid, sid.sensor_mid);
     /* TODO: update data for azure part */
    if (SEND_TO_BESMART_ENERGY) {
         lastCap = getLastCap(&hi, sid.client_cid, sid.sensor_mid);
         sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);
    }

    while (1) {
        clock_gettime(CLOCK_REALTIME, &start);
        if (SEND_TO_BESMART_ENERGY)
            openConnection(&hi);

        msg.type = 5;
        if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
            printf("Could not get status from metersrv (%d)\n", res);
            sleep(30);
            continue;
        }
        result = *(MeterBasicResult_t **)msg.o.raw;
        timestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
            sizeof(MeterConf_t *) + sizeof(meter_state_t));
        /* reopen connection to Azure IoTHub */
        if ((SEND_TO_AZURE_IOTHUB) && (((long long)timestamp - lastTimeSync) >= RECONNECT_FREQ_SEC)) {
            if (DEBUG) printf("\nRe-opening connection...\n");
            azure_close(&devhandle);
            azure_open(AZURE_CONNECTION_STRING, ca_crt_rsa_azure, &devhandle);
            lastTimeSync = (long long)timestamp;
        }
        /* TODO: add synchronization for azure part */
        if (SEND_TO_BESMART_ENERGY) {
            if ((long long)timestamp - lastTimeSync >= ((int)SYNC_TIME_FREQ_MIN) * 60) {
                if (DEBUG) printf("Checking time sync\n");
                newTimestamp = getTime(&hi);

                msg.type = 5;
                if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                    printf("Could not get status from metersrv (%d)\n", res);
                    sleep(30);
                    continue;
                }
                result = *(MeterBasicResult_t **)msg.o.raw;
                timestamp = *(time_t *)(msg.o.raw + sizeof(MeterBasicResult_t *) +
                    sizeof(MeterConf_t *) + sizeof(meter_state_t));

                if (abs((long long)newTimestamp - (long long)timestamp) >= (int)SYNC_TIME_TH_S) {
                    if (DEBUG) printf("Syncing time, old time: %llu, new time: %llu\n", (long long)timestamp, (long long)newTimestamp);
                    msg.type = 7;
                    memcpy(msg.o.raw, &newTimestamp, sizeof(newTimestamp));

                    if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                        printf("Could not set meter time (%d)\n", res);
                    }
                    else {
                        timestamp = newTimestamp;
                    }
                }
                lastTimeSync = (long long)timestamp;
            }
            if (timestamp - (int)(lastCap / 1000) > (int)METER_PROFILE_FREQ_S) {
                sendProfileData(&hi, &oid, sid.client_cid, sid.sensor_mid, timestamp);
            }
        }

        if (timestamp % ((int)CURRENT_FREQ_S) == 0 || timestamp % ((int)ENERGY_FREQ_S) == 0) {
            startDataBlock();

            if (timestamp % ((int)CURRENT_FREQ_S) == 0) {
                prepareCurrentData(&sid, timestamp * 1000, result);
            }

            if (timestamp % ((int)ENERGY_FREQ_S) == 0) {
                msg.type = 8;
                if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                    printf("Could not get data from metersrv (%d)\n", res);
                    sleep(30);
                    continue;
                }
                result = *(MeterBasicResult_t **)msg.o.raw;

                prepareEnergyData(sid.client_cid, sid.sensor_mid, timestamp * 1000, result);
            }

            endDataBlock();
            printf("Prepared data block: %s\n", data);
            if (SEND_TO_BESMART_ENERGY)
                sendData(&hi, timestamp, &res);
            if (SEND_TO_AZURE_IOTHUB)
                azure_sendMsg(&devhandle, data);
        }

	    clock_gettime(CLOCK_REALTIME, &finish);

        // Wait to get desired frequency and align to closest time
        calculateWaitUS(start, finish, freq, &wait_us);
        wait_us = wait_us - (timestamp % freq) * 1000000;

        if (wait_us > 0) {
            if (DEBUG) printf("\nSleeping %dms...\n", (int)(wait_us / 1000));

            usleep(wait_us);
        }
        else {
            if (DEBUG) printf("\nNot sleeping.\n");
        }
    }
    if (SEND_TO_BESMART_ENERGY)
        http_destroy(&hi);
    if (SEND_TO_AZURE_IOTHUB) {
        azure_close(&devhandle);
        azure_deinit();
    }

    return 0;
}
