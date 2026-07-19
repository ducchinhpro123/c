#include "relay_policy.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t target;
    RelayMessage message;
} CapturedEffect;

static RelayPolicy* policy;
static CapturedEffect captured[256];
static size_t captured_count;
static uint64_t fail_target;

static void destroy_captured(void)
{
    for (size_t i = 0; i < captured_count; ++i)
        protocol_message_destroy(&captured[i].message);
    memset(captured, 0, sizeof(captured));
    captured_count = 0;
}

static bool capture_send(void* context, uint64_t target, const RelayMessage* message)
{
    (void)context;
    if (target == fail_target)
        return false;
    TEST_ASSERT_LESS_THAN(256, captured_count);
    CapturedEffect* effect = &captured[captured_count++];
    effect->target = target;
    effect->message = *message;
    if (message->type == RELAY_MESSAGE_FILE_CHUNK) {
        effect->message.as.file_chunk.data = malloc(message->as.file_chunk.data_length);
        TEST_ASSERT_NOT_NULL(effect->message.as.file_chunk.data);
        memcpy(effect->message.as.file_chunk.data, message->as.file_chunk.data,
            message->as.file_chunk.data_length);
    }
    return true;
}

static RelayPolicyEffects effects(void)
{
    return (RelayPolicyEffects) { .send = capture_send, .context = NULL };
}

void setUp(void)
{
    destroy_captured();
    fail_target = 0;
    policy = relay_policy_create();
    TEST_ASSERT_NOT_NULL(policy);
}

void tearDown(void)
{
    relay_policy_destroy(policy);
    policy = NULL;
    destroy_captured();
}

static uint64_t join(const char* name)
{
    uint64_t id = 0;
    TEST_ASSERT_TRUE(relay_policy_join(policy, name, &id));
    TEST_ASSERT_NOT_EQUAL(0, id);
    return id;
}

static CapturedEffect* find_effect(uint64_t target, RelayMessageType type, size_t occurrence)
{
    for (size_t i = 0; i < captured_count; ++i) {
        if (captured[i].target == target && captured[i].message.type == type) {
            if (occurrence == 0)
                return &captured[i];
            occurrence--;
        }
    }
    return NULL;
}

static uint64_t create_offer(uint64_t sender, const char* filename, uint64_t now_ms)
{
    RelayMessage create = { .type = RELAY_MESSAGE_FILE_OFFER_CREATE };
    create.as.file_offer_create.request_id = 77;
    strcpy(create.as.file_offer_create.filename, filename);
    create.as.file_offer_create.total_size = 4;
    create.as.file_offer_create.chunk_size = 4;
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, sender, &create, now_ms, &fx);
    CapturedEffect* created = find_effect(sender, RELAY_MESSAGE_FILE_OFFER_CREATED, 0);
    TEST_ASSERT_NOT_NULL(created);
    return created->message.as.file_offer_created.offer_id;
}

static void respond(uint64_t participant, uint64_t offer_id, bool accepted)
{
    RelayMessage response = { .type = RELAY_MESSAGE_FILE_OFFER_RESPONSE };
    response.as.file_offer_response.offer_id = offer_id;
    response.as.file_offer_response.accepted = accepted;
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, participant, &response, 100, &fx);
}

