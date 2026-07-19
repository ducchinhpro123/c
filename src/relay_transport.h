#ifndef RELAY_TRANSPORT_H
#define RELAY_TRANSPORT_H

#include "protocol.h"

#include <stdbool.h>

typedef enum {
    RELAY_SEND_OK = 0,
    RELAY_SEND_BACKPRESSURE,
    RELAY_SEND_CLOSED,
    RELAY_SEND_ERROR
} RelaySendResult;

typedef RelaySendResult (*RelayTransportSend)(void* context, const RelayMessage* message);
typedef bool (*RelayTransportConnected)(void* context);

typedef struct {
    void* context;
    RelayTransportSend send;
    RelayTransportConnected connected;
} RelayTransport;

static inline RelaySendResult relay_transport_send(const RelayTransport* transport,
    const RelayMessage* message)
{
    if (!transport || !transport->send)
        return RELAY_SEND_ERROR;
    return transport->send(transport->context, message);
}

static inline bool relay_transport_is_connected(const RelayTransport* transport)
{
    return transport && transport->connected && transport->connected(transport->context);
}

#endif
