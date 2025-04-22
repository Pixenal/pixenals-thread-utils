/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#pragma once
#include "alloc.h"

#define PIX_THREAD_MAX_THREADS 32
#define PIX_THREAD_MAX_SUB_MAPPING_JOBS 8
#define PIX_THREAD_MAX_MAPPING_JOBS 3

void pixthThreadPoolInit(
	void **pThreadPool,
	int32_t *ThreadCount,
	const PixalcFPtrs *pAlloc
);
void pixthJobStackGetJob(void *pThreadPool, void **ppJob);
PixErr pixthJobStackPushJobs(
	void *pThreadPool,
	int32_t jobAmount,
	void **ppJobHandles,
	PixErr(*pJob)(void *),
	void **pJobArgs
);
bool pixthGetAndDoJob(void *pThreadPool);
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
PixErr pixthWaitForJobsIntern(
	void *pThreadPool,
	int32_t jobCount,
	void **ppJobs,
	bool wait,
	bool *pDone
);
PixErr pixthGetJobErr(void *pThreadPool, void *pJobHandle, PixErr *pJobErr);
PixErr pixthJobHandleDestroy(void *pThreadPool, void **ppJobHandle);