void test_file_offer_freezes_recipients_and_routes_chunks_only_to_acceptors(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t carol = join("Carol");
    uint64_t offer_id = create_offer(alice, "report|final.txt", 0);
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_OFFER_PUBLISHED, 0));
    TEST_ASSERT_NOT_NULL(find_effect(carol, RELAY_MESSAGE_FILE_OFFER_PUBLISHED, 0));

    destroy_captured();
    respond(bob, offer_id, true);
    TEST_ASSERT_NULL(find_effect(alice, RELAY_MESSAGE_FILE_TRANSFER_READY, 0));
    respond(carol, offer_id, false);
    CapturedEffect* ready = find_effect(alice, RELAY_MESSAGE_FILE_TRANSFER_READY, 0);
    TEST_ASSERT_NOT_NULL(ready);
    TEST_ASSERT_EQUAL(1, ready->message.as.file_transfer_ready.recipient_count);

    destroy_captured();
    uint8_t bytes[] = { 1, 2, 3, 4 };
    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = offer_id;
    chunk.as.file_chunk.data = bytes;
    chunk.as.file_chunk.data_length = sizeof(bytes);
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, alice, &chunk, 200, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_CHUNK, 0));
    TEST_ASSERT_NULL(find_effect(carol, RELAY_MESSAGE_FILE_CHUNK, 0));
}

void test_offer_expiry_declines_when_nobody_accepts(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t offer_id = create_offer(alice, "x.txt", 1000);
    destroy_captured();

    RelayPolicyEffects fx = effects();
    relay_policy_tick(policy, 1000 + RELAY_POLICY_OFFER_WINDOW_MS, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(alice, RELAY_MESSAGE_FILE_OFFER_DECLINED, 0));
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_TRANSFER_CANCEL, 0));
    TEST_ASSERT_EQUAL(0, relay_policy_file_offer_count(policy));
    (void)offer_id;
}

void test_invited_disconnect_closes_window_early(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t carol = join("Carol");
    uint64_t offer_id = create_offer(alice, "x.txt", 0);
    destroy_captured();
    respond(bob, offer_id, true);
    RelayPolicyEffects fx = effects();
    relay_policy_leave(policy, carol, 10, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(alice, RELAY_MESSAGE_FILE_TRANSFER_READY, 0));
}

void test_slow_recipient_failure_isolated_from_other_delivery(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t carol = join("Carol");
    uint64_t offer_id = create_offer(alice, "x.txt", 0);
    respond(bob, offer_id, true);
    respond(carol, offer_id, true);
    destroy_captured();

    fail_target = carol;
    uint8_t bytes[] = { 1, 2, 3, 4 };
    RelayMessage chunk = { .type = RELAY_MESSAGE_FILE_CHUNK };
    chunk.as.file_chunk.offer_id = offer_id;
    chunk.as.file_chunk.data = bytes;
    chunk.as.file_chunk.data_length = sizeof(bytes);
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, alice, &chunk, 20, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_CHUNK, 0));
    CapturedEffect* failed = find_effect(alice, RELAY_MESSAGE_FILE_DELIVERY_UPDATE, 0);
    TEST_ASSERT_NOT_NULL(failed);
    TEST_ASSERT_EQUAL_UINT64(carol, failed->message.as.file_delivery_update.recipient_id);
    TEST_ASSERT_FALSE(failed->message.as.file_delivery_update.success);

    fail_target = 0;
    destroy_captured();
    RelayMessage end = { .type = RELAY_MESSAGE_FILE_TRANSFER_END };
    end.as.file_transfer_end.offer_id = offer_id;
    end.as.file_transfer_end.total_size = 4;
    relay_policy_handle(policy, alice, &end, 30, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_TRANSFER_END, 0));
    TEST_ASSERT_NULL(find_effect(carol, RELAY_MESSAGE_FILE_TRANSFER_END, 0));

    RelayMessage result = { .type = RELAY_MESSAGE_FILE_DELIVERY_RESULT };
    result.as.file_delivery_result.offer_id = offer_id;
    result.as.file_delivery_result.success = true;
    relay_policy_handle(policy, bob, &result, 40, &fx);
    TEST_ASSERT_EQUAL(0, relay_policy_file_offer_count(policy));
}

void test_sender_disconnect_cancels_every_active_delivery(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t offer_id = create_offer(alice, "x.txt", 0);
    respond(bob, offer_id, true);
    destroy_captured();

    RelayPolicyEffects fx = effects();
    relay_policy_leave(policy, alice, 20, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(bob, RELAY_MESSAGE_FILE_TRANSFER_CANCEL, 0));
    TEST_ASSERT_EQUAL(0, relay_policy_file_offer_count(policy));
}

