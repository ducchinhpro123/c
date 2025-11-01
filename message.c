#include "message.h"
#include <string.h>
#include <pthread.h>
#include <time.h>


void init_message_queue(MessageQueue *q)
{
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

void add_message(MessageQueue *q, const char *sender, const char *text)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count >= MAX_MESSAGES)
    {
        memmove(&q->messages[0], &q->messages[1], sizeof(Message)*(MAX_MESSAGES - 1));
        q->count = MAX_MESSAGES - 1;
    }
    Message *m = &q->messages[q->count++];
    strncpy(m->sender, sender, sizeof m->sender - 1);
    m->sender[sizeof m->sender - 1] = '\0';

    strncpy(m->text, text, sizeof m->text - 1);
    m->text[sizeof m->text - 1] = '\0';

    m->timestamp = (double)time(NULL);
    pthread_mutex_unlock(&q->mutex);
}

Message get_message(MessageQueue *q, int index)
{
    pthread_mutex_lock(&q->mutex);
    Message m = (index >= 0 && index < q->count) ? q->messages[index] : (Message){0};
    pthread_mutex_unlock(&q->mutex);
    return m;
}

int get_message_count(MessageQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    int c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}

