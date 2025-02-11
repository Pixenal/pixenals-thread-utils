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
	int32_t count;
} StucJobStack;

typedef struct {
	HANDLE threads[MAX_THREADS];
	DWORD threadIds[MAX_THREADS];
	int32_t threadAmount;
	HANDLE jobMutex;
	int32_t run;
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

void stucBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	int32_t size = sizeof(SYNCHRONIZATION_BARRIER);
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

static
void stucJobStackGetJob(void *pThreadPool, void **pJob) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
	if (pState->jobs.count > 0) {
		pState->jobs.count--;
		*pJob = pState->jobs.stack[pState->jobs.count];
		pState->jobs.stack[pState->jobs.count] = NULL;
	}
	else {
		*pJob = NULL;
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

int32_t stucJobStackPushJobs(void *pThreadPool, int32_t jobAmount, void **ppJobHandles,
                             StucResult (*pJob)(void *), void **pJobArgs) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
	int32_t nextTop = pState->jobs.count + jobAmount;
	if (nextTop > MAX_THREADS) {
		ReleaseMutex(pState->jobMutex);
		return 1;
	}
	for (int32_t i = 0; i < jobAmount; ++i) {
		StucJob *pJobEntry = pState->alloc.pCalloc(1, sizeof(StucJob));
		pJobEntry->pJob = pJob;
		pJobEntry->pArgs = pJobArgs[i];
		stucMutexGet(pThreadPool, &pJobEntry->pMutex);
		pState->jobs.stack[pState->jobs.count] = pJobEntry;
		pState->jobs.count++;
		ppJobHandles[i] = pJobEntry;
	}
	ReleaseMutex(pState->jobMutex);
	return 0;
}

void stucThreadPoolInit(void **pThreadPool, int32_t *pThreadCount,
                        StucAlloc *pAllocator) {
	ThreadPool *pState = pAllocator->pCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAllocator;
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
	for (int32_t i = 0; i < pState->threadAmount; ++i) {
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

StucResult stucThreadPoolSetCustom(StucContext context, StucThreadPool *pThreadPool) {
	if (!pThreadPool->pInit || !pThreadPool->pDestroy || !pThreadPool->pMutexGet ||
	    !pThreadPool->pMutexLock || !pThreadPool->pMutexUnlock || !pThreadPool->pMutexDestroy ||
		!pThreadPool->pJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return STUC_ERROR;
	}
	context->threadPool.pDestroy(context);
	context->threadPool = *pThreadPool;
	return STUC_SUCCESS;
}

void stucThreadPoolSetDefault(StucContext context) {
	context->threadPool.pInit = stucThreadPoolInit;
	context->threadPool.pDestroy = stucThreadPoolDestroy;
	context->threadPool.pMutexGet = stucMutexGet;
	context->threadPool.pMutexLock = stucMutexLock;
	context->threadPool.pMutexUnlock = stucMutexUnlock;
	context->threadPool.pMutexDestroy = stucMutexDestroy;
	context->threadPool.pBarrierGet = stucBarrierGet;
	context->threadPool.pBarrierWait = stucBarrierWait;
	context->threadPool.pBarrierDestroy = stucBarrierDestroy;
	context->threadPool.pJobStackGetJob = stucJobStackGetJob;
	context->threadPool.pJobStackPushJobs = stucJobStackPushJobs;
	context->threadPool.pGetAndDoJob = stucGetAndDoJob;
}

//TODO replace custom barrier with system barrier?
StucResult stucWaitForJobs(void *pThreadPool, int32_t jobCount, void **ppJobsVoid) {
	StucResult err = STUC_SUCCESS;
	STUC_THROW_IF(err, jobCount > 0, "", 0);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob **ppJobs = ppJobsVoid;
	int32_t finished = 0;
	bool *pChecked = pState->alloc.pCalloc(jobCount, sizeof(bool));
	do  {
		bool gotJob = stucGetAndDoJob(pThreadPool);
		for (int32_t i = 0; i < jobCount; ++i) {
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
		STUC_THROW_IF(err, finished <= jobCount && finished >= 0, "", 0);
		if (finished == jobCount) {
			break;
		}
		else if (!gotJob) {
			Sleep(25);
		}
	} while(true);
	STUC_CATCH(0, err, ;);
	pState->alloc.pFree(pChecked);
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