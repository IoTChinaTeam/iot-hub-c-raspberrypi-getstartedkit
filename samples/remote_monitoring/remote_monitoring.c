// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef WINCE
#include "iothubtransportmqtt.h"
#else
#include "iothubtransporthttp.h"
#endif
#include "schemalib.h"
#include "iothub_client.h"
#include "serializer.h"
#include "schemaserializer.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <ctype.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>
#include "bme280.h"
#include "locking.h"
#include "stringhelper.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/platform.h"

#ifdef MBED_BUILD_TIMESTAMP
#include "certs.h"
#endif // MBED_BUILD_TIMESTAMP


static const char* deviceId = "[Device Id]";
static const char* deviceKey = "[Device Key]";
static const char* hubName = "[IoTHub Name]";
static const char* hubSuffix = "[IoTHub Suffix, i.e. azure-devices.net]";

static IOTHUB_CLIENT_HANDLE g_iotHubClientHandle = NULL;

static const int Spi_channel = 0;
static const int Spi_clock = 1000000L;

static const int Grn_led_pin = 7;

static int g_telemetryInterval = 15;
static char g_firmwareVersion[16] = { 0 };

static int Lock_fd;
static const int wireSetupFailed = -1;

/*json of supported methods*/
static const char* supportedMethod = "{ 'SupportedMethods': { 'LightBlink': 'light blink', 'ChangeLightStatus--LightStatusValue-int'"
									 ": 'Change light status, 0 light off, 1 light on', 'InitiateFirmwareUpdate--FwPackageUri-string':"
									 " 'Updates device Firmware. Use parameter FwPackageUri to specifiy the URI of the firmware file, "
									 "e.g. https://iotrmassets.blob.core.windows.net/firmwares/FW20.bin'  } }";

/*json of report config*/
static const char* reportedConfig = "{ 'Config': { 'TelemetryInterval': %d }}";
static const char* reportedVersion = "{ 'FirmwareVersion': '%s' }";

// Define the Model
BEGIN_NAMESPACE(Contoso);

DECLARE_STRUCT(DeviceProperties,
ascii_char_ptr, DeviceID,
_Bool, HubEnabledState
);

DECLARE_MODEL(Thermostat,

/* Event data (temperature and humidity) */
WITH_DATA(int, Temperature),
WITH_DATA(int, Humidity),
WITH_DATA(ascii_char_ptr, DeviceId),

/* Device Info - This is command metadata + some extra fields */
WITH_DATA(ascii_char_ptr, ObjectType),
WITH_DATA(_Bool, IsSimulatedDevice),
WITH_DATA(ascii_char_ptr, Version),
WITH_DATA(DeviceProperties, DeviceProperties),
WITH_DATA(ascii_char_ptr_no_quotes, Commands)
);

END_NAMESPACE(Contoso);

void UpdateReportedProperties(const char* format, ...)
{
	unsigned char* report;
	size_t len;

	va_list args;
	va_start(args, format);
	AllocAndVPrintf(&report, &len, format, args);
	va_end(args);

	if (IoTHubClient_SendReportedState(g_iotHubClientHandle, report, len, NULL, NULL) != IOTHUB_CLIENT_OK)
	{
		(void)printf("Failed to update reported properties: %.*s\r\n", len, report);
	}
	else
	{
		(void)printf("Succeeded in updating reported properties: %.*s\r\n", len, report);
	}

	free(report);
}

/*report the new Config.TelemetryInterval while received the desire property change*/
void OnDesiredTelemetryIntervalChanged(int telemetryInterval)
{
	g_telemetryInterval = telemetryInterval;

	UpdateReportedProperties(
		reportedConfig,
		g_telemetryInterval);

	printf("Telemetry interval set to %u\r\n", g_telemetryInterval);
}

/*change config property TelemetryInterval as DesiredProperty change example*/
void OnDesiredPropertyChanged(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payload, size_t size, void* userContextCallback)
{
	printf("Property changed: %.*s\r\n", size, payload);

	unsigned char *p = strstr(payload, "\"TelemetryInterval\":");

	int telemetryInterval;
	if (GetNumberFromString(p, size - (p - payload), &telemetryInterval) && telemetryInterval > 0)
	{
		OnDesiredTelemetryIntervalChanged(telemetryInterval);
	}
}

