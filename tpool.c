#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#include "at_queue.h"

enum __future_flags {
    __FUTURE_RUNNING = 01,
    __FUTURE_FINISHED = 02,
    __FUTURE_TIMEOUT = 04,
    __FUTURE_CANCELLED = 010,
    __FUTURE_DESTROYED = 020,
};

typedef struct __threadtask {
    void *(*func)(void *);
    void *arg;
    struct __tpool_future *future;
} threadtask_t;

struct __tpool_future {
    int flag;
    void *result;
    pthread_mutex_t mutex;
    pthread_cond_t cond_finished;
};

struct __threadpool {
    size_t count;
    size_t rrind;
    pthread_t *workers;
    at_queue_t jobqueue;
};

static struct __tpool_future *tpool_future_create(void)
{
    struct __tpool_future *future = malloc(sizeof(struct __tpool_future));
    if (future) {
        future->flag = 0;
        future->result = NULL;
        pthread_mutex_init(&future->mutex, NULL);
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_cond_init(&future->cond_finished, &attr);
        pthread_condattr_destroy(&attr);
    }
    return future;
}

int tpool_future_destroy(struct __tpool_future *future)
{
    if (future) {
        pthread_mutex_lock(&future->mutex);
        if (future->flag & __FUTURE_FINISHED ||
            future->flag & __FUTURE_CANCELLED) {
            pthread_mutex_unlock(&future->mutex);
            pthread_mutex_destroy(&future->mutex);
            pthread_cond_destroy(&future->cond_finished);
            free(future);
        } else {
            future->flag |= __FUTURE_DESTROYED;
            pthread_mutex_unlock(&future->mutex);
        }
    }
    return 0;
}

void *tpool_future_get(struct __tpool_future *future, unsigned int seconds)
{
    pthread_mutex_lock(&future->mutex);
    /* turn off the timeout bit set previously */
    future->flag &= ~__FUTURE_TIMEOUT;
    while ((future->flag & __FUTURE_FINISHED) == 0) {
        if (seconds) {
            struct timespec expire_time;
            clock_gettime(CLOCK_MONOTONIC, &expire_time);
            expire_time.tv_sec += seconds;
            int status = pthread_cond_timedwait(&future->cond_finished,
                                                &future->mutex, &expire_time);
            if (status == ETIMEDOUT) {
                future->flag |= __FUTURE_TIMEOUT;
                pthread_mutex_unlock(&future->mutex);
                return NULL;
            }
        } else
            pthread_cond_wait(&future->cond_finished, &future->mutex);
    }

    pthread_mutex_unlock(&future->mutex);
    return future->result;
}

static void jobqueue_destroy(at_queue_t jobqueue)
{
    threadtask_t *tmp = (threadtask_t *) at_queue_pop(jobqueue);
    while (tmp) {
        pthread_mutex_lock(&tmp->future->mutex);
        if (tmp->future->flag & __FUTURE_DESTROYED) {
            pthread_mutex_unlock(&tmp->future->mutex);
            pthread_mutex_destroy(&tmp->future->mutex);
            pthread_cond_destroy(&tmp->future->cond_finished);
            free(tmp->future);
        } else {
            tmp->future->flag |= __FUTURE_CANCELLED;
            pthread_mutex_unlock(&tmp->future->mutex);
        }
        free(tmp);
        tmp = (threadtask_t *) at_queue_pop(jobqueue);
    }

    at_queue_destroy(jobqueue);
}

static void __jobqueue_fetch_cleanup(void *arg)
{
    pthread_mutex_t *mutex = (pthread_mutex_t *) arg;
    pthread_mutex_unlock(mutex);
}

static void *jobqueue_fetch(void *queue)
{
    at_queue_t jobqueue = (at_queue_t) queue;
    threadtask_t *task;
    int old_state;

    while (1) {
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
        pthread_testcancel();

        while (!(task = (threadtask_t *) at_queue_pop(jobqueue)))
            usleep(10);
        
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);

        if (task->func) {
            pthread_mutex_lock(&task->future->mutex);
            if (task->future->flag & __FUTURE_CANCELLED) {
                pthread_mutex_unlock(&task->future->mutex);
                free(task);
                continue;
            } else {
                task->future->flag |= __FUTURE_RUNNING;
                pthread_mutex_unlock(&task->future->mutex);
            }

            void *ret_value = task->func(task->arg);
            pthread_mutex_lock(&task->future->mutex);
            if (task->future->flag & __FUTURE_DESTROYED) {
                pthread_mutex_unlock(&task->future->mutex);
                pthread_mutex_destroy(&task->future->mutex);
                pthread_cond_destroy(&task->future->cond_finished);
                free(task->future);
            } else {
                task->future->flag |= __FUTURE_FINISHED;
                task->future->result = ret_value;
                pthread_cond_broadcast(&task->future->cond_finished);
                pthread_mutex_unlock(&task->future->mutex);
            }
            free(task);
        } else {
            pthread_mutex_destroy(&task->future->mutex);
            pthread_cond_destroy(&task->future->cond_finished);
            free(task->future);
            free(task);
            break;
        }
    }

    pthread_exit(NULL);
}

struct __threadpool *tpool_create(size_t count)
{
    struct __threadpool *pool = malloc(sizeof(struct __threadpool));
    at_queue_t jobqueue = at_queue_create(64);
    if (!pool || !jobqueue) {
        if (pool)
            free(pool);
        if (jobqueue)
            jobqueue_destroy(jobqueue);
        return NULL;
    }

    pool->count = count;
    pool->jobqueue = jobqueue;
    pool->workers = malloc(count * sizeof(pthread_t));
    if (pool->workers) {
        for (int i = 0; i < count; i++) {
            if (pthread_create(&pool->workers[i], NULL, jobqueue_fetch,
                               (void *) pool->jobqueue)) {
                for (int j = 0; j < i; j++)
                    pthread_cancel(pool->workers[j]);
                for (int j = 0; j < i; j++)
                    pthread_join(pool->workers[j], NULL);
                goto failed_cleanup;
            }
        }
        return pool;
    }

failed_cleanup:
    if (pool->workers)
        free(pool->workers);
    jobqueue_destroy(jobqueue);
    free(pool);
    return NULL;
}

struct __tpool_future *tpool_apply(struct __threadpool *pool,
                                   void *(*func)(void *),
                                   void *arg)
{
    at_queue_t jobqueue = pool->jobqueue;
    
    threadtask_t *new_task = calloc(1, sizeof(threadtask_t));
    struct __tpool_future *future = tpool_future_create();
    if (new_task && future) {
        new_task->func = func, new_task->arg = arg, new_task->future = future;
        while (!at_queue_push(jobqueue, new_task))
            usleep(10);
    } else if (new_task) {
        free(new_task);
        return NULL;
    } else if (future) {
        tpool_future_destroy(future);
        return NULL;
    }
    return future;
}

int tpool_join(struct __threadpool *pool)
{
    size_t num_threads = pool->count;
    for (int i = 0; i < num_threads; i++)
        tpool_apply(pool, NULL, NULL);
    for (int i = 0; i < num_threads; i++)
        pthread_join(pool->workers[i], NULL);
    jobqueue_destroy(pool->jobqueue);
    free(pool->workers);
    free(pool);
    return 0;
}