#ifndef RELAY_POLICY_H
#define RELAY_POLICY_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAY_POLICY_MAX_PARTICIPANTS 32u
#define RELAY_POLICY_MAX_FILE_OFFERS 32u
#define RELAY_POLICY_MAX_OFFERS_PER_SENDER 8u
#define RELAY_POLICY_OFFER_WINDOW_MS 60000u

typedef struct RelayPolicy RelayPolicy;

typedef bool (*RelayPolicySend)(void* context, uint64_t participant_id,
    const RelayMessage* message);

typedef struct {
    RelayPolicySend send;
    void* context;
} RelayPolicyEffects;

RelayPolicy* relay_policy_create(void);
void relay_policy_destroy(RelayPolicy* policy);

bool relay_policy_join(RelayPolicy* policy, const char* display_name, uint64_t* participant_id);
void relay_policy_leave(RelayPolicy* policy, uint64_t participant_id, uint64_t now_ms,
    const RelayPolicyEffects* effects);

void relay_policy_handle(RelayPolicy* policy, uint64_t participant_id,
    const RelayMessage* message, uint64_t now_ms, const RelayPolicyEffects* effects);
void relay_policy_tick(RelayPolicy* policy, uint64_t now_ms,
    const RelayPolicyEffects* effects);

size_t relay_policy_participant_count(const RelayPolicy* policy);
size_t relay_policy_file_offer_count(const RelayPolicy* policy);

#endif