void test_chat_attribution_comes_from_participant_identity(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    RelayMessage chat = { .type = RELAY_MESSAGE_CHAT_SEND };
    strcpy(chat.as.chat_send.text, "Bob: forged?");
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, alice, &chat, 0, &fx);
    CapturedEffect* delivered = find_effect(bob, RELAY_MESSAGE_CHAT_DELIVER, 0);
    TEST_ASSERT_NOT_NULL(delivered);
    TEST_ASSERT_EQUAL_UINT64(alice, delivered->message.as.chat_deliver.participant_id);
    TEST_ASSERT_EQUAL_STRING("Alice", delivered->message.as.chat_deliver.display_name);
    TEST_ASSERT_EQUAL_STRING("Bob: forged?", delivered->message.as.chat_deliver.text);
}

void test_failed_last_delivery_cancels_sender_before_more_chunks(void)
{
    uint64_t alice = join("Alice");
    uint64_t bob = join("Bob");
    uint64_t offer_id = create_offer(alice, "x.txt", 0);
    respond(bob, offer_id, true);
    destroy_captured();

    RelayMessage result = { .type = RELAY_MESSAGE_FILE_DELIVERY_RESULT };
    result.as.file_delivery_result.offer_id = offer_id;
    strcpy(result.as.file_delivery_result.reason, "local write failed");
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, bob, &result, 20, &fx);
    TEST_ASSERT_NOT_NULL(find_effect(alice, RELAY_MESSAGE_FILE_DELIVERY_UPDATE, 0));
    TEST_ASSERT_NOT_NULL(find_effect(alice, RELAY_MESSAGE_FILE_TRANSFER_CANCEL, 0));
    TEST_ASSERT_EQUAL_size_t(0, relay_policy_file_offer_count(policy));
}

void test_duplicate_active_request_identity_is_rejected(void)
{
    uint64_t alice = join("Alice");
    (void)join("Bob");
    (void)create_offer(alice, "first.txt", 0);
    destroy_captured();

    RelayMessage duplicate = { .type = RELAY_MESSAGE_FILE_OFFER_CREATE };
    duplicate.as.file_offer_create.request_id = 77;
    duplicate.as.file_offer_create.total_size = 4;
    duplicate.as.file_offer_create.chunk_size = 4;
    strcpy(duplicate.as.file_offer_create.filename, "second.txt");
    RelayPolicyEffects fx = effects();
    relay_policy_handle(policy, alice, &duplicate, 10, &fx);
    CapturedEffect* rejected = find_effect(alice, RELAY_MESSAGE_ACTION_REJECTED, 0);
    TEST_ASSERT_NOT_NULL(rejected);
    TEST_ASSERT_EQUAL(RELAY_MESSAGE_FILE_OFFER_CREATE,
        rejected->message.as.action_rejected.rejected_type);
    TEST_ASSERT_EQUAL_UINT64(77, rejected->message.as.action_rejected.correlation_id);
    TEST_ASSERT_EQUAL_size_t(1, relay_policy_file_offer_count(policy));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_file_offer_freezes_recipients_and_routes_chunks_only_to_acceptors);
    RUN_TEST(test_offer_expiry_declines_when_nobody_accepts);
    RUN_TEST(test_invited_disconnect_closes_window_early);
    RUN_TEST(test_slow_recipient_failure_isolated_from_other_delivery);
    RUN_TEST(test_sender_disconnect_cancels_every_active_delivery);
    RUN_TEST(test_chat_attribution_comes_from_participant_identity);
    RUN_TEST(test_failed_last_delivery_cancels_sender_before_more_chunks);
    RUN_TEST(test_duplicate_active_request_identity_is_rejected);
    return UNITY_END();
}
