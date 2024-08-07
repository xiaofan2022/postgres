/*-------------------------------------------------------------------------
 *
 * IDENTIFICATION
 *	  walminer_thread.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "walminer_decode.h"
#include <pthread.h>


typedef struct ThreadInfo
{
    pthread_t           search_thread;
    pthread_t           decode_thread;          /* 已废弃 */
    pthread_mutex_t     mutex;
    pthread_mutex_t     mutex_debug_out;
    pthread_mutex_t     mutex_search_write;
}ThreadInfo;

ThreadInfo  thread_info;

void
init_thread_info(void)
{
    memset(&thread_info, 0, sizeof(ThreadInfo));
    thread_info.decode_thread = 0;
    thread_info.search_thread = 0;
    pthread_mutex_init(&thread_info.mutex, NULL);
    pthread_mutex_init(&thread_info.mutex_debug_out, NULL);
    pthread_mutex_init(&thread_info.mutex_search_write, NULL);
}

void
lock_walminer_thread(int  locktype)
{
    pthread_mutex_t     *mutex = NULL;

    if(1 == locktype)
    {
        mutex = &thread_info.mutex;
    }
    else if(2 == locktype)
    {
        mutex = &thread_info.mutex_debug_out;
    }
    else if(3 == locktype)
    {
        mutex = &thread_info.mutex_search_write;
    }
    else
    {
        return;
    }
    pthread_mutex_lock(mutex);
}

void
unlock_walminer_thread(int  locktype)
{
    pthread_mutex_t     *mutex = NULL;

    if(1 == locktype)
    {
        mutex = &thread_info.mutex;
    }
    else if(2 == locktype)
    {
        mutex = &thread_info.mutex_debug_out;
    }
    else if(3 == locktype)
    {
        mutex = &thread_info.mutex_search_write;
    }
    else
    {
        return;
    }

    pthread_mutex_unlock(mutex);
}

void
create_walminer_thread(void*(*start_rtn)(void*), bool issearch)
{
    int         result = 0;
    pthread_t   *temp_t = NULL;

    if(issearch)
        temp_t = &thread_info.search_thread;
    else
        temp_t = &thread_info.decode_thread;
    result = pthread_create(temp_t, NULL, start_rtn, NULL);

    if(0 != result)
    {
        elog(ERROR, "Can not create walminer thread %d", issearch);
    }
}


void wait_thread(void)
{
    if(0 != thread_info.search_thread)
        pthread_join(thread_info.search_thread, NULL);
}