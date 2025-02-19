#include <windows.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <ThreadPool.h>
#include <Context.h>
#include <Error.h>

#define JOB_STACK_SIZE 128

typedef struct {
	StucResult (*pJob) (void *);
	void *pArgs;
	HANDLE pMutex;
	StucResult err;
} StucJob;

typedef struct {
	StucJob *stack[JOB_STACK_SIZE];
	I32 count;
} StucJobStack;

typedef struct {
	HANDLE threads[MAX_THREADS];
	DWORD threadIds[MAX_THREADS];
	StucJobStack jobs;
	StucAlloc alloc;
	HANDLE jobMutex;
	I32 threadAmount;
	I32 run;
} ThreadPool;

void stucMutexGet(void *pThreadPool, void **pMutex) {
	*pMutex = CreateMutex(NULL, 0, NULL);
}

void stucMutexLock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	WaitForSingleObject(mutex, INFINITE);
}

void stucMutexUnlock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	ReleaseMutex(mutex);
}

void stucMutexDestroy(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	CloseHandle(mutex);
}

void stucBarrierGet(void *pThreadPool, void **ppBarrier, I32 jobCount) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	I32 size = sizeof(SYNCHRONIZATION_BARRIER);
	*ppBarrier = pState->alloc.pCalloc(1, size);
	InitializeSynchronizationBarrier(*ppBarrier, jobCount, -1);
}

bool stucBarrierWait(void *pThreadPool, void *pBarrier) {
	return EnterSynchronizationBarrier(pBarrier, 0);
}

void stucBarrierDestroy(void *pThreadPool, void *pBarrier) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	DeleteSynchronizationBarrier(pBarrier);
	pState->alloc.pFree(pBarrier);
}

void stucJobStackGetJob(void *pThreadPool, void **ppJob) {
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

bool stucGetAndDoJob(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = NULL;
	stucJobStackGetJob(pState, &pJob);
	if (!pJob) {
		return false;
	}
	StucResult err = pJob->pJob(pJob->pArgs);
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
		bool gotJob = stucGetAndDoJob(pArgs);
		if (!gotJob) {
			Sleep(25);
		}
	}
	return 0;
}

Result stucJobStackPushJobs(
	void *pThreadPool,
	I32 jobAmount,
	void **ppJobHandles,
	StucResult(*pJob)(void *),
	void **pJobArgs
) {
	Result err = STUC_SUCCESS;
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
			StucJob *pJobEntry = pState->alloc.pCalloc(1, sizeof(StucJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			stucMutexGet(pThreadPool, &pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
		ReleaseMutex(pState->jobMutex);
		STUC_ASSERT("", jobsPushed >= 0 && jobsPushed <= jobAmount);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			Sleep(25);
		}
	} while(true);
	STUC_CATCH(0, err, ;);
	return err;
}

void stucThreadPoolInit(
	void **pThreadPool,
	I32 *pThreadCount,
	const StucAlloc *pAlloc
) {
	ThreadPool *pState = pAlloc->pCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAlloc;
	pState->jobMutex = CreateMutex(NULL, 0, NULL);
	pState->run = 1;
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	pState->threadAmount = systemInfo.dwNumberOfProcessors;
	if (pState->threadAmount > MAX_THREADS) {
		pState->threadAmount = MAX_THREADS;
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

void stucThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		WaitForSingleObject(pState->jobMutex, INFINITE);
		pState->run = 0;
		ReleaseMutex(pState->jobMutex);
		WaitForMultipleObjects(pState->threadAmount, pState->threads, 1, INFINITE);
	}
	CloseHandle(pState->jobMutex);
	pState->alloc.pFree(pState);
}

StucResult stucThreadPoolSetCustom(
	StucContext pCtx,
	const StucThreadPool *pThreadPool
) {
	if (!pThreadPool->pInit || !pThreadPool->pDestroy || !pThreadPool->pMutexGet ||
	    !pThreadPool->pMutexLock || !pThreadPool->pMutexUnlock || !pThreadPool->pMutexDestroy ||
	    !pThreadPool->pJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return STUC_ERROR;
	}
	pCtx->threadPool.pDestroy(pCtx);
	pCtx->threadPool = *pThreadPool;
	return STUC_SUCCESS;
}

void stucThreadPoolSetDefault(StucContext pCtx) {
	pCtx->threadPool.pInit = stucThreadPoolInit;
	pCtx->threadPool.pWaitForJobs = stucWaitForJobsIntern;
	pCtx->threadPool.pGetJobErr = stucGetJobErr;
	pCtx->threadPool.pJobHandleDestroy = stucJobHandleDestroy;
	pCtx->threadPool.pDestroy = stucThreadPoolDestroy;
	pCtx->threadPool.pMutexGet = stucMutexGet;
	pCtx->threadPool.pMutexLock = stucMutexLock;
	pCtx->threadPool.pMutexUnlock = stucMutexUnlock;
	pCtx->threadPool.pMutexDestroy = stucMutexDestroy;
	pCtx->threadPool.pBarrierGet = stucBarrierGet;
	pCtx->threadPool.pBarrierWait = stucBarrierWait;
	pCtx->threadPool.pBarrierDestroy = stucBarrierDestroy;
	pCtx->threadPool.pJobStackGetJob = stucJobStackGetJob;
	pCtx->threadPool.pJobStackPushJobs = stucJobStackPushJobs;
	pCtx->threadPool.pGetAndDoJob = stucGetAndDoJob;
}

StucResult stucWaitForJobsIntern(
	void *pThreadPool,
	I32 jobCount,
	void **ppJobsVoid,
	bool wait,
	bool *pDone
) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", jobCount > 0);
	STUC_ASSERT("if wait is false, pDone must not be null", pDone || wait);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob **ppJobs = (StucJob **)ppJobsVoid;
	I32 finished = 0;
	bool *pChecked = pState->alloc.pCalloc(jobCount, sizeof(bool));
	if (!wait) {
		*pDone = false;
	}
	do {
		bool gotJob = false;
		if (wait) {
			gotJob = stucGetAndDoJob(pThreadPool);
		}
		for (I32 i = 0; i < jobCount; ++i) {
			if (pChecked[i]) {
				continue;
			}
			WaitForSingleObject(ppJobs[i]->pMutex, INFINITE);
			if (ppJobs[i]->err != STUC_NOT_SET) {
				pChecked[i] = true;
				finished++;
			}
			ReleaseMutex(ppJobs[i]->pMutex);
		}
		STUC_ASSERT("", finished <= jobCount && finished >= 0);
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
	STUC_CATCH(1, err, ;);
	pState->alloc.pFree(pChecked);
	STUC_CATCH(0, err, ;);
	return err;
}

StucResult stucGetJobErr(void *pThreadPool, void *pJobHandle, StucResult *pJobErr) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", pThreadPool && pJobHandle && pJobErr);
	StucJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	STUC_CATCH(0, err, ;);
	return err;
}

StucResult stucJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", pThreadPool && ppJobHandle);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = *ppJobHandle;
	if (*ppJobHandle) {
		stucMutexDestroy(pThreadPool, pJob->pMutex);
		pState->alloc.pFree(pJob);
		*ppJobHandle = NULL;
	}
	STUC_CATCH(0, err, ;);
	return err;
}