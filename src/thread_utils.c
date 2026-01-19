/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <pixenals_thread_utils.h>

typedef int32_t I32;

#define JOB_STACK_SIZE 128

typedef struct PixJob {
	PixErr (*pJob) (void *);
	void *pArgs;
	void *pMutex;
	PixErr err;
} PixJob;

typedef struct PixJobStack {
	PixJob *stack[JOB_STACK_SIZE];
	I32 count;
} PixJobStack;

struct ThreadPool;

typedef struct ThreadArgs {
	struct ThreadPool *pState;
	int32_t id;
} ThreadArgs;

typedef enum Directive {
	THREAD_NONE,
	THREAD_SLEEP,
	THREAD_WAKE,
	THREAD_STOP
} Directive;

union Core {
	PixthPlatform pPlatform;
	PixalcFPtrs *pAlloc;
};

typedef struct ThreadPool {
	union Core core;
	PixJobStack jobs;
	//TODO mutex should be opaque ptr not void *
	void *pJobMutex;
	I32 threadAmount;
	Directive directive;
	ThreadArgs args[PIX_THREAD_MAX_THREADS];
} ThreadPool;


void pixthJobStackGetJob(void *pThreadPool, void **ppJob, int32_t threadId) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pixthMutexLock(pState, pState->pJobMutex);
	if (pState->jobs.count > 0) {
		//printf("found job on thread %d\n", threadId);
		pState->jobs.count--;
		*ppJob = pState->jobs.stack[pState->jobs.count];
		pState->jobs.stack[pState->jobs.count] = NULL;
	}
	else {
		//printf("no job found on thread %d\n", threadId);
		*ppJob = NULL;
		pState->directive = THREAD_SLEEP;
	}
	pixthMutexUnlock(pState, pState->pJobMutex);
	return;
}

static
Directive checkRunDirective(ThreadPool *pState) {
	pixthMutexLock(pState, pState->pJobMutex);
	Directive directive = pState->directive;
	pixthMutexUnlock(pState, pState->pJobMutex);
	return directive;
}

bool pixthGetAndDoJob(void *pThreadPool, I32 threadId) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob *pJob = NULL;
	pixthJobStackGetJob(pState, (void **)&pJob, threadId);
	if (!pJob) {
		return false;
	}
	PixErr err = pJob->pJob(pJob->pArgs);
	pixthMutexLock(pState, pJob->pMutex);
	pJob->err = err;
	//printf("did job on thread %d\n", threadId);
	pixthMutexUnlock(pState, pJob->pMutex);
	return true;
}

static
I32 threadLoop(void *pArgsVoid) {
	ThreadArgs *pArgs = pArgsVoid;
	Directive directive = THREAD_SLEEP;
	while(true) {
		switch (directive) {
			case THREAD_SLEEP:
				directive = checkRunDirective(pArgs->pState);
				if (directive != THREAD_WAKE) {
					if (directive == THREAD_SLEEP) {
						pixthSleep(pArgs->pState, 25);
					}
					break;
				}
				//if THREAD_WAKE then fall through to next case
			case THREAD_WAKE:
				if (!pixthGetAndDoJob(pArgs->pState, pArgs->id)) {
					directive = THREAD_SLEEP;//no jobs left in stack
				}
				break;
			case THREAD_STOP:
			default:
				return 0; //TODO should default return 1?
		}
	}
}

