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
#include <math.h>

#include <imxrt-multi.h>
#include <phoenix/arch/imxrt.h>
#include <sys/platform.h>
#include <termios.h>
#include <fcntl.h>

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

typedef struct sensorId {
    unsigned long client_cid;
    unsigned long long sensor_mid;
} sensor_id;


static int gpio_setPin(oid_t oid, int pin, int state)
{
    msg_t msg;
    multi_i_t *imsg = NULL;
    int res = EOK;

    msg.type = mtDevCtl;
    msg.i.data = NULL;
    msg.i.size = 0;
    msg.o.data = NULL;
    msg.o.size = 0;

    imsg = (multi_i_t *)msg.i.raw;

    imsg->id = oid.id;
    imsg->gpio.type = gpio_set_port;
    imsg->gpio.port.val = !!state << pin;
    imsg->gpio.port.mask = 1 << pin;

    if ((res = msgSend(oid.port, &msg)) < 0)
        return res;

    return res;
}


static int gpio_setDir(oid_t oid, int pin, int dir)
{
    msg_t msg;
    multi_i_t *imsg = NULL;
    int res = EOK;

    msg.type = mtDevCtl;
    msg.i.data = NULL;
    msg.i.size = 0;
    msg.o.data = NULL;
    msg.o.size = 0;

    imsg = (multi_i_t *)msg.i.raw;

    imsg->id = oid.id;
    imsg->gpio.type = gpio_set_dir;
    imsg->gpio.dir.val = !!dir << pin;
    imsg->gpio.dir.mask = 1 << pin;

    if ((res = msgSend(oid.port, &msg)) < 0)
        return res;

    return res;
}


static int gpio_configMux(int mux, int sion, int mode)
{
    platformctl_t pctl;

    pctl.action = pctl_set;
    pctl.type = pctl_iomux;

    pctl.iomux.mux = mux;
    pctl.iomux.sion = sion;
    pctl.iomux.mode = mode;

    return platformctl(&pctl);
}

#if CONNECTION_MODE == MODE_WIFI

