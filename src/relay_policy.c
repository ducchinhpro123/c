#include "relay_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool active;
    uint64_t id;
    char display_name[PROTOCOL_DISPLAY_NAME_MAX + 1u];
} Participant;

typedef enum {
    RECIPIENT_PENDING,
    RECIPIENT_ACCEPTED,
    RECIPIENT_REJECTED,
    RECIPIENT_ACTIVE,
    RECIPIENT_SUCCEEDED,
    RECIPIENT_FAILED
} RecipientStatus;

typedef struct {
    uint64_t participant_id;
    RecipientStatus status;
} OfferRecipient;

typedef enum {
    OFFER_OPEN,
    OFFER_TRANSFERRING
} OfferState;

typedef struct {
    bool active;
    OfferState state;
    uint64_t id;
    uint64_t request_id;
    uint64_t sender_id;
    char filename[PROTOCOL_FILENAME_MAX + 1u];
    uint64_t total_size;
    uint32_t chunk_size;
    uint64_t deadline_ms;
    uint64_t forwarded_bytes;
    bool sender_finished;
    OfferRecipient recipients[RELAY_POLICY_MAX_PARTICIPANTS];
    size_t recipient_count;
} FileOffer;

struct RelayPolicy {
    Participant participants[RELAY_POLICY_MAX_PARTICIPANTS];
    FileOffer offers[RELAY_POLICY_MAX_FILE_OFFERS];
    uint64_t next_participant_id;
    uint64_t next_offer_id;
};

static Participant* find_participant(RelayPolicy* policy, uint64_t participant_id)
{
    if (!policy || participant_id == 0)
        return NULL;
    for (size_t i = 0; i < RELAY_POLICY_MAX_PARTICIPANTS; ++i) {
        if (policy->participants[i].active && policy->participants[i].id == participant_id)
            return &policy->participants[i];
    }
    return NULL;
}

static const Participant* find_participant_const(const RelayPolicy* policy, uint64_t participant_id)
{
    return find_participant((RelayPolicy*)policy, participant_id);
}

static FileOffer* find_offer(RelayPolicy* policy, uint64_t offer_id)
{
    if (!policy || offer_id == 0)
        return NULL;
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        if (policy->offers[i].active && policy->offers[i].id == offer_id)
            return &policy->offers[i];
    }
    return NULL;
}

static OfferRecipient* find_recipient(FileOffer* offer, uint64_t participant_id)
{
    if (!offer)
        return NULL;
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        if (offer->recipients[i].participant_id == participant_id)
            return &offer->recipients[i];
    }
    return NULL;
}

static bool send_effect(const RelayPolicyEffects* effects, uint64_t participant_id,
    const RelayMessage* message)
{
    return effects && effects->send && effects->send(effects->context, participant_id, message);
}

static void reject_action(const RelayPolicyEffects* effects, uint64_t participant_id,
    RelayMessageType rejected_type, uint64_t correlation_id, const char* reason)
{
    RelayMessage rejection = { .type = RELAY_MESSAGE_ACTION_REJECTED };
    rejection.as.action_rejected.rejected_type = rejected_type;
    rejection.as.action_rejected.correlation_id = correlation_id;
    snprintf(rejection.as.action_rejected.reason, sizeof(rejection.as.action_rejected.reason),
        "%s", reason ? reason : "Rejected");
    (void)send_effect(effects, participant_id, &rejection);
}

static size_t active_offer_count_for_sender(const RelayPolicy* policy, uint64_t sender_id)
{
    size_t count = 0;
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        if (policy->offers[i].active && policy->offers[i].sender_id == sender_id)
            count++;
    }
    return count;
}

static bool request_id_is_active(const RelayPolicy* policy, uint64_t sender_id,
    uint64_t request_id)
{
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        const FileOffer* offer = &policy->offers[i];
        if (offer->active && offer->sender_id == sender_id
            && offer->request_id == request_id)
            return true;
    }
    return false;
}

static FileOffer* allocate_offer(RelayPolicy* policy)
{
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        if (!policy->offers[i].active) {
            memset(&policy->offers[i], 0, sizeof(policy->offers[i]));
            policy->offers[i].active = true;
            return &policy->offers[i];
        }
    }
    return NULL;
}

static void clear_offer(FileOffer* offer)
{
    if (offer)
        memset(offer, 0, sizeof(*offer));
}

static bool response_set_is_closed(const FileOffer* offer)
{
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        if (offer->recipients[i].status == RECIPIENT_PENDING)
            return false;
    }
    return true;
}

