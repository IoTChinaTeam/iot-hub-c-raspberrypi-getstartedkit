
#include "stringhelper.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdbool.h>
#include "serializer.h"

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
