#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

/* config */
#define DEBUG 0

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

// ------

#endif
