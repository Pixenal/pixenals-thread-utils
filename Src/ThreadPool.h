#pragma once
#include <stdint.h>
#include <UvStucco.h>

#define MAX_THREADS 8

void stucThreadPoolInit(void **pThreadPool, int32_t *ThreadCount, StucAlloc *pAlloc);
void stucJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **jobArgs);
int32_t stucJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void (*job)(void *), void **pJobArgs);
bool stucGetAndDoJob(void *pThreadPool);
void stucMutexGet(void *pThreadPool, void **pMutex);
void stucMutexLock(void *pThreadPool, void *pMutex);
void stucMutexUnlock(void *pThreadPool, void *pMutex);
void stucMutexDestroy(void *pThreadPool, void *pMutex);
void stucBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount);
bool stucBarrierWait(void *pThreadPool, void *pBarrier);
void stucBarrierDestroy(void *pThreadPool, void *pBarrier);
void stucThreadPoolDestroy(void *pThreadPool);

void stucThreadPoolSetDefault(StucContext context);