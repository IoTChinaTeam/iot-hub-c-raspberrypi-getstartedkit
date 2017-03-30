
#include "serializer.h"

/**
* @brief	Get number from a json
*
* @param	text	        target json
* @param	size		    json string size
* @param	pValue		    int value in json to return
**
* @return	A @c bool. ture means get int value success from json. Any other value returns a false.
*/
bool GetNumberFromString(const unsigned char* text, size_t size, int* pValue);


/**
* @brief	Write format data to buffer
*
* @param	buffer	        target buffer to write data
* @param	size		    buffer size
* @param	pValue		    formatted data to write to buffer
**
*/
void AllocAndPrintf(unsigned char** buffer, size_t* size, const char* format, ...);

/**
* @brief	Write format data to buffer
*
* @param	buffer	        target buffer to write data
* @param	size		    buffer size
* @param	pValue		    formatted data to write to buffer
* @param	argptr		    a variable arguments list to write to formatted data in pValue
**
*/
void AllocAndVPrintf(unsigned char** buffer, size_t* size, const char* format, va_list argptr);