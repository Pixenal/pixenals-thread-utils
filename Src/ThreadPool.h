#pragma once
#include <stdint.h>
#include <UvStucco.h>

#define MAX_THREADS 32
#define MAX_SUB_MAPPING_JOBS 4
#define MAX_MAPPING_JOBS 3

void stucThreadPoolInit(void **pThreadPool, int32_t *ThreadCount, StucAlloc *pAlloc);
int32_t stucJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void **ppJobHandles,
                             StucResult (*pJob)(void *), void **pJobArgs);
bool stucGetAndDoJob(void *pThreadPool);
void stucMutexGet(void *pThreadPool, void **pMutex);
void stucMutexLock(void *pThreadPool, void *pMutex);
void stucMutexUnlock(void *pThreadPool, void *pMutex);
void stucMutexDestroy(void *pThreadPool, void *pMutex);
void stucBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount);
bool stucBarrierWait(void *pThreadPool, void *pBarrier);
void stucBarrierDestroy(void *pThreadPool, void *pBarrier);
void stucThreadPoolDestroy(void *pThreadPool);
StucResult stucWaitForJobsIntern(void *pThreadPool, int32_t jobCount, void **ppJobs);
StucResult stucGetJobErr(void *pThreadPool, void *pJobHandle, StucResult *pJobErr);
StucResult stucJobHandleDestroy(void *pThreadPool, void **ppJobHandle);

void stucThreadPoolSetDefault(StucContext context);