static size_t active_delivery_count(const FileOffer* offer)
{
    size_t count = 0;
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        if (offer->recipients[i].status == RECIPIENT_ACTIVE)
            count++;
    }
    return count;
}

static bool all_deliveries_terminal(const FileOffer* offer)
{
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        RecipientStatus status = offer->recipients[i].status;
        if (status == RECIPIENT_ACTIVE || status == RECIPIENT_ACCEPTED)
            return false;
    }
    return true;
}

static void send_cancel(const RelayPolicyEffects* effects, uint64_t participant_id,
    uint64_t offer_id, const char* reason)
{
    RelayMessage cancel = { .type = RELAY_MESSAGE_FILE_TRANSFER_CANCEL };
    cancel.as.file_transfer_cancel.offer_id = offer_id;
    snprintf(cancel.as.file_transfer_cancel.reason, sizeof(cancel.as.file_transfer_cancel.reason),
        "%s", reason ? reason : "Cancelled");
    (void)send_effect(effects, participant_id, &cancel);
}

static void cancel_offer(FileOffer* offer, const RelayPolicyEffects* effects,
    const char* reason, bool notify_sender)
{
    if (!offer || !offer->active)
        return;
    if (notify_sender)
        send_cancel(effects, offer->sender_id, offer->id, reason);
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        RecipientStatus status = offer->recipients[i].status;
        if (status == RECIPIENT_PENDING || status == RECIPIENT_ACCEPTED || status == RECIPIENT_ACTIVE)
            send_cancel(effects, offer->recipients[i].participant_id, offer->id, reason);
    }
    clear_offer(offer);
}

static void send_delivery_update(RelayPolicy* policy, FileOffer* offer,
    OfferRecipient* recipient, bool success, const char* reason,
    const RelayPolicyEffects* effects)
{
    const Participant* participant = find_participant_const(policy, recipient->participant_id);
    RelayMessage update = { .type = RELAY_MESSAGE_FILE_DELIVERY_UPDATE };
    update.as.file_delivery_update.offer_id = offer->id;
    update.as.file_delivery_update.recipient_id = recipient->participant_id;
    snprintf(update.as.file_delivery_update.recipient_name,
        sizeof(update.as.file_delivery_update.recipient_name), "%s",
        participant ? participant->display_name : "Disconnected participant");
    update.as.file_delivery_update.success = success;
    snprintf(update.as.file_delivery_update.reason,
        sizeof(update.as.file_delivery_update.reason), "%s", reason ? reason : "");
    (void)send_effect(effects, offer->sender_id, &update);
}

static void fail_delivery(RelayPolicy* policy, FileOffer* offer, OfferRecipient* recipient,
    const char* reason, const RelayPolicyEffects* effects)
{
    if (!offer || !recipient || recipient->status != RECIPIENT_ACTIVE)
        return;
    recipient->status = RECIPIENT_FAILED;
    send_delivery_update(policy, offer, recipient, false, reason, effects);
}

static void close_offer_window(RelayPolicy* policy, FileOffer* offer,
    const RelayPolicyEffects* effects)
{
    if (!offer || offer->state != OFFER_OPEN)
        return;

    uint16_t accepted_count = 0;
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        OfferRecipient* recipient = &offer->recipients[i];
        if (recipient->status == RECIPIENT_PENDING)
            recipient->status = RECIPIENT_REJECTED;
        if (recipient->status == RECIPIENT_ACCEPTED) {
            recipient->status = RECIPIENT_ACTIVE;
            accepted_count++;
        } else {
            send_cancel(effects, recipient->participant_id, offer->id, "File Offer closed");
        }
    }

    if (accepted_count == 0) {
        RelayMessage declined = { .type = RELAY_MESSAGE_FILE_OFFER_DECLINED };
        declined.as.file_offer_declined.offer_id = offer->id;
        (void)send_effect(effects, offer->sender_id, &declined);
        clear_offer(offer);
        return;
    }

    offer->state = OFFER_TRANSFERRING;
    RelayMessage ready = { .type = RELAY_MESSAGE_FILE_TRANSFER_READY };
    ready.as.file_transfer_ready.offer_id = offer->id;
    ready.as.file_transfer_ready.recipient_count = accepted_count;
    (void)send_effect(effects, offer->sender_id, &ready);
    (void)policy;
}

