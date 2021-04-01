#ifndef AT_QUEUE_H
#define AT_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * The maximum size supported by at_queue.
 */
#define MAX_BUF_LEN 0x80000000U

typedef struct at_queue_s * at_queue_t;

/*
 * Creates an at_queue whose internal buffer has at least
 * min_len entries.
 */
at_queue_t at_queue_create(uint32_t min_len);

/*
 * Push an element to given at_queue, true is returned while
 * element is pushed successfully, false while queue is full.
 */
bool at_queue_push(at_queue_t que, const void *ele);

/*
 * Pop an element from given at_queue if there is any element
 * in it.
 */
void *at_queue_pop(at_queue_t que);

/*
 * Destroy given at_queue, this function will not deal with
 * elements left in queue, the one that invokes this function
 * should deal with those elements first, in order to prevent
 * memory leak.
 */
void at_queue_destroy(at_queue_t que);

#endif