#pragma once
#include <stdint.h>
#include <RUVM.h>

#define MAX_THREADS 8

void ruvmInitThreadPool(void **pThreadPool, int32_t *ThreadCount, RuvmAllocator *pAlloc);
void ruvmJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **jobArgs);
int32_t ruvmJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void (*job)(void *), void **pJobArgs);
void ruvmMutexGet(void *pThreadPool, void **pMutex);
void ruvmMutexLock(void *pThreadPool, void *pMutex);
void ruvmMutexUnlock(void *pThreadPool, void *pMutex);
void ruvmMutexDestroy(void *pThreadPool, void *pMutex);
void ruvmDestroyThreadPool(void *pThreadPool);

void ruvmThreadPoolSetDefault(RuvmContext context);
