#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

#include "protocol.h"
#include "relay_transport.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct ClientConnection ClientConnection;

ClientConnection* client_connection_create(void);
void client_connection_destroy(ClientConnection* connection);

int connect_to_server(ClientConnection* connection, const char* host, const char* port,
    const char* display_name);
void disconnect_from_server(ClientConnection* connection);

bool client_connection_is_connected(const ClientConnection* connection);
uint64_t client_connection_participant_id(const ClientConnection* connection);
const char* client_connection_display_name(const ClientConnection* connection);

RelaySendResult client_connection_send(ClientConnection* connection,
    const RelayMessage* message);
RelaySendResult client_connection_send_chat(ClientConnection* connection,
    const char* text);

int client_connection_poll(ClientConnection* connection, RelayMessageHandler handler,
    void* context);

RelayTransport client_connection_transport(ClientConnection* connection);

#endif
