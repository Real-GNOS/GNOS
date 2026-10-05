#ifndef NT_PING_REPLY_H
#define NT_PING_REPLY_H

#include <stdint.h>

struct NET_PING_REPLY {
    uint32_t src;
    uint16_t id;
    uint16_t seq;
    uint32_t rtt_ms;
};

#endif
