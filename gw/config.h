#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

/* config */
#define DEBUG 1

#define SEND_TO_AZURE_IOTHUB 1
#define SEND_TO_BESMART_ENERGY 0

#define PORT ""
#define TOKEN ""
#define API_NAME ""
#define SENSORS_API ""
#define FIND_STATE_ENDPOINT ""
#define PUT_SIGNALS_DATA_ENDPOINT ""
#define METER_PROFILE_FREQ_S 15 * 60

#define CURRENT_RATIO 1
#define VOLTAGE_RATIO 1

// ENERGY_FREQ_S <= METER_PROFILE_FREQ_S
#define ENERGY_FREQ_S 15 * 60
#define CURRENT_FREQ_S 60

// sync time
#define SYNC_TIME_FREQ_MIN 12 * 60
#define SYNC_TIME_TH_S 5

// Azure IoT Config
#define AZURE_CONNECTION_STRING ""

// ------

#endif