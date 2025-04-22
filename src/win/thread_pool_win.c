/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <windows.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <thread_pool.h>

typedef int32_t I32;

#define JOB_STACK_SIZE 128

typedef struct StucJob {
	PixErr (*pJob) (void *);
	void *pArgs;
	HANDLE pMutex;
	PixErr err;
} StucJob;

typedef struct StucJobStack {
	StucJob *stack[JOB_STACK_SIZE];
	I32 count;
} StucJobStack;

typedef struct ThreadPool {
	HANDLE threads[PIX_THREAD_MAX_THREADS];
	DWORD threadIds[PIX_THREAD_MAX_THREADS];
	StucJobStack jobs;
	PixalcFPtrs alloc;
	HANDLE jobMutex;
	I32 threadAmount;
	I32 run;
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

void pixthJobStackGetJob(void *pThreadPool, void **ppJob) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
	if (pState->jobs.count > 0) {
		pState->jobs.count--;
		*ppJob = pState->jobs.stack[pState->jobs.count];
		pState->jobs.stack[pState->jobs.count] = NULL;
	}
	else {
		*ppJob = NULL;
	}
	ReleaseMutex(pState->jobMutex);
	return;
}

static
bool checkRunFlag(const ThreadPool *pState) {
	WaitForSingleObject(pState->jobMutex, INFINITE);
	bool run = pState->run;
	ReleaseMutex(pState->jobMutex);
	return run;
}

bool pixthGetAndDoJob(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = NULL;
	pixthJobStackGetJob(pState, &pJob);
	if (!pJob) {
		return false;
	}
	PixErr err = pJob->pJob(pJob->pArgs);
	WaitForSingleObject(pJob->pMutex, INFINITE);
	pJob->err = err;
	ReleaseMutex(pJob->pMutex);
	return true;
}

static
unsigned long threadLoop(void *pArgs) {
	ThreadPool *pState = (ThreadPool *)pArgs;
	while(1) {
		if (!checkRunFlag(pState)) {
			break;
		}
		bool gotJob = pixthGetAndDoJob(pArgs);
		if (!gotJob) {
			Sleep(25);
		}
	}
	return 0;
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
			StucJob *pJobEntry = pState->alloc.fpCalloc(1, sizeof(StucJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			pixthMutexGet(pThreadPool, &pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
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
	pState->run = 1;
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
		pState->threads[i] =
			CreateThread(NULL, 0, &threadLoop, pState, 0, pState->threadIds + i);
	}
}

void pixthThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		WaitForSingleObject(pState->jobMutex, INFINITE);
		pState->run = 0;
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
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob **ppJobs = (StucJob **)ppJobsVoid;
	I32 finished = 0;
	bool *pChecked = pState->alloc.fpCalloc(jobCount, sizeof(bool));
	if (!wait) {
		*pDone = false;
	}
	do {
		bool gotJob = false;
		if (wait) {
			gotJob = pixthGetAndDoJob(pThreadPool);
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
	PIX_ERR_CATCH(1, err, ;);
	pState->alloc.fpFree(pChecked);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthGetJobErr(void *pThreadPool, void *pJobHandle, PixErr *pJobErr) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && pJobHandle && pJobErr);
	StucJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && ppJobHandle);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = *ppJobHandle;
	if (*ppJobHandle) {
		pixthMutexDestroy(pThreadPool, pJob->pMutex);
		pState->alloc.fpFree(pJob);
		*ppJobHandle = NULL;
	}
	PIX_ERR_CATCH(0, err, ;);
	return err;
}