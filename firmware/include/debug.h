#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <Arduino.h>

#define DEBUG_FUNC_BEGIN() Serial.printf("Begin %s\n", __FUNCTION__);
#define DEBUG_FUNC_END() Serial.printf("End %s\n", __FUNCTION__);

#define DEBUG_VAR(var) Serial.printf("%s:%d -> %s = %lu\n", __FUNCTION__, __LINE__, #var, var)

#endif