PixErr pixthJobStackPushJobs(
	void *pThreadPool,
	I32 jobAmount,
	void **ppJobHandles,
	PixErr(*pJob)(void *),
	void **pJobArgs
) {
	PixErr err = PIX_ERR_SUCCESS;
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	I32 jobsPushed = 0;
	do {
		I32 batchTop = jobAmount;
		pixthMutexLock(pState, pState->pJobMutex);
		I32 nextTop = pState->jobs.count + jobAmount - jobsPushed;
		if (nextTop > JOB_STACK_SIZE) {
			batchTop -= nextTop - JOB_STACK_SIZE;
		}
		for (I32 i = jobsPushed; i < batchTop; ++i) {
			PixJob *pJobEntry = pState->core.pAlloc->fpCalloc(1, sizeof(PixJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			pixthMutexGet(pThreadPool, &pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
		pState->directive = THREAD_WAKE;
		pixthMutexUnlock(pState, pState->pJobMutex);
		PIX_ERR_ASSERT("", jobsPushed >= 0 && jobsPushed <= jobAmount);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			pixthSleep(pState, 25);
		}
	} while(true);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

void *pixthArgGet(void *pThreadPool, I32 idx) {
	ThreadPool *pState = pThreadPool;
	PIX_ERR_ASSERT("", idx < pState->threadAmount);
	return pState->args + idx;
}

PixErr pixthThreadPoolInit(
	void **ppThreadPool,
	I32 *pThreadCount,
	const PixalcFPtrs *pAlloc
) {
	PixErr err = PIX_ERR_SUCCESS;
	ThreadPool *pState = pAlloc->fpCalloc(1, sizeof(ThreadPool));
	*ppThreadPool = pState;
	pixthMutexGet((void *)&pAlloc, &pState->pJobMutex);
	pState->directive = THREAD_SLEEP;
	for (I32 i = 0; i < PIX_THREAD_MAX_THREADS; ++i) {
		pState->args[i] = (ThreadArgs){.pState = pState, .id = i};
	}
	err = pixthThreadPoolInitPlatform(
		pAlloc,
		&pState->core.pPlatform,
		&pState->threadAmount,
		threadLoop
	);
	PIX_ERR_RETURN_IFNOT(err, "");
	*pThreadCount = pState->threadAmount;
	return err;
}

PixErr pixthWaitForJobsIntern(
	void *pThreadPool,
	I32 jobCount,
	void **ppJobsVoid,
	bool wait,
	bool *pDone
) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", jobCount > 0);
	PIX_ERR_ASSERT("if wait is false, pDone must not be null", pDone || wait);
	ThreadPool *pState = pThreadPool;
	PixJob **ppJobs = (PixJob **)ppJobsVoid;
	I32 finished = 0;
	bool *pChecked = pState->core.pAlloc->fpCalloc(jobCount, sizeof(bool));
	if (!wait) {
		*pDone = false;
	}
	do {
		bool gotJob = false;
		if (wait) {
			gotJob = pixthGetAndDoJob(pThreadPool, -1);
		}
		for (I32 i = 0; i < jobCount; ++i) {
			if (pChecked[i]) {
				continue;
			}
			pixthMutexLock(pState, ppJobs[i]->pMutex);
			if (ppJobs[i]->err != PIX_ERR_NOT_SET) {
				pChecked[i] = true;
				finished++;
			}
			pixthMutexUnlock(pState, ppJobs[i]->pMutex);
		}
		PIX_ERR_ASSERT("", finished <= jobCount && finished >= 0);
		if (finished == jobCount) {
			if (!wait) {
				*pDone = true;
			}
			break;
		}
		else if (!gotJob) {
			pixthSleep(pState, 25);
		}
	} while(wait);
	//printf("finished waiting for jobs\n");
	PIX_ERR_CATCH(1, err, ;);
	pState->core.pAlloc->fpFree(pChecked);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthGetJobErr(void *pThreadPool, void *pJobHandle, PixErr *pJobErr) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && pJobHandle && pJobErr);
	PixJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && ppJobHandle);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob *pJob = *ppJobHandle;
	if (*ppJobHandle) {
		pixthMutexDestroy(pThreadPool, pJob->pMutex);
		pState->core.pAlloc->fpFree(pJob);
		*ppJobHandle = NULL;
	}
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

void pixthThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 0) {
		PIX_ERR_ASSERT("", pState->threadAmount != 1);
		pixthMutexLock(pState, pState->pJobMutex);
		pState->directive = THREAD_STOP;
		pixthMutexUnlock(pState, pState->pJobMutex);
	}
	PixalcFPtrs alloc = *pState->core.pAlloc;
	PixalcFPtrs *pAlloc = &alloc;
	pixthPlatformDestroy(pState->core.pPlatform, pState->threadAmount);
	pixthMutexDestroy((void *)&pAlloc, pState->pJobMutex);
	alloc.fpFree(pState);
}
