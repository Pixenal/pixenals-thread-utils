/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#pragma once
#include "../../pixenals-alloc-utils/include/pixenals_alloc_utils.h"

#define PIX_THREAD_MAX_THREADS 32
//TODO these are stuc settings, move them to stuc
//TODO also add an option to adjust thread count (within max) to stuc ui
#define PIX_THREAD_MAX_SUB_MAPPING_JOBS 6
#define PIX_THREAD_MAX_MAPPING_JOBS 3

typedef struct PixthPlatform *PixthPlatform;

PixErr pixthThreadPoolInit(
	void **pThreadPool,
	int32_t *ThreadCount,
	const PixalcFPtrs *pAlloc
);
void *pixthArgGet(void *pThreadPool, I32 idx);
PixErr pixthThreadPoolInitPlatform(
	const PixalcFPtrs *pAlloc,
	PixthPlatform *ppPlatform,
	int32_t *pThreadCount,
	int32_t (*fpLoop)(void *)
);
void pixthJobStackGetJob(void *pThreadPool, void **ppJob, int32_t threadId);
PixErr pixthJobStackPushJobs(
	void *pThreadPool,
	int32_t jobAmount,
	void **ppJobHandles,
	PixErr(*pJob)(void *),
	void **pJobArgs
);
bool pixthGetAndDoJob(void *pThreadPool, int32_t id);
void pixthMutexGet(void *pThreadPool, void **pMutex);
void pixthMutexLock(void *pThreadPool, void *pMutex);
void pixthMutexUnlock(void *pThreadPool, void *pMutex);
void pixthMutexDestroy(void *pThreadPool, void *pMutex);
/*
void stucBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount);
bool stucBarrierWait(void *pThreadPool, void *pBarrier);
void stucBarrierDestroy(void *pThreadPool, void *pBarrier);
*/
void pixthThreadPoolDestroy(void *pThreadPool);
void pixthPlatformDestroy(
	PixthPlatform pPlatform,
	I32 threadCount
);
PixErr pixthWaitForJobsIntern(
	void *pThreadPool,
	int32_t jobCount,
	void **ppJobs,
	bool wait,
	bool *pDone
);
PixErr pixthGetJobErr(void *pThreadPool, void *pJobHandle, PixErr *pJobErr);
PixErr pixthJobHandleDestroy(void *pThreadPool, void **ppJobHandle);
void pixthSleep(void *pThreadPool, int32_t nanosec);