static int serial_open(const char *devname, speed_t speed)
{
    oid_t oid;
    int fd, ret, cnt;
    struct termios tio;

    /* try if uart is registered */
    for (cnt = 0; (ret = lookup(devname, NULL, &oid)) < 0; cnt++) {
        usleep(200 * 1000);
        if (cnt > 3) {
            return ret;
        }
    }

    if ((fd = open(devname, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0) {
        return fd;
    }

    memset(&tio, 0, sizeof(tio));

    if ((ret = tcgetattr(fd, &tio)) < 0) {
        goto on_error;
    }

    if ((ret = cfsetspeed(&tio, speed)) < 0) {
        goto on_error;
    }

    tio.c_cc[VTIME] = 0; /* no timeout */
    tio.c_cc[VMIN] = 0;  /* polling */

    /* libtty does not support yet: IXON|IXOFF|IXANY|PARMRK|INPCK|IGNPAR */
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(CSIZE | CSTOPB);

    tio.c_cflag |= CS8 | CREAD | CLOCAL;

    if ((ret = tcflush(fd, TCIOFLUSH)) < 0) {
        goto on_error;
    }

    if ((ret = tcsetattr(fd, TCSANOW, &tio)) < 0) {
        goto on_error;
    }

on_error:
    close(fd);
    return ret;
}

#elif CONNECTION_MODE == MODE_GSM

static void modemReset(void)
{
    oid_t oid;

    if (lookup("/dev/gpio4", NULL, &oid) < 0) {
        return;
    }

    /* Reset USB port */
    gpio_setPin(oid, 18, 0);
	sleep(1);
    gpio_setPin(oid, 18, 1);

    fprintf(stderr, "gateway: Modem not responding. Power down USB.\n");
}
#endif /* CONNECTION_MODE */


static void platform_config(void)
{
    oid_t oid;

    while (lookup("/dev/gpio4", NULL, &oid) < 0) {
        sleep(1);
    }

#if CONNECTION_MODE == MODE_WIFI
    /* Power-up uart to enable wi-fi module communication */
    gpio_setDir(oid, 17, 1);
    gpio_setPin(oid, 17, 1);
    gpio_configMux(pctl_mux_gpio_emc_17, 5, 5);

    /* Configure uart to respond do AT commands issued by the pppos driver */
	while (serial_open("/dev/uart3", B115200) < 0) {
        sleep(1);
    }

#elif CONNECTION_MODE == MODE_GSM
    /* Power-up USB port to enable GSM communication */
    gpio_setDir(oid, 18, 1);
    gpio_setPin(oid, 18, 1);
    gpio_configMux(pctl_mux_gpio_emc_18, 5, 5);
#endif /* CONNECTION_MODE */
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
#if CONNECTION_MODE == MODE_GSM
            modemReset();
#endif /* CONNECTION_MODE */
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

sensor_id identify(HTTP_INFO *hi) {
    int res;

    sprintf(endpoint, "%s?meter_dev=%s&meter_type_name=%s", FIND_STATE_ENDPOINT, SERIAL, METER_TYPE_NAME);
    if (DEBUG) printf("%s - ", endpoint);
    fflush(stdout);

    res = http_get(hi, endpoint, response, sizeof(response));
    if (res < 0) reOpenConnection(hi);
    if (DEBUG) printf("%d\nresponse: %s\n", res, response);

    sensor_id sid;
    sid.client_cid = sid.sensor_mid = 0;
    if (res < 0) {
        return sid;
    } 

    char *client_cid_pos = strstr(response, "client_cid\"");
    char *sensor_mid_pos = strstr(response, "sensor_mid\"");
    if (client_cid_pos != NULL && sensor_mid_pos != NULL) {
        sscanf(client_cid_pos, "%*[^0-9]%lu", &sid.client_cid);
        sscanf(sensor_mid_pos, "%*[^0-9]%llu", &sid.sensor_mid);
    }
    char *since_pos = strstr(response, "since\"");
    if (since_pos != NULL) {
        sscanf(since_pos, "%*[^0-9]%llu", &stateSince);
    }
    else {
        stateSince = 0;
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

void addDataToRequest(sensor_id *sid, long moid, time_t timestamp, double value) {
    decimal = (int)value;
    fraction = (int)round((value - ((int)value)) * 100000000);
    fractionLen =  8 - count_digits_of_integer(fraction);

    dataOffset += snprintf(data + dataOffset, 140,
        "{"
        "\"client_cid\":%ld,"
        "\"sensor_mid\":%lld,"
        "\"signal_type_moid\":%ld,"
        "\"data\":{"
        "\"time\":[%llu],"
        "\"value\":[%d.",
        sid->client_cid,
        sid->sensor_mid,
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

unsigned long long getLastCap(HTTP_INFO *hi, sensor_id *sid) {
    int ret;
    sprintf(endpoint, "%s/%ld.%lld/signals/cap?signal_type_moid=32&signal_origin_id=1", SENSORS_API, sid->client_cid, sid->sensor_mid);
    ret = http_get(hi, endpoint, response, sizeof(response));
    while(ret < 0) {
        reOpenConnection(hi);
        ret = http_get(hi, endpoint, response, sizeof(response));
    };

    unsigned long long lastTimestamp = 0;
    const char* capMaxPos = strstr(response, "cap_max\"");

    if (capMaxPos != NULL) {
        sscanf(capMaxPos, "%*[^0-9]%llu", &lastTimestamp);
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

void sendData(HTTP_INFO *hi, time_t timestamp, int *res, bool updateLastCap) {
    *res = sendRequest(hi);
    if (*res < 200) {
        reOpenConnection(hi);
        return;
    }
    else if (updateLastCap && *res >= 200 && *res < 300) {
        lastCap = timestamp * 1000;
    }
}

void prepareCurrentData(sensor_id *sid, time_t timestamp, MeterBasicResult_t *result) {
    addDataToRequest(sid, 103, timestamp, (double)(result->frequency)); // freq
    addDataToRequest(sid, 53, timestamp, (double)(result->u_rms_avg[0] * VOLTAGE_RATIO)); // V1
    addDataToRequest(sid, 56, timestamp, (double)(result->i_rms_avg[0] * CURRENT_RATIO)); // I1
    if (PHASES == 3) {
        addDataToRequest(sid, 54, timestamp, (double)(result->u_rms_avg[1] * VOLTAGE_RATIO)); // V2
        addDataToRequest(sid, 57, timestamp, (double)(result->i_rms_avg[1] * CURRENT_RATIO)); // I2
        addDataToRequest(sid, 55, timestamp, (double)(result->u_rms_avg[2] * VOLTAGE_RATIO)); // V3
        addDataToRequest(sid, 58, timestamp, (double)(result->i_rms_avg[2] * CURRENT_RATIO)); // I3
    }
}

void prepareEnergyData(sensor_id *sid, time_t timestamp, MeterBasicResult_t *result) {
    addDataToRequest(sid, 32, timestamp,
        (double)(((double)result->eactive_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 34, timestamp,
        (double)(((double)result->eactive_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 44, timestamp,
        (double)(((double)result->eapparent_plus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 46, timestamp,
        (double)(((double)result->eapparent_minus_sum.value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 36, timestamp,
        (double)(((double)result->ereactive_sum[0].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 38, timestamp,
        (double)(((double)result->ereactive_sum[1].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 40, timestamp,
        (double)(((double)result->ereactive_sum[2].value) / 1000.0 / 3600.0 * CURRENT_RATIO * VOLTAGE_RATIO));
    addDataToRequest(sid, 42, timestamp,
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

void sendProfileData(HTTP_INFO *hi, oid_t *oid, sensor_id *sid, unsigned long long current) {
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
        prepareEnergyData(sid, timestamp * 1000, result);
        endDataBlock();
        sendData(hi, timestamp, &res, 1);

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
    sensor_id sid;
    sid.client_cid = 0;
    sid.sensor_mid = 0;
    unsigned int freq = 1;
	struct timespec start;
    struct timespec finish;
    long long wait_us;
    long long lastTimeSync = 0;
    bool isEnergyFreq = 0;
    bool isCurrentFreq = 0;

    platform_config();

    // Find highest common frequency
    for(unsigned int gcd = 1; gcd <= ENERGY_FREQ_S && gcd <= CURRENT_FREQ_S; ++gcd) {
        if (ENERGY_FREQ_S % gcd == 0 && CURRENT_FREQ_S % gcd == 0)
            freq = gcd;
    }

    while (lookup("/dev/metersrv", NULL, &oid) < 0) {
        sleep(5);
    }

    HTTP_INFO hi;

    http_setup(&hi);
    http_set_host(&hi, API_NAME, PORT, 1);
    http_set_token(&hi, TOKEN);

    openConnection(&hi);
    timestamp = getTime(&hi);

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

        sid = identify(&hi);
        if (sid.client_cid == 0 && sid.sensor_mid == 0) {
            printf("Couldn't identify meter in besmart.energy. Waiting 1m...\n");
            sleep(60);
        }
    } while (sid.client_cid == 0 && sid.sensor_mid == 0);

    if (DEBUG) printf("Identified (cid: %ld, mid: %lld)\n\n", sid.client_cid, sid.sensor_mid);

    lastCap = getLastCap(&hi, &sid);
    sendProfileData(&hi, &oid, &sid, timestamp);

    while (1) {
	    clock_gettime(CLOCK_REALTIME, &start);

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

        if ((long long)timestamp - lastTimeSync >= ((int)SYNC_FREQ_MIN) * 60) {
            if (DEBUG) printf("Checking sensor identification\n");

            do {
                sid = identify(&hi);
                if (sid.client_cid == 0 && sid.sensor_mid == 0) {
                    printf("Couldn't identify meter in besmart.energy. Waiting 1m...\n");
                    sleep(60);
                }
            } while (sid.client_cid == 0 && sid.sensor_mid == 0);

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
            sendProfileData(&hi, &oid, &sid, timestamp);
        }

        isEnergyFreq = timestamp % ((int)CURRENT_FREQ_S) == 0;
        isCurrentFreq = timestamp % ((int)ENERGY_FREQ_S) == 0;

        if (isCurrentFreq || isEnergyFreq) {
            startDataBlock();

            if (isCurrentFreq) {
                prepareCurrentData(&sid, timestamp * 1000, result);
            }

            if (isEnergyFreq) {
                msg.type = 8;
                if ((res = msgSend(oid.port, &msg)) < 0 || msg.o.io.err == -1) {
                    printf("Could not get data from metersrv (%d)\n", res);
                    sleep(30);
                    continue;
                }
                result = *(MeterBasicResult_t **)msg.o.raw;

                prepareEnergyData(&sid, timestamp * 1000, result);
            }

            endDataBlock();

            sendData(&hi, timestamp, &res, isEnergyFreq);
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
    http_destroy(&hi);
    return 0;
}
