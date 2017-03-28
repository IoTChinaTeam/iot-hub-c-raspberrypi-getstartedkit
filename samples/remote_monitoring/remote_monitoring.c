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
#include <curl/curl.h>
#include <pthread.h>

#include <wiringPi.h>
#include <wiringPiSPI.h>
#include "bme280.h"
#include "locking.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/platform.h"

#ifdef MBED_BUILD_TIMESTAMP
#include "certs.h"
#endif // MBED_BUILD_TIMESTAMP


static const char* deviceId = "<replace>";
static const char* deviceKey = "<replace>";
static const char* hubName = "<replace>";
static const char* hubSuffix = "<replace>";

static IOTHUB_CLIENT_HANDLE g_iotHubClientHandle = NULL;

static const int Spi_channel = 0;
static const int Spi_clock = 1000000L;

static const int Grn_led_pin = 5;

static int g_telemetryInterval = 15;

static int Lock_fd;
static bool b_exit = false;

// Define the Model
BEGIN_NAMESPACE(Contoso);

DECLARE_STRUCT(SystemProperties,
ascii_char_ptr, DeviceID,
_Bool, Enabled
);

DECLARE_STRUCT(DeviceProperties,
ascii_char_ptr, DeviceID,
_Bool, HubEnabledState
);

DECLARE_MODEL(Thermostat,

/* Event data (temperature, external temperature and humidity) */
WITH_DATA(int, Temperature),
WITH_DATA(int, ExternalTemperature),
WITH_DATA(int, Humidity),
WITH_DATA(ascii_char_ptr, DeviceId),

/* Device Info - This is command metadata + some extra fields */
WITH_DATA(ascii_char_ptr, ObjectType),
WITH_DATA(_Bool, IsSimulatedDevice),
WITH_DATA(ascii_char_ptr, Version),
WITH_DATA(DeviceProperties, DeviceProperties),
WITH_DATA(ascii_char_ptr_no_quotes, Commands),

/* Commands implemented by the device */
WITH_ACTION(SetTemperature, int, temperature)
);

END_NAMESPACE(Contoso);

EXECUTE_COMMAND_RESULT SetTemperature(Thermostat* thermostat, int temperature)
{
	(void)printf("Received temperature %d\r\n", temperature);
	thermostat->Temperature = temperature;
	return EXECUTE_COMMAND_SUCCESS;
}

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t written = fwrite(ptr, size, nmemb, stream);
	return written;
}

bool DownloadFileFromURL(const char *url)
{
	CURL *curl;
	FILE *fp;
	CURLcode res;
	char outfilename[FILENAME_MAX] = "//home//pi//fireware2.0//remote_monitoring.exe";
	curl = curl_easy_init();
	if (curl) {
		fp = fopen(outfilename, "wb");
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
		res = curl_easy_perform(curl);
		/* always cleanup */
		curl_easy_cleanup(curl);
		fclose(fp);
	}
	return true;
}

bool GetNumberFromString(const unsigned char* text, size_t size, int* pValue)
{
	const unsigned char* pStart = text;
	for (; pStart < text + size; pStart++)
	{
		if (isdigit(*pStart))
		{
			break;
		}
	}

	const unsigned char* pEnd = pStart + 1;
	for (; pEnd <= text + size; pEnd++)
	{
		if (!isdigit(*pEnd))
		{
			break;
		}
	}

	if (pStart >= text + size)
	{
		return false;
	}

	unsigned char buffer[16] = { 0 };
	strncpy(buffer, pStart, pEnd - pStart);

	*pValue = atoi(buffer);
	return true;
}

/* utilities region */
void AllocAndPrintf(unsigned char** buffer, size_t* size, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	*size = vsnprintf(NULL, 0, format, args);
	va_end(args);

	*buffer = malloc(*size + 1);
	va_start(args, format);
	vsprintf((char*)*buffer, format, args);
	va_end(args);
}

void AllocAndVPrintf(unsigned char** buffer, size_t* size, const char* format, va_list argptr)
{
	*size = vsnprintf(NULL, 0, format, argptr);

	*buffer = malloc(*size + 1);
	vsprintf((char*)*buffer, format, argptr);
}

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

void ReportConfigProperties()
{
	UpdateReportedProperties(
		"{ 'Config': { 'TelemetryInterval': %d }, 'FirewareVersion': '1.0'}",
		g_telemetryInterval);
}

void OnDesiredTelemetryIntervalChanged(int telemetryInterval)
{
	g_telemetryInterval = telemetryInterval;
	ReportConfigProperties();

	printf("Telemetry interval set to %u\r\n", g_telemetryInterval);
}

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

void ReportSupportedMethods()
{
	UpdateReportedProperties("{ 'SupportedMethods': { 'ChangeLightStatus--LightStatusValue-int': 'Change light status, 0 off 1 on', 'InitiateFirmwareUpdate--FwPackageUri-string': 'Updates device Firmware. Use parameter FwPackageUri to specifiy the URI of the firmware file, e.g. https://iotrmassets.blob.core.windows.net/firmwares/FW20.bin' } }");
}
void OnMethodChangeLightStatus(const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size)
{
	int lightstatus;
	if (!GetNumberFromString(payload, size, &lightstatus))
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'Invalid payload' }");
	}
	else
	{
		int pin = 7;
		printf("Raspberry Pi light status change\n");
		if (wiringPiSetup() == -1)
		{
			AllocAndPrintf(response, resp_size, "{ 'message': 'wiring Pi set up failed' }");
		}
		else
		{
			pinMode(pin, OUTPUT);
			printf("LED value\n %d", lightstatus);
			digitalWrite(pin, lightstatus);
			AllocAndPrintf(response, resp_size, "{ 'message': 'light status: %d' }", lightstatus);

		}
	}
}

void OnMethodInitiateFirmwareUpdate(const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size)
{
	int lightstatus;
	if (!DownloadFileFromURL(payload))
	{
		AllocAndPrintf(response, resp_size, "{ 'message': 'download file failed' }");
	}
	else
	{
		printf("download from url%s\n", payload);
		AllocAndPrintf(response, resp_size, "{ 'message': 'download the newest fireware and reboot device ' }");
	}
	b_exit = true;
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
		OnMethodInitiateFirmwareUpdate(payload, size, response, resp_size);
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

						ReportSupportedMethods();
						ReportConfigProperties();

						thermostat->Temperature = 50;
						thermostat->ExternalTemperature = 55;
						thermostat->Humidity = 50;
						thermostat->DeviceId = (char*)deviceId;

						while (!b_exit)
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
								pinMode(Grn_led_pin, OUTPUT);
							}
							else
							{
								thermostat->Temperature = 50;
								thermostat->ExternalTemperature = 55;
								thermostat->Humidity = 50;
							}
							(void)printf("Sending sensor value Temperature = %d, Humidity = %d\r\n", thermostat->Temperature, thermostat->Humidity);

							if (SERIALIZE(&buffer, &bufferSize, thermostat->DeviceId, thermostat->Temperature, thermostat->Humidity, thermostat->ExternalTemperature) != CODEFIRST_OK)
							{
								(void)printf("Failed sending sensor value\r\n");
							}
							else
							{
								sendMessage(iotHubClientHandle, buffer, bufferSize);
							}

							ThreadAPI_Sleep(g_telemetryInterval * 1000);
						}
						/* wait a while before exit program*/
						ThreadAPI_Sleep(5 * 1000);
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

