#pragma once
#include <stdint.h>
#include <RUVM.h>

#define MAX_THREADS 1

void ruvmThreadPoolInit(void **pThreadPool, int32_t *ThreadCount, RuvmAlloc *pAlloc);
void ruvmJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **jobArgs);
int32_t ruvmJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void (*job)(void *), void **pJobArgs);
void ruvmMutexGet(void *pThreadPool, void **pMutex);
void ruvmMutexLock(void *pThreadPool, void *pMutex);
void ruvmMutexUnlock(void *pThreadPool, void *pMutex);
void ruvmMutexDestroy(void *pThreadPool, void *pMutex);
void ruvmBarrierGet(void *pThreadPool, void **ppBarrier);
bool ruvmBarrierWait(void *pThreadPool, void *pBarrier);
void ruvmBarrierDestroy(void *pThreadPool, void *pBarrier);
void ruvmThreadPoolDestroy(void *pThreadPool);

void ruvmThreadPoolSetDefault(RuvmContext context);