/*change light status on Raspberry Pi to received value*/
void OnMethodChangeLightStatus(const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size)
{
	int lightstatus;
	if (!GetNumberFromString(payload, size, &lightstatus))
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'Invalid payload' }");
	}
	else
	{
		printf("Raspberry Pi light status change\n");
		if (wiringPiSetup() == wireSetupFailed)
		{
			AllocAndPrintf(response, resp_size, "{ 'message': 'wiring Pi set up failed' }");
		}
		else
		{
			pinMode(Grn_led_pin, OUTPUT);
			printf("LED value\n %d", lightstatus);
			digitalWrite(Grn_led_pin, lightstatus);
			AllocAndPrintf(response, resp_size, "{ 'message': 'light status: %d' }", lightstatus);

		}
	}
}

void OnMethodFirmwareUpdate(const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size)
{
	const unsigned char* key = "\"FwPackageUri\":";

	unsigned char* p = strstr(payload, key);
	if (p == NULL)
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'Invalid payload' }");
		return;
	}

	unsigned char* pStart = strchr(p + strlen(key), '\"');
	if (pStart == NULL)
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'Invalid payload' }");
		return;
	}

	unsigned char* pEnd = strchr(pStart + 1, '\"');
	if (pEnd == NULL)
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'Invalid payload' }");
		return;
	}

	unsigned char url[1024];
	strcpy(url, pStart + 1);
	url[pEnd - pStart - 1] = 0;
	printf("Updaing firmware with %s\r\n", url);
	AllocAndPrintf(response, resp_size, "{ 'message': 'Accepted, url = %s' }", url);

	int version;
	GetNumberFromString(url, strlen(url), &version);
	printf("Version = %d\r\n", version);
	sprintf(g_firmwareVersion, "%d.%d", version / 10, version % 10);
	UpdateReportedProperties(reportedVersion, g_firmwareVersion);

}

void OnMethodLightBlink(const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size)
{
	int blinkCount = 2;
	printf("Raspberry Pi light blink\n");
	if (wiringPiSetup() == -1)
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'wiring Pi set up failed' }");
	}
	else
	{
		while (blinkCount--)
		{
			pinMode(Grn_led_pin, OUTPUT);
			printf("light on\n");
			digitalWrite(Grn_led_pin, 1);
			ThreadAPI_Sleep(1000);
			printf("light off\n");
			digitalWrite(Grn_led_pin, 0);
			ThreadAPI_Sleep(1000);
		}
		AllocAndPrintf(response, resp_size, "{ 'message': 'Pi light blink success' }");
	}
}

int OnDeviceMethodInvoked(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size, void* userContextCallback)
{
	printf("Method call: name = %s, payload = %.*s\r\n", method_name, size, payload);

	if (strcmp(method_name, "ChangeLightStatus") == 0)
	{
		OnMethodChangeLightStatus(payload, size, response, resp_size);
	}
	else if (strcmp(method_name, "InitiateFirmwareUpdate") == 0)
	{
		OnMethodFirmwareUpdate(payload, size, response, resp_size);
	}
	else if (strcmp(method_name, "LightBlink") == 0)
	{
		OnMethodLightBlink(payload, size, response, resp_size);
	}
	else
	{
		AllocAndPrintf(response, resp_size, "{'message': 'Unknown method %s'}", method_name);
	}

	return IOTHUB_CLIENT_OK;
}

static void sendMessage(IOTHUB_CLIENT_HANDLE iotHubClientHandle, const unsigned char* buffer, size_t size)
{
	IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray(buffer, size);
	if (messageHandle == NULL)
	{
		printf("unable to create a new IoTHubMessage\r\n");
	}
	else
	{
		if (IoTHubClient_SendEventAsync(iotHubClientHandle, messageHandle, NULL, NULL) != IOTHUB_CLIENT_OK)
		{
			printf("failed to hand over the message to IoTHubClient");
		}
		else
		{
			printf("IoTHubClient accepted the message for delivery\r\n");
		}

		IoTHubMessage_Destroy(messageHandle);
	}
	free((void*)buffer);
}

