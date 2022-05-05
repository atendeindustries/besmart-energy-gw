/*
 * Azure iot sdk c basic api inspired by iothub_ll_telemetry_sample
 *
 * Copyright 2022 Phoenix Systems
 * Author: Damian Loewnau
 *
 */

#ifndef AZURE_H
#define AZURE_H

#include "iothub.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "iothubtransportmqtt.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/shared_util_options.h"

extern int azure_init(const char *connectionString, const unsigned char *cert, IOTHUB_DEVICE_CLIENT_LL_HANDLE *devhandle);
extern int azure_sendMsg(IOTHUB_DEVICE_CLIENT_LL_HANDLE *devhandle, const char *msg);
extern void azure_deinit(void);

#endif