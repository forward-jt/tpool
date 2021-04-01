#include "at_queue.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

struct at_queue_s {
    atomic_uint rind, wind;
    uint32_t mask;
    const void *buffer[0];
};

at_queue_t at_queue_create(uint32_t min_len)
{
    uint32_t buf_len = min_len;
    if (buf_len > MAX_BUF_LEN)
        return NULL;
    else if (buf_len & (buf_len - 1))
        buf_len = 1U << (32 - __builtin_clz(buf_len));

    at_queue_t que = (at_queue_t) malloc(sizeof(struct at_queue_s) + sizeof(void *) * buf_len);
    if (que) {
        atomic_init(&que->rind, 0);
        atomic_init(&que->wind, 0);
        que->mask = buf_len - 1;
        memset(que->buffer, 0, sizeof(void *) * buf_len);
    }

    return que;
}

bool at_queue_push(at_queue_t que, const void *ele)
{
    do {
        uint32_t last_r = atomic_load(&que->rind);
        uint32_t last_w = atomic_load(&que->wind);
        uint32_t new_w = (last_w + 1) & que->mask;

        if (new_w == last_r)
            return false;

        if(atomic_compare_exchange_weak(&que->wind, &last_w, new_w)) {
            while (que->buffer[last_w])
                ;
            que->buffer[last_w] = ele;
            return true;
        }
    } while (1);
}

void *at_queue_pop(at_queue_t que)
{
    do {
        uint32_t last_w = atomic_load(&que->wind);
        uint32_t last_r = atomic_load(&que->rind);
        uint32_t new_r = (last_r + 1) & que->mask;

        if (last_r == last_w)
            return NULL;

        if (atomic_compare_exchange_weak(&que->rind, &last_r, new_r)) {
            const void *ele;
            while (!(ele = que->buffer[last_r]))
                ;
            que->buffer[last_r] = NULL;
            return (void *) ele;
        }
    } while(1);
}

void at_queue_destroy(at_queue_t que)
{
    if (que)
        free(que);
}