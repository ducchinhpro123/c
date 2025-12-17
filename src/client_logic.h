#ifndef CLIENT_LOGIC_H
#define CLIENT_LOGIC_H

#include "client_network.h"
#include "message.h"

// Key function to be tested
bool update_client_state(ClientConnection* conn, MessageQueue* mq);

// Exposed for testing if needed
void process_incoming_stream(MessageQueue* mq, const char* data, size_t len);

#endif