static void handle_chat(RelayPolicy* policy, const Participant* sender,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    RelayMessage delivered = { .type = RELAY_MESSAGE_CHAT_DELIVER };
    delivered.as.chat_deliver.participant_id = sender->id;
    snprintf(delivered.as.chat_deliver.display_name,
        sizeof(delivered.as.chat_deliver.display_name), "%s", sender->display_name);
    snprintf(delivered.as.chat_deliver.text, sizeof(delivered.as.chat_deliver.text),
        "%s", message->as.chat_send.text);

    for (size_t i = 0; i < RELAY_POLICY_MAX_PARTICIPANTS; ++i) {
        if (policy->participants[i].active && policy->participants[i].id != sender->id)
            (void)send_effect(effects, policy->participants[i].id, &delivered);
    }
}

static void handle_offer_create(RelayPolicy* policy, const Participant* sender,
    const RelayMessage* message, uint64_t now_ms, const RelayPolicyEffects* effects)
{
    uint64_t request_id = message->as.file_offer_create.request_id;
    if (request_id_is_active(policy, sender->id, request_id)) {
        reject_action(effects, sender->id, message->type, request_id,
            "File Offer request identity is already active");
        return;
    }
    if (active_offer_count_for_sender(policy, sender->id) >= RELAY_POLICY_MAX_OFFERS_PER_SENDER) {
        reject_action(effects, sender->id, message->type, request_id, "Too many active File Offers");
        return;
    }

    FileOffer* offer = allocate_offer(policy);
    if (!offer) {
        reject_action(effects, sender->id, message->type, request_id, "Relay Server is at capacity");
        return;
    }
    offer->id = policy->next_offer_id++;
    if (offer->id == 0)
        offer->id = policy->next_offer_id++;
    offer->request_id = request_id;
    offer->sender_id = sender->id;
    offer->total_size = message->as.file_offer_create.total_size;
    offer->chunk_size = message->as.file_offer_create.chunk_size;
    offer->deadline_ms = now_ms + RELAY_POLICY_OFFER_WINDOW_MS;
    snprintf(offer->filename, sizeof(offer->filename), "%s",
        message->as.file_offer_create.filename);

    RelayMessage created = { .type = RELAY_MESSAGE_FILE_OFFER_CREATED };
    created.as.file_offer_created.request_id = request_id;
    created.as.file_offer_created.offer_id = offer->id;
    created.as.file_offer_created.offer_window_ms = RELAY_POLICY_OFFER_WINDOW_MS;
    (void)send_effect(effects, sender->id, &created);

    RelayMessage published = { .type = RELAY_MESSAGE_FILE_OFFER_PUBLISHED };
    published.as.file_offer_published.offer_id = offer->id;
    published.as.file_offer_published.sender_id = sender->id;
    published.as.file_offer_published.total_size = offer->total_size;
    published.as.file_offer_published.offer_window_ms = RELAY_POLICY_OFFER_WINDOW_MS;
    snprintf(published.as.file_offer_published.sender_name,
        sizeof(published.as.file_offer_published.sender_name), "%s", sender->display_name);
    snprintf(published.as.file_offer_published.filename,
        sizeof(published.as.file_offer_published.filename), "%s", offer->filename);

    for (size_t i = 0; i < RELAY_POLICY_MAX_PARTICIPANTS; ++i) {
        Participant* participant = &policy->participants[i];
        if (!participant->active || participant->id == sender->id)
            continue;
        OfferRecipient* recipient = &offer->recipients[offer->recipient_count++];
        recipient->participant_id = participant->id;
        recipient->status = RECIPIENT_PENDING;
        if (!send_effect(effects, participant->id, &published))
            recipient->status = RECIPIENT_REJECTED;
    }

    if (offer->recipient_count == 0 || response_set_is_closed(offer))
        close_offer_window(policy, offer, effects);
}

static void handle_offer_response(RelayPolicy* policy, const Participant* participant,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    FileOffer* offer = find_offer(policy, message->as.file_offer_response.offer_id);
    OfferRecipient* recipient = find_recipient(offer, participant->id);
    if (!offer || offer->state != OFFER_OPEN || !recipient
        || recipient->status != RECIPIENT_PENDING) {
        reject_action(effects, participant->id, message->type,
            message->as.file_offer_response.offer_id, "File Offer is not open");
        return;
    }
    recipient->status = message->as.file_offer_response.accepted
        ? RECIPIENT_ACCEPTED
        : RECIPIENT_REJECTED;
    if (response_set_is_closed(offer))
        close_offer_window(policy, offer, effects);
}

