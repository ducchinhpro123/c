#include "unity.h"
#include "packet_queue.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

void setUp(void) {}
void tearDown(void) {}

void test_pq_init(void)
{
    PacketQueue pq;
    pq_init(&pq);

    TEST_ASSERT_NULL(pq.head);
    TEST_ASSERT_NULL(pq.tail);
    TEST_ASSERT_EQUAL(0, pq.count);
    TEST_ASSERT_EQUAL(0, pq.total_data_size);
}

void test_pq_push_pop_single(void)
{
    PacketQueue pq;
    pq_init(&pq);

    const char* data = "Hello, World!";
    size_t len = strlen(data);

    int result = pq_push(&pq, 1, data, (uint32_t)len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, pq.count);
    TEST_ASSERT_EQUAL(len, pq.total_data_size);

    Packet* pkt = pq_pop(&pq);
    TEST_ASSERT_NOT_NULL(pkt);
    TEST_ASSERT_EQUAL(1, pkt->type);
    TEST_ASSERT_EQUAL(len, pkt->length);
    TEST_ASSERT_EQUAL_MEMORY(data, pkt->data, len);

    pq_free_packet(pkt);
    TEST_ASSERT_EQUAL(0, pq.count);
    TEST_ASSERT_EQUAL(0, pq.total_data_size);
}

void test_pq_push_zero_copy(void)
{
    PacketQueue pq;
    pq_init(&pq);

    // Allocate data that will be owned by the queue
    size_t len = 1024;
    void* data = malloc(len);
    TEST_ASSERT_NOT_NULL(data);
    memset(data, 'X', len);

    int result = pq_push_zero_copy(&pq, 2, data, (uint32_t)len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, pq.count);
    TEST_ASSERT_EQUAL(len, pq.total_data_size);

    Packet* pkt = pq_pop(&pq);
    TEST_ASSERT_NOT_NULL(pkt);
    TEST_ASSERT_EQUAL(2, pkt->type);
    TEST_ASSERT_EQUAL(len, pkt->length);
    // Data pointer should be the same (zero-copy)
    TEST_ASSERT_EQUAL_PTR(data, pkt->data);

    pq_free_packet(pkt);  // This frees the original data
}

void test_pq_multiple_packets_fifo(void)
{
    PacketQueue pq;
    pq_init(&pq);

    // Push 3 packets
    pq_push(&pq, 1, "first", 5);
    pq_push(&pq, 2, "second", 6);
    pq_push(&pq, 3, "third", 5);

    TEST_ASSERT_EQUAL(3, pq.count);
    TEST_ASSERT_EQUAL(16, pq.total_data_size);

    // Pop and verify FIFO order
    Packet* pkt1 = pq_pop(&pq);
    TEST_ASSERT_EQUAL(1, pkt1->type);
    TEST_ASSERT_EQUAL_STRING_LEN("first", pkt1->data, 5);
    pq_free_packet(pkt1);

    Packet* pkt2 = pq_pop(&pq);
    TEST_ASSERT_EQUAL(2, pkt2->type);
    TEST_ASSERT_EQUAL_STRING_LEN("second", pkt2->data, 6);
    pq_free_packet(pkt2);

    Packet* pkt3 = pq_pop(&pq);
    TEST_ASSERT_EQUAL(3, pkt3->type);
    TEST_ASSERT_EQUAL_STRING_LEN("third", pkt3->data, 5);
    pq_free_packet(pkt3);

    TEST_ASSERT_EQUAL(0, pq.count);
}

void test_pq_get_data_size(void)
{
    PacketQueue pq;
    pq_init(&pq);

    TEST_ASSERT_EQUAL(0, pq_get_data_size(&pq));

    pq_push(&pq, 1, "test", 4);
    TEST_ASSERT_EQUAL(4, pq_get_data_size(&pq));

    pq_push(&pq, 1, "longer data here", 16);
    TEST_ASSERT_EQUAL(20, pq_get_data_size(&pq));

    Packet* pkt = pq_pop(&pq);
    pq_free_packet(pkt);
    TEST_ASSERT_EQUAL(16, pq_get_data_size(&pq));

    pkt = pq_pop(&pq);
    pq_free_packet(pkt);
    TEST_ASSERT_EQUAL(0, pq_get_data_size(&pq));
}

void test_pq_empty_data(void)
{
    PacketQueue pq;
    pq_init(&pq);

    int result = pq_push(&pq, 1, NULL, 0);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(1, pq.count);
    TEST_ASSERT_EQUAL(0, pq.total_data_size);

    Packet* pkt = pq_pop(&pq);
    TEST_ASSERT_NOT_NULL(pkt);
    TEST_ASSERT_EQUAL(1, pkt->type);
    TEST_ASSERT_EQUAL(0, pkt->length);
    TEST_ASSERT_NULL(pkt->data);

    pq_free_packet(pkt);
}

// Thread function for producer
typedef struct {
    PacketQueue* pq;
    int start;
    int count;
} ProducerArgs;

