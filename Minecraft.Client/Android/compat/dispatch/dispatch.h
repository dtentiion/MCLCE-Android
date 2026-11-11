#pragma once

// The main queue is the thread the app's frame loop runs on. Work given to
// it runs at the top of that thread's next frame.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcle_dispatch_queue *dispatch_queue_t;
typedef void (*dispatch_function_t)(void *);

dispatch_queue_t dispatch_get_main_queue(void);
void dispatch_async_f(dispatch_queue_t queue, void *context, dispatch_function_t work);
void dispatch_sync_f(dispatch_queue_t queue, void *context, dispatch_function_t work);

// pthread.h has this on iOS: nonzero on the main queue's thread
int pthread_main_np(void);

#ifdef __cplusplus
}
#endif