static void handle_chunk(RelayPolicy* policy, const Participant* sender,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    FileOffer* offer = find_offer(policy, message->as.file_chunk.offer_id);
    if (!offer || offer->state != OFFER_TRANSFERRING || offer->sender_id != sender->id
        || offer->sender_finished || message->as.file_chunk.offset != offer->forwarded_bytes
        || message->as.file_chunk.data_length > offer->chunk_size
        || offer->forwarded_bytes > offer->total_size
        || message->as.file_chunk.data_length > offer->total_size - offer->forwarded_bytes) {
        reject_action(effects, sender->id, message->type,
            message->as.file_chunk.offer_id, "Invalid File Transfer chunk");
        if (offer && offer->sender_id == sender->id)
            cancel_offer(offer, effects, "Invalid File Transfer chunk", false);
        return;
    }

    for (size_t i = 0; i < offer->recipient_count; ++i) {
        OfferRecipient* recipient = &offer->recipients[i];
        if (recipient->status == RECIPIENT_ACTIVE
            && !send_effect(effects, recipient->participant_id, message))
            fail_delivery(policy, offer, recipient, "Recipient delivery queue is full", effects);
    }
    offer->forwarded_bytes += message->as.file_chunk.data_length;
    if (active_delivery_count(offer) == 0)
        cancel_offer(offer, effects, "No Recipients remain", true);
}

static void handle_transfer_end(RelayPolicy* policy, const Participant* sender,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    FileOffer* offer = find_offer(policy, message->as.file_transfer_end.offer_id);
    if (!offer || offer->state != OFFER_TRANSFERRING || offer->sender_id != sender->id
        || offer->sender_finished || message->as.file_transfer_end.total_size != offer->total_size
        || offer->forwarded_bytes != offer->total_size) {
        reject_action(effects, sender->id, message->type,
            message->as.file_transfer_end.offer_id, "File Transfer size mismatch");
        if (offer && offer->sender_id == sender->id)
            cancel_offer(offer, effects, "File Transfer size mismatch", false);
        return;
    }
    offer->sender_finished = true;
    for (size_t i = 0; i < offer->recipient_count; ++i) {
        OfferRecipient* recipient = &offer->recipients[i];
        if (recipient->status == RECIPIENT_ACTIVE
            && !send_effect(effects, recipient->participant_id, message))
            fail_delivery(policy, offer, recipient, "Recipient delivery queue is full", effects);
    }
    if (all_deliveries_terminal(offer))
        clear_offer(offer);
}

static void handle_delivery_result(RelayPolicy* policy, const Participant* participant,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    FileOffer* offer = find_offer(policy, message->as.file_delivery_result.offer_id);
    OfferRecipient* recipient = find_recipient(offer, participant->id);
    if (!offer || offer->state != OFFER_TRANSFERRING || !recipient
        || recipient->status != RECIPIENT_ACTIVE
        || (message->as.file_delivery_result.success && !offer->sender_finished)) {
        reject_action(effects, participant->id, message->type,
            message->as.file_delivery_result.offer_id, "Delivery is not awaiting this result");
        return;
    }

    recipient->status = message->as.file_delivery_result.success
        ? RECIPIENT_SUCCEEDED
        : RECIPIENT_FAILED;
    send_delivery_update(policy, offer, recipient,
        message->as.file_delivery_result.success,
        message->as.file_delivery_result.reason, effects);
    if (!message->as.file_delivery_result.success && !offer->sender_finished
        && active_delivery_count(offer) == 0) {
        cancel_offer(offer, effects, "No Recipients remain", true);
        return;
    }
    if (all_deliveries_terminal(offer))
        clear_offer(offer);
}

static void handle_transfer_cancel(RelayPolicy* policy, const Participant* participant,
    const RelayMessage* message, const RelayPolicyEffects* effects)
{
    FileOffer* offer = find_offer(policy, message->as.file_transfer_cancel.offer_id);
    if (!offer) {
        reject_action(effects, participant->id, message->type,
            message->as.file_transfer_cancel.offer_id, "Unknown File Offer");
        return;
    }
    if (offer->sender_id == participant->id) {
        cancel_offer(offer, effects, message->as.file_transfer_cancel.reason, false);
        return;
    }
    OfferRecipient* recipient = find_recipient(offer, participant->id);
    if (!recipient || recipient->status != RECIPIENT_ACTIVE) {
        reject_action(effects, participant->id, message->type,
            message->as.file_transfer_cancel.offer_id, "Participant has no active Delivery");
        return;
    }
    fail_delivery(policy, offer, recipient, message->as.file_transfer_cancel.reason, effects);
    if (active_delivery_count(offer) == 0)
        cancel_offer(offer, effects, "No Recipients remain", true);
}

