#pragma once
#include <stdint.h>
#include <RUVM.h>

#define MAX_THREADS 8

void uvsThreadPoolInit(void **pThreadPool, int32_t *ThreadCount, RuvmAlloc *pAlloc);
void uvsJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **jobArgs);
int32_t uvsJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void (*job)(void *), void **pJobArgs);
bool uvsGetAndDoJob(void *pThreadPool);
void uvsMutexGet(void *pThreadPool, void **pMutex);
void uvsMutexLock(void *pThreadPool, void *pMutex);
void uvsMutexUnlock(void *pThreadPool, void *pMutex);
void uvsMutexDestroy(void *pThreadPool, void *pMutex);
void uvsBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount);
bool uvsBarrierWait(void *pThreadPool, void *pBarrier);
void uvsBarrierDestroy(void *pThreadPool, void *pBarrier);
void uvsThreadPoolDestroy(void *pThreadPool);

void uvsThreadPoolSetDefault(RuvmContext context);