void* producer_thread(void* arg)
{
    ProducerArgs* args = (ProducerArgs*)arg;
    for (int i = 0; i < args->count; i++) {
        char data[64];
        snprintf(data, sizeof(data), "packet_%d_%d", args->start, i);
        pq_push(args->pq, (uint8_t)(args->start % 256), data, (uint32_t)strlen(data));
    }
    return NULL;
}

void test_pq_concurrent_producers(void)
{
    PacketQueue pq;
    pq_init(&pq);

    const int NUM_PRODUCERS = 4;
    const int PACKETS_PER_PRODUCER = 100;

    pthread_t threads[NUM_PRODUCERS];
    ProducerArgs args[NUM_PRODUCERS];

    // Start producer threads
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        args[i].pq = &pq;
        args[i].start = i * PACKETS_PER_PRODUCER;
        args[i].count = PACKETS_PER_PRODUCER;
        pthread_create(&threads[i], NULL, producer_thread, &args[i]);
    }

    // Wait for all producers
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
    }

    // Verify count
    TEST_ASSERT_EQUAL(NUM_PRODUCERS * PACKETS_PER_PRODUCER, pq.count);

    // Drain queue
    int consumed = 0;
    while (pq.count > 0) {
        Packet* pkt = pq_pop(&pq);
        pq_free_packet(pkt);
        consumed++;
    }

    TEST_ASSERT_EQUAL(NUM_PRODUCERS * PACKETS_PER_PRODUCER, consumed);
    TEST_ASSERT_EQUAL(0, pq.count);
    TEST_ASSERT_EQUAL(0, pq.total_data_size);
}

void test_pq_large_packet(void)
{
    PacketQueue pq;
    pq_init(&pq);

    // Test with 1MB packet (new chunk size)
    size_t len = 1024 * 1024;
    void* data = malloc(len);
    TEST_ASSERT_NOT_NULL(data);
    memset(data, 'Y', len);

    int result = pq_push_zero_copy(&pq, 5, data, (uint32_t)len);
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL(len, pq.total_data_size);

    Packet* pkt = pq_pop(&pq);
    TEST_ASSERT_NOT_NULL(pkt);
    TEST_ASSERT_EQUAL(len, pkt->length);
    TEST_ASSERT_EQUAL_PTR(data, pkt->data);

    pq_free_packet(pkt);
}

double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void test_pq_push_performance(void)
{
    PacketQueue pq;
    pq_init(&pq);

    const int NUM_ITERATIONS = 10000;
    const size_t PACKET_SIZE = 1024;
    char* data = malloc(PACKET_SIZE);
    memset(data, 'Z', PACKET_SIZE);

    double start = get_time_sec();
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        pq_push(&pq, 1, data, PACKET_SIZE);
    }
    double push_time = get_time_sec() - start;

    double push_rate = NUM_ITERATIONS / push_time;
    printf("\nPush performance: %.0f packets/sec (%.2f MB/s)\n",
           push_rate, (push_rate * PACKET_SIZE) / (1024 * 1024));

    // Pop all
    start = get_time_sec();
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        Packet* pkt = pq_pop(&pq);
        pq_free_packet(pkt);
    }
    double pop_time = get_time_sec() - start;

    double pop_rate = NUM_ITERATIONS / pop_time;
    printf("Pop performance: %.0f packets/sec (%.2f MB/s)\n",
           pop_rate, (pop_rate * PACKET_SIZE) / (1024 * 1024));

    free(data);

    // Performance should be reasonable (at least 10k ops/sec)
    TEST_ASSERT_GREATER_THAN(10000, push_rate);
    TEST_ASSERT_GREATER_THAN(10000, pop_rate);
}

void test_pq_zero_copy_performance(void)
{
    PacketQueue pq;
    pq_init(&pq);

    const int NUM_ITERATIONS = 1000;
    const size_t PACKET_SIZE = 1024 * 1024;  // 1MB packets

    double start = get_time_sec();
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        void* data = malloc(PACKET_SIZE);
        memset(data, 'A', PACKET_SIZE);  // Simulate filling buffer
        pq_push_zero_copy(&pq, 1, data, PACKET_SIZE);
    }
    double push_time = get_time_sec() - start;

    double throughput = (NUM_ITERATIONS * PACKET_SIZE) / (push_time * 1024 * 1024);
    printf("\nZero-copy push throughput: %.2f MB/s\n", throughput);

    // Pop all
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        Packet* pkt = pq_pop(&pq);
        pq_free_packet(pkt);
    }

    // Zero-copy should achieve at least 500 MB/s (memory speed limited)
    TEST_ASSERT_GREATER_THAN(500, throughput);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pq_init);
    RUN_TEST(test_pq_push_pop_single);
    RUN_TEST(test_pq_push_zero_copy);
    RUN_TEST(test_pq_multiple_packets_fifo);
    RUN_TEST(test_pq_get_data_size);
    RUN_TEST(test_pq_empty_data);
    RUN_TEST(test_pq_concurrent_producers);
    RUN_TEST(test_pq_large_packet);
    RUN_TEST(test_pq_push_performance);
    RUN_TEST(test_pq_zero_copy_performance);
    return UNITY_END();
}