RelayPolicy* relay_policy_create(void)
{
    RelayPolicy* policy = calloc(1, sizeof(*policy));
    if (policy) {
        policy->next_participant_id = 1;
        policy->next_offer_id = 1;
    }
    return policy;
}

void relay_policy_destroy(RelayPolicy* policy)
{
    free(policy);
}

bool relay_policy_join(RelayPolicy* policy, const char* display_name, uint64_t* participant_id)
{
    if (!policy || !participant_id || !protocol_display_name_is_valid(display_name))
        return false;
    for (size_t i = 0; i < RELAY_POLICY_MAX_PARTICIPANTS; ++i) {
        if (!policy->participants[i].active) {
            Participant* participant = &policy->participants[i];
            memset(participant, 0, sizeof(*participant));
            participant->active = true;
            participant->id = policy->next_participant_id++;
            if (participant->id == 0)
                participant->id = policy->next_participant_id++;
            snprintf(participant->display_name, sizeof(participant->display_name), "%s", display_name);
            *participant_id = participant->id;
            return true;
        }
    }
    return false;
}

void relay_policy_leave(RelayPolicy* policy, uint64_t participant_id, uint64_t now_ms,
    const RelayPolicyEffects* effects)
{
    (void)now_ms;
    Participant* participant = find_participant(policy, participant_id);
    if (!participant)
        return;
    participant->active = false;

    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        FileOffer* offer = &policy->offers[i];
        if (!offer->active)
            continue;
        if (offer->sender_id == participant_id) {
            cancel_offer(offer, effects, "Sender disconnected", false);
            continue;
        }
        OfferRecipient* recipient = find_recipient(offer, participant_id);
        if (!recipient)
            continue;
        if (offer->state == OFFER_OPEN
            && (recipient->status == RECIPIENT_PENDING || recipient->status == RECIPIENT_ACCEPTED)) {
            recipient->status = RECIPIENT_REJECTED;
            if (response_set_is_closed(offer))
                close_offer_window(policy, offer, effects);
        } else if (offer->state == OFFER_TRANSFERRING
            && recipient->status == RECIPIENT_ACTIVE) {
            fail_delivery(policy, offer, recipient, "Recipient disconnected", effects);
            if (active_delivery_count(offer) == 0)
                cancel_offer(offer, effects, "No Recipients remain", true);
        }
    }
    memset(participant, 0, sizeof(*participant));
}

void relay_policy_handle(RelayPolicy* policy, uint64_t participant_id,
    const RelayMessage* message, uint64_t now_ms, const RelayPolicyEffects* effects)
{
    Participant* participant = find_participant(policy, participant_id);
    if (!participant || !protocol_message_is_valid(message))
        return;

    switch (message->type) {
    case RELAY_MESSAGE_CHAT_SEND:
        handle_chat(policy, participant, message, effects);
        break;
    case RELAY_MESSAGE_FILE_OFFER_CREATE:
        handle_offer_create(policy, participant, message, now_ms, effects);
        break;
    case RELAY_MESSAGE_FILE_OFFER_RESPONSE:
        handle_offer_response(policy, participant, message, effects);
        break;
    case RELAY_MESSAGE_FILE_CHUNK:
        handle_chunk(policy, participant, message, effects);
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_END:
        handle_transfer_end(policy, participant, message, effects);
        break;
    case RELAY_MESSAGE_FILE_DELIVERY_RESULT:
        handle_delivery_result(policy, participant, message, effects);
        break;
    case RELAY_MESSAGE_FILE_TRANSFER_CANCEL:
        handle_transfer_cancel(policy, participant, message, effects);
        break;
    default:
        reject_action(effects, participant_id, message->type, 0,
            "Message type is not accepted from a Participant");
        break;
    }
}

void relay_policy_tick(RelayPolicy* policy, uint64_t now_ms,
    const RelayPolicyEffects* effects)
{
    if (!policy)
        return;
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        FileOffer* offer = &policy->offers[i];
        if (offer->active && offer->state == OFFER_OPEN && now_ms >= offer->deadline_ms)
            close_offer_window(policy, offer, effects);
    }
}

size_t relay_policy_participant_count(const RelayPolicy* policy)
{
    if (!policy)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < RELAY_POLICY_MAX_PARTICIPANTS; ++i) {
        if (policy->participants[i].active)
            count++;
    }
    return count;
}

size_t relay_policy_file_offer_count(const RelayPolicy* policy)
{
    if (!policy)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < RELAY_POLICY_MAX_FILE_OFFERS; ++i) {
        if (policy->offers[i].active)
            count++;
    }
    return count;
}
