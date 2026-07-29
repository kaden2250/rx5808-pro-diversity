#ifndef SERIAL_LINK_H
#define SERIAL_LINK_H


#include "settings.h"


#ifdef USE_SERIAL_OUT
namespace SerialLink {
    void setup();
    void update();
}
#endif


#endif