/*this function "links" IoTHub to the serialization library*/
static IOTHUBMESSAGE_DISPOSITION_RESULT IoTHubMessage(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
	IOTHUBMESSAGE_DISPOSITION_RESULT result;
	const unsigned char* buffer;
	size_t size;
	if (IoTHubMessage_GetByteArray(message, &buffer, &size) != IOTHUB_MESSAGE_OK)
	{
		printf("unable to IoTHubMessage_GetByteArray\r\n");
		result = IOTHUBMESSAGE_ABANDONED;
	}
	else
	{
		/*buffer is not zero terminated*/
		char* temp = malloc(size + 1);
		if (temp == NULL)
		{
			printf("failed to malloc\r\n");
			result = IOTHUBMESSAGE_ABANDONED;
		}
		else
		{
			EXECUTE_COMMAND_RESULT executeCommandResult;

			(void)memcpy(temp, buffer, size);
			temp[size] = '\0';
			executeCommandResult = EXECUTE_COMMAND(userContextCallback, temp);
			result =
				(executeCommandResult == EXECUTE_COMMAND_ERROR) ? IOTHUBMESSAGE_ABANDONED :
				(executeCommandResult == EXECUTE_COMMAND_SUCCESS) ? IOTHUBMESSAGE_ACCEPTED :
				IOTHUBMESSAGE_REJECTED;
			free(temp);
		}
	}
	return result;
}

