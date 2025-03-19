/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#pragma once
#include <stdint.h>

#include <uv_stucco.h>
#include <types.h>

#define MAX_THREADS 32
#define MAX_SUB_MAPPING_JOBS 8
#define MAX_MAPPING_JOBS 3

void stucThreadPoolInit(
	void **pThreadPool,
	I32 *ThreadCount,
	const StucAlloc *pAlloc
);
StucResult stucJobStackPushJobs(
	void *pThreadPool,
	I32 jobAmount,
	void **ppJobHandles,
	StucResult(*pJob)(void *),
	void **pJobArgs
);
bool stucGetAndDoJob(void *pThreadPool);
void stucMutexGet(void *pThreadPool, void **pMutex);
void stucMutexLock(void *pThreadPool, void *pMutex);
void stucMutexUnlock(void *pThreadPool, void *pMutex);
void stucMutexDestroy(void *pThreadPool, void *pMutex);
void stucBarrierGet(void *pThreadPool, void **ppBarrier, I32 jobCount);
bool stucBarrierWait(void *pThreadPool, void *pBarrier);
void stucBarrierDestroy(void *pThreadPool, void *pBarrier);
void stucThreadPoolDestroy(void *pThreadPool);
StucResult stucWaitForJobsIntern(
	void *pThreadPool,
	I32 jobCount,
	void **ppJobs,
	bool wait,
	bool *pDone
);
StucResult stucGetJobErr(void *pThreadPool, void *pJobHandle, StucResult *pJobErr);
StucResult stucJobHandleDestroy(void *pThreadPool, void **ppJobHandle);

void stucThreadPoolSetDefault(StucContext context);