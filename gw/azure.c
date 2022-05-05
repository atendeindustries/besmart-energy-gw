/*
 * Azure iot sdk c basic api inspired by iothub_ll_telemetry_sample
 *
 * Copyright 2022 Phoenix Systems
 * Author: Damian Loewnau
 *
 */

#include "azure.h"

static unsigned long msgConfirmations = 0;


static void azure_sendConfirmCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* userContextCallback)
{
	(void)userContextCallback;
	/* When a message is sent this callback will get invoked */
	msgConfirmations++;
	printf("Confirmation callback received for message %lu with result %s\r\n", (unsigned long)msgConfirmations, MU_ENUM_TO_STRING(IOTHUB_CLIENT_CONFIRMATION_RESULT, result));
}


static void azure_connectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_context)
{
	(void)reason;
	(void)user_context;
	/* It doesn't take into consideration network outages */
	if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
	{
		printf("The device client is connected to iothub\r\n");
	}
	else
	{
		printf("The device client has been disconnected\r\n");
	}
}


int azure_init(const char *connectionString, const unsigned char *cert, IOTHUB_DEVICE_CLIENT_LL_HANDLE *devhandle)
{
	IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol;

	protocol = MQTT_Protocol;
	IoTHub_Init();
	printf("Creating IoTHub Device handle\r\n");
	*devhandle = IoTHubDeviceClient_LL_CreateFromConnectionString(connectionString, protocol);
	if (*devhandle == NULL)
	{
		fprintf(stderr, "Failure creating IotHub device. Hint: Check your connection string.\n");
		return 1;
	}
	IoTHubDeviceClient_LL_SetOption(*devhandle, OPTION_TRUSTED_CERT, cert);
	bool urlEncodeOn = true;
	IoTHubDeviceClient_LL_SetOption(*devhandle, OPTION_AUTO_URL_ENCODE_DECODE, &urlEncodeOn);
	/* Setting connection status callback to get indication of connection to iothub */
	IoTHubDeviceClient_LL_SetConnectionStatusCallback(*devhandle, azure_connectionStatusCallback, NULL);
	return 0;
}


int azure_sendMsg(IOTHUB_DEVICE_CLIENT_LL_HANDLE *devhandle, const char *msg)
{
	IOTHUB_MESSAGE_HANDLE msghandle;
	unsigned long messagesSent = 0;
	bool running = true;
	do {
		if (messagesSent < 1) {
			msghandle = IoTHubMessage_CreateFromString(msg);
			IoTHubMessage_SetProperty(msghandle, "property_key", "property_value");
			printf("Sending message the following message to IoTHub: %s\n", msg);
			IoTHubDeviceClient_LL_SendEventAsync(*devhandle, msghandle, azure_sendConfirmCallback, NULL);
			/* The message is copied to the sdk so the we can destroy it */
			IoTHubMessage_Destroy(msghandle);
			messagesSent++;
		}
		else if (msgConfirmations >= 1) {
			/* After all messages are all received stop running */
			running = false;
		}
		IoTHubDeviceClient_LL_DoWork(*devhandle);
		ThreadAPI_Sleep(1);
	} while (running);
	printf("The message sent properly\n");
	return 0;
}


void azure_deinit(void)
{
	IoTHub_Deinit();
}