void remote_monitoring_run(void)
{
	if (platform_init() != 0)
	{
		printf("Failed to initialize the platform.\r\n");
	}
	else
	{
		if (serializer_init(NULL) != SERIALIZER_OK)
		{
			printf("Failed on serializer_init\r\n");
		}
		else
		{
			IOTHUB_CLIENT_CONFIG config;
			IOTHUB_CLIENT_HANDLE iotHubClientHandle;

			config.deviceSasToken = NULL;
			config.deviceId = deviceId;
			config.deviceKey = deviceKey;
			config.iotHubName = hubName;
			config.iotHubSuffix = hubSuffix;
			config.protocolGatewayHostName = NULL;
#ifndef WINCE
			config.protocol = MQTT_Protocol;
#else
			config.protocol = HTTP_Protocol;
#endif
			iotHubClientHandle = IoTHubClient_Create(&config);
			g_iotHubClientHandle = iotHubClientHandle;
			if (iotHubClientHandle == NULL)
			{
				(void)printf("Failed on IoTHubClient_CreateFromConnectionString\r\n");
			}
			else
			{
#ifdef MBED_BUILD_TIMESTAMP
				// For mbed add the certificate information
				if (IoTHubClient_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
				{
					printf("failure to set option \"TrustedCerts\"\r\n");
				}
#endif // MBED_BUILD_TIMESTAMP

				Thermostat* thermostat = CREATE_MODEL_INSTANCE(Contoso, Thermostat);
				if (thermostat == NULL)
				{
					(void)printf("Failed on CREATE_MODEL_INSTANCE\r\n");
				}
				else
				{
					STRING_HANDLE commandsMetadata;

					if (IoTHubClient_SetMessageCallback(iotHubClientHandle, IoTHubMessage, thermostat) != IOTHUB_CLIENT_OK)
					{
						printf("unable to IoTHubClient_SetMessageCallback\r\n");
					}
					else if (IoTHubClient_SetDeviceMethodCallback(iotHubClientHandle, OnDeviceMethodInvoked, NULL) != IOTHUB_CLIENT_OK)
					{
						printf("unable to IoTHubClient_SetDeviceMethodCallback\r\n");
					}
					else if (IoTHubClient_SetDeviceTwinCallback(iotHubClientHandle, OnDesiredPropertyChanged, NULL) != IOTHUB_CLIENT_OK)
					{
						printf("unable to IoTHubClient_SetDeviceTwinCallback\r\n");
					}
					else
					{

						/* send the device info upon startup so that the cloud app knows
						what commands are available and the fact that the device is up */
						thermostat->ObjectType = "DeviceInfo";
						thermostat->IsSimulatedDevice = false;
						thermostat->Version = "1.0";
						thermostat->DeviceProperties.HubEnabledState = true;
						thermostat->DeviceProperties.DeviceID = (char*)deviceId;

						commandsMetadata = STRING_new();
						if (commandsMetadata == NULL)
						{
							(void)printf("Failed on creating string for commands metadata\r\n");
						}
						else
						{
							/* Serialize the commands metadata as a JSON string before sending */
							if (SchemaSerializer_SerializeCommandMetadata(GET_MODEL_HANDLE(Contoso, Thermostat), commandsMetadata) != SCHEMA_SERIALIZER_OK)
							{
								(void)printf("Failed serializing commands metadata\r\n");
							}
							else
							{
								unsigned char* buffer;
								size_t bufferSize;
								thermostat->Commands = (char*)STRING_c_str(commandsMetadata);

								/* Here is the actual send of the Device Info */
								if (SERIALIZE(&buffer, &bufferSize, thermostat->ObjectType, thermostat->Version, thermostat->IsSimulatedDevice, thermostat->DeviceProperties, thermostat->Commands) != CODEFIRST_OK)
								{
									(void)printf("Failed serializing\r\n");
								}
								else
								{
									sendMessage(iotHubClientHandle, buffer, bufferSize);
								}

							}

							STRING_delete(commandsMetadata);
						}

						UpdateReportedProperties(supportedMethod);
						UpdateReportedProperties(
							reportedConfig,
							g_telemetryInterval);
						UpdateReportedProperties(reportedVersion, "1.0");

						thermostat->Temperature = 50;
						thermostat->Humidity = 50;
						thermostat->DeviceId = (char*)deviceId;

						while (1)
						{
							unsigned char*buffer;
							size_t bufferSize;
							float tempC = -300.0;
							float pressurePa = -300;
							float humidityPct = -300;

							int sensorResult = bme280_read_sensors(&tempC, &pressurePa, &humidityPct);

							if (sensorResult == 1)
							{
								thermostat->Temperature = tempC;
								thermostat->Humidity = humidityPct;
								printf("Read Sensor Data: Humidity = %.1f%% Temperature = %.1f*C \n",
									humidityPct, tempC);
							}
							else
							{
								thermostat->Temperature = 50;
								thermostat->Humidity = 50;
							}
							(void)printf("Sending sensor value Temperature = %d, Humidity = %d\r\n", thermostat->Temperature, thermostat->Humidity);

							if (SERIALIZE(&buffer, &bufferSize, thermostat->DeviceId, thermostat->Temperature, thermostat->Humidity) != CODEFIRST_OK)
							{
								(void)printf("Failed sending sensor value\r\n");
							}
							else
							{
								sendMessage(iotHubClientHandle, buffer, bufferSize);
							}

							ThreadAPI_Sleep(g_telemetryInterval * 1000);
						}
					}

					DESTROY_MODEL_INSTANCE(thermostat);
				}
				IoTHubClient_Destroy(iotHubClientHandle);
			}
			serializer_deinit();
		}
		platform_deinit();
	}
}

int remote_monitoring_init(void)
{
	int result;

	Lock_fd = open_lockfile(LOCKFILE);

	if (setuid(getuid()) < 0)
	{
		perror("Dropping privileges failed. (did you use sudo?)n");
		result = EXIT_FAILURE;
	}
	else
	{
		result = wiringPiSetup();
		if (result != 0)
		{
			perror("Wiring Pi setup failed.");
		}
		else
		{
			result = wiringPiSPISetup(Spi_channel, Spi_clock);
			if (result < 0)
			{
				printf("Can't setup SPI, error %i calling wiringPiSPISetup(%i, %i)  %sn",
					result, Spi_channel, Spi_clock, strerror(result));
			}
			else
			{
				int sensorResult = bme280_init(Spi_channel);
				if (sensorResult != 1)
				{
					printf("It appears that no BMP280 module on Chip Enable %i is attached. Aborting.\n", Spi_channel);
					result = 1;
				}
				else
				{
					// Read the Temp & Pressure module.
					float tempC = -300.0;
					float pressurePa = -300;
					float humidityPct = -300;
					sensorResult = bme280_read_sensors(&tempC, &pressurePa, &humidityPct);
					if (sensorResult == 1)
					{
						printf("Temperature = %.1f *C  Pressure = %.1f Pa  Humidity = %1f %%\n",
							tempC, pressurePa, humidityPct);
						result = 0;
					}
					else
					{
						printf("Unable to read BME280 on pin %i. Aborting.\n", Spi_channel);
						result = 1;
					}
				}
			}
		}
	}
	return result;
}
int main(void)
{
	int result = remote_monitoring_init();
	if (result == 0)
	{
		remote_monitoring_run();
	}
	return result;
}

