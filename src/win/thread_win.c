/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <windows.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <pixenals_thread_utils.h>

typedef int32_t I32;

#define JOB_STACK_SIZE 128

typedef struct PixJob {
	PixErr (*pJob) (void *);
	void *pArgs;
	HANDLE pMutex;
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

typedef struct ThreadPool {
	HANDLE threads[PIX_THREAD_MAX_THREADS];
	DWORD threadIds[PIX_THREAD_MAX_THREADS];
	ThreadArgs threadArgs[PIX_THREAD_MAX_THREADS];
	PixJobStack jobs;
	PixalcFPtrs alloc;
	HANDLE jobMutex;
	I32 threadAmount;
	Directive directive;
} ThreadPool;

void pixthMutexGet(void *pThreadPool, void **pMutex) {
	*pMutex = CreateMutex(NULL, 0, NULL);
}

void pixthMutexLock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	WaitForSingleObject(mutex, INFINITE);
}

void pixthMutexUnlock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	ReleaseMutex(mutex);
}

void pixthMutexDestroy(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	CloseHandle(mutex);
}

/*
void stucBarrierGet(void *pThreadPool, void **ppBarrier, I32 jobCount) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	I32 size = sizeof(SYNCHRONIZATION_BARRIER);
	*ppBarrier = pState->alloc.fpCalloc(1, size);
	InitializeSynchronizationBarrier(*ppBarrier, jobCount, -1);
}

bool stucBarrierWait(void *pThreadPool, void *pBarrier) {
	return EnterSynchronizationBarrier(pBarrier, 0);
}

void stucBarrierDestroy(void *pThreadPool, void *pBarrier) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	DeleteSynchronizationBarrier(pBarrier);
	pState->alloc.fpFree(pBarrier);
}
*/

void pixthJobStackGetJob(void *pThreadPool, void **ppJob, int32_t threadId) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
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
	ReleaseMutex(pState->jobMutex);
	return;
}

static
Directive checkRunDirective(const ThreadPool *pState) {
	WaitForSingleObject(pState->jobMutex, INFINITE);
	Directive directive = pState->directive;
	ReleaseMutex(pState->jobMutex);
	return directive;
}

bool pixthGetAndDoJob(void *pThreadPool, I32 threadId) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob *pJob = NULL;
	pixthJobStackGetJob(pState, &pJob, threadId);
	if (!pJob) {
		return false;
	}
	PixErr err = pJob->pJob(pJob->pArgs);
	WaitForSingleObject(pJob->pMutex, INFINITE);
	pJob->err = err;
	//printf("did job on thread %d\n", threadId);
	ReleaseMutex(pJob->pMutex);
	return true;
}

static
unsigned long threadLoop(void *pArgsVoid) {
	ThreadArgs *pArgs = pArgsVoid;
	Directive directive = THREAD_SLEEP;
	while(true) {
		switch (directive) {
			case THREAD_SLEEP:
				directive = checkRunDirective(pArgs->pState);
				if (directive != THREAD_WAKE) {
					if (directive == THREAD_SLEEP) {
						Sleep(25);
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
		WaitForSingleObject(pState->jobMutex, INFINITE);
		I32 nextTop = pState->jobs.count + jobAmount - jobsPushed;
		if (nextTop > JOB_STACK_SIZE) {
			batchTop -= nextTop - JOB_STACK_SIZE;
		}
		for (I32 i = jobsPushed; i < batchTop; ++i) {
			PixJob *pJobEntry = pState->alloc.fpCalloc(1, sizeof(PixJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			pixthMutexGet(pThreadPool, &pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
		pState->directive = THREAD_WAKE;
		ReleaseMutex(pState->jobMutex);
		PIX_ERR_ASSERT("", jobsPushed >= 0 && jobsPushed <= jobAmount);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			Sleep(25);
		}
	} while(true);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

void pixthThreadPoolInit(
	void **pThreadPool,
	I32 *pThreadCount,
	const PixalcFPtrs *pAlloc
) {
	ThreadPool *pState = pAlloc->fpCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAlloc;
	pState->jobMutex = CreateMutex(NULL, 0, NULL);
	pState->directive = THREAD_SLEEP;
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	pState->threadAmount = systemInfo.dwNumberOfProcessors;
	if (pState->threadAmount > PIX_THREAD_MAX_THREADS) {
		pState->threadAmount = PIX_THREAD_MAX_THREADS;
	}
	*pThreadCount = pState->threadAmount;
	if (pState->threadAmount <= 1) {
		return;
	}
	for (I32 i = 0; i < pState->threadAmount; ++i) {
		pState->threadArgs[i] = (ThreadArgs){.pState = pState, .id = i};
		pState->threads[i] = CreateThread(
			NULL,
			0,
			&threadLoop,
			pState->threadArgs + i,
			0,
			pState->threadIds + i
		);
	}
}

void pixthThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		WaitForSingleObject(pState->jobMutex, INFINITE);
		pState->directive = THREAD_STOP;
		ReleaseMutex(pState->jobMutex);
		WaitForMultipleObjects(pState->threadAmount, pState->threads, 1, INFINITE);
	}
	CloseHandle(pState->jobMutex);
	pState->alloc.fpFree(pState);
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
	bool *pChecked = pState->alloc.fpCalloc(jobCount, sizeof(bool));
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
			WaitForSingleObject(ppJobs[i]->pMutex, INFINITE);
			if (ppJobs[i]->err != PIX_ERR_NOT_SET) {
				pChecked[i] = true;
				finished++;
			}
			ReleaseMutex(ppJobs[i]->pMutex);
		}
		PIX_ERR_ASSERT("", finished <= jobCount && finished >= 0);
		if (finished == jobCount) {
			if (!wait) {
				*pDone = true;
			}
			break;
		}
		else if (!gotJob) {
			Sleep(25);
		}
	} while(wait);
	//printf("finished waiting for jobs\n");
	PIX_ERR_CATCH(1, err, ;);
	pState->alloc.fpFree(pChecked);
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
		pState->alloc.fpFree(pJob);
		*ppJobHandle = NULL;
	}
	PIX_ERR_CATCH(0, err, ;);
	return err;
}
