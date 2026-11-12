#ifndef NODE_UTILS_H_
#define NODE_UTILS_H_

#include <ESPAsyncWebServer.h>
#include <Arduino.h>

namespace NODE_Utils {

    void setup();
    void processLoRaPacket(const String& packet);
    void registerWeb(AsyncWebServer *server);

}

#endif
