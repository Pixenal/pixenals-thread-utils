/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#pragma once
#include "../../pixenals-alloc-utils/include/pixenals_alloc_utils.h"

#define PIXTH_MAX_THREADS 16
#define PIXTH_NO_JOB_THRES (PIXTH_MAX_THREADS * 4)

//TODO these are stuc settings, move them to stuc
//TODO also add an option to adjust thread count (within max) to stuc ui
#define PIXTH_MAX_SUB_MAPPING_JOBS 6

typedef struct PixthPlatform *PixthPlatform;

#define PIXTH_DEQUE_SIZE 64

#define PIXTH_CACHELINE_SIZE 64//using a static estimate for now

typedef struct PixthJob {
	PixErr (*pJob) (void *, I32);
	void *pArgs;
	uint64_t hash; //hash of ptr and time of push
	char padding[PIXTH_CACHELINE_SIZE - 24 - 4];
	volatile PixErr err;
} PixthJob;

//"Dynamic Circular Work-Stealing Deque" Chase, Lev 2005
//https://dl.acm.org/doi/10.1145/1073970.1073974 
//
//the dynamic part of this deque is currently not implemented,
//mem is kept on the stack rn, and so the deque doesn't grow.
typedef struct PixthJobDeque {
	int64_t topAprox;
	volatile int64_t bottom;
	char paddingA[PIXTH_CACHELINE_SIZE];
	PixthJob *deque[PIXTH_DEQUE_SIZE];
	//last element of deque is not used, so no need for padding before top
	volatile int64_t top;
} PixthJobDeque;

struct PixthPoolCtx;

typedef enum PixthLogAction {
	PIX_THREAD_ACTION_NONE,
	PIX_THREAD_ACTION_PUSH,
	PIX_THREAD_ACTION_PUSH_FAIL,
	PIX_THREAD_ACTION_POP,
	PIX_THREAD_ACTION_STEAL,
	PIX_THREAD_ACTION_FINISH,
	PIX_THREAD_ACTION_SET_WAKE,
	PIX_THREAD_ACTION_WAKE,
	PIX_THREAD_ACTION_SET_SLEEP,
	PIX_THREAD_ACTION_SLEEP,
	PIX_THREAD_ACTION_PAUSE,
	PIX_THREAD_ACTION_STOP,
	PIX_THREAD_ACTION_ENUM_SIZE
} PixthLogAction;

typedef struct PixthLogEntry {
	int64_t timeS;
	uint64_t job;
	int32_t timeNs;
	int16_t action;
	int16_t stoleFrom;
} PixthLogEntry;

typedef struct PixthLog {
	PixthLogEntry *pArr;
	I32 size;
	I32 count;
} PixthLog;

typedef struct PixthThreadArgs {
	struct PixthPoolCtx *pCtx;
	int32_t id;
	PixthLog log;
	PixthJobDeque jobs;
	char padding[PIXTH_CACHELINE_SIZE];//last element of jobs is shared, so pad here
} PixthThreadArgs;

typedef enum PixthPoolDirective {
	THREAD_NONE,
	THREAD_SLEEP,
	THREAD_WAKE,
	THREAD_PAUSE,
	THREAD_STOP
} PixthPoolDirective;

union PixthPoolCore {
	PixthPlatform pPlatform;
	PixalcFPtrs *pAlloc;
};

typedef struct PixthPoolCtx {
	union PixthPoolCore core;
	int32_t threadCount;
	bool logging;
	char paddingA[PIXTH_CACHELINE_SIZE];
	PixthThreadArgs args[PIXTH_MAX_THREADS];
	volatile PixthPoolDirective directive;
	volatile int32_t paused;
	char paddingB[PIXTH_CACHELINE_SIZE];
} PixthPoolCtx;


PixErr pixthThreadPoolInitIntern(const PixalcFPtrs *pAlloc, PixthPoolCtx *pCtx);
void *pixthArgGet(PixthPoolCtx *pCtx, int32_t idx);
int32_t pixthThreadCountGet();
PixErr pixthThreadPoolInitPlatform(
	const PixalcFPtrs *pAlloc,
	PixthPlatform *ppPlatform,
	int32_t threadCount,
	int32_t (*fpLoop)(void *)
);
PixErr pixthJobStackGetJob(PixthPoolCtx *pCtx, void **ppJob, int32_t threadId, uint32_t tick);
void pixthJobsInit(PixthJob *pJobs, int32_t count, PixErr(*func)(void *, int32_t), void **ppArgs);
PixErr pixthJobStackPushJobs(
	PixthPoolCtx *pCtx,
	int32_t id,
	int32_t jobCount,
	PixthJob *pJobs
);
void pixthMutexGet(PixthPoolCtx *pCtx, void **pMutex);
void pixthMutexLock(PixthPoolCtx *pCtx, void *pMutex);
void pixthMutexUnlock(PixthPoolCtx *pCtx, void *pMutex);
void pixthMutexDestroy(PixthPoolCtx *pCtx, void *pMutex);
int32_t pixthAtomicSwapI32(PixthPoolCtx *pCtx, volatile int32_t *ptr, int32_t val);
int64_t pixthAtomicSwapI64(PixthPoolCtx *pCtx, volatile int64_t *ptr, int64_t val);
int32_t pixthAtomicCmpAndSwapI32(
	PixthPoolCtx *pCtx,
	volatile int32_t *ptr,
	int32_t cmp,
	int32_t val
);
int64_t pixthAtomicCmpAndSwapI64(
	PixthPoolCtx *pCtx,
	volatile int64_t *ptr,
	int64_t cmp,
	int64_t val
);
PixErr pixthThreadPoolLogDump(PixthPoolCtx *pCtx, PixtyI8Arr *pLog);
void pixthThreadPoolDestroy(PixthPoolCtx *pCtx);
void pixthPlatformDestroy(
	PixthPlatform pPlatform,
	I32 threadCount
);
PixErr pixthWaitForJobs(
	PixthPoolCtx *pCtx,
	int32_t jobCount,
	PixthJob *pJobs,
	I32 id,
	bool wait,
	bool *pDone
);
PixErr pixthGetJobErr(PixthPoolCtx *pCtx, PixthJob *pJobHandle, PixErr *pJobErr);
void pixthSleep(PixthPoolCtx *pCtx, int32_t nanosec);

static inline
PixErr pixthThreadPoolInit(
	PixthPoolCtx *pCtx,
	int32_t *pThreadCount,
	const PixalcFPtrs *pAlloc,
	bool logging
) {
	PixErr err = PIX_ERR_SUCCESS;
	*pCtx = (PixthPoolCtx) {
		.directive = THREAD_SLEEP,
		.logging = logging
	};
	for (int32_t i = 0; i < *pThreadCount; ++i) {
		pCtx->args[i] = (PixthThreadArgs){.pCtx = pCtx, .id = i};
	}
	pCtx->threadCount = pixthThreadCountGet();
	if (pCtx->threadCount > *pThreadCount) {
		pCtx->threadCount = *pThreadCount;
	}
	pixthThreadPoolInitIntern(pAlloc, pCtx);
	*pThreadCount = pCtx->threadCount;
	return err;
}
