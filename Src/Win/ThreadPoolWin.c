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
	I32 threadAmount;
	HANDLE jobMutex;
	I32 run;
	StucJobStack jobs;
	StucAlloc alloc;
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
bool checkRunFlag(ThreadPool *pState) {
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

I32 stucJobStackPushJobs(void *pThreadPool, I32 jobAmount, void **ppJobHandles,
                             StucResult (*pJob)(void *), void **pJobArgs) {
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
		STUC_THROW_IF(err, jobsPushed >= 0 && jobsPushed <= jobAmount, "", 0);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			Sleep(25);
		}
	} while(true);
	STUC_CATCH(0, err, ;);
	return 0;
}

void stucThreadPoolInit(void **pThreadPool, I32 *pThreadCount,
                        const StucAlloc *pAlloc) {
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
		pState->threads[i] = CreateThread(NULL, 0, &threadLoop, pState, 0,
		                                  pState->threadIds + i);
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

StucResult stucThreadPoolSetCustom(StucContext pContext, const StucThreadPool *pThreadPool) {
	if (!pThreadPool->pInit || !pThreadPool->pDestroy || !pThreadPool->pMutexGet ||
	    !pThreadPool->pMutexLock || !pThreadPool->pMutexUnlock || !pThreadPool->pMutexDestroy ||
	    !pThreadPool->pJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return STUC_ERROR;
	}
	pContext->threadPool.pDestroy(pContext);
	pContext->threadPool = *pThreadPool;
	return STUC_SUCCESS;
}

void stucThreadPoolSetDefault(StucContext pContext) {
	pContext->threadPool.pInit = stucThreadPoolInit;
	pContext->threadPool.pWaitForJobs = stucWaitForJobsIntern;
	pContext->threadPool.pGetJobErr = stucGetJobErr;
	pContext->threadPool.pJobHandleDestroy = stucJobHandleDestroy;
	pContext->threadPool.pDestroy = stucThreadPoolDestroy;
	pContext->threadPool.pMutexGet = stucMutexGet;
	pContext->threadPool.pMutexLock = stucMutexLock;
	pContext->threadPool.pMutexUnlock = stucMutexUnlock;
	pContext->threadPool.pMutexDestroy = stucMutexDestroy;
	pContext->threadPool.pBarrierGet = stucBarrierGet;
	pContext->threadPool.pBarrierWait = stucBarrierWait;
	pContext->threadPool.pBarrierDestroy = stucBarrierDestroy;
	pContext->threadPool.pJobStackGetJob = stucJobStackGetJob;
	pContext->threadPool.pJobStackPushJobs = stucJobStackPushJobs;
	pContext->threadPool.pGetAndDoJob = stucGetAndDoJob;
}

StucResult stucWaitForJobsIntern(void *pThreadPool, I32 jobCount, void **ppJobsVoid,
                                 bool wait, bool *pDone) {
	StucResult err = STUC_SUCCESS;
	STUC_THROW_IF(err, jobCount > 0, "", 0);
	STUC_THROW_IF(err, pDone || wait, "if wait is false, pDone must not be null", 0);
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
			bool gotJob = stucGetAndDoJob(pThreadPool);
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
		STUC_THROW_IF(err, finished <= jobCount && finished >= 0, "", 1);
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
	STUC_THROW_IF(err, pThreadPool && pJobHandle && pJobErr, "", 0);
	StucJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	STUC_CATCH(0, err, ;);
	return err;
}

StucResult stucJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	StucResult err = STUC_SUCCESS;
	STUC_THROW_IF(err, pThreadPool && ppJobHandle, "", 0);
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