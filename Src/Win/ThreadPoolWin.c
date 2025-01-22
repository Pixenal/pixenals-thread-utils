#include <windows.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <ThreadPool.h>
#include <Context.h>

typedef struct {
	HANDLE threads[MAX_THREADS];
	DWORD threadIds[MAX_THREADS];
	int32_t threadAmount;
	HANDLE jobMutex;
	int32_t run;
	void *jobStack[MAX_THREADS];
	void *argStack[MAX_THREADS];
	int32_t jobStackSize;
	RuvmAlloc allocator;
} ThreadPool;

void uvsMutexGet(void *pThreadPool, void **pMutex) {
	*pMutex = CreateMutex(NULL, 0, NULL);
}

void uvsMutexLock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	WaitForSingleObject(mutex, INFINITE);
}

void uvsMutexUnlock(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	ReleaseMutex(mutex);
}

void uvsMutexDestroy(void *pThreadPool, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	CloseHandle(mutex);
}

void uvsBarrierGet(void *pThreadPool, void **ppBarrier, int32_t jobCount) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	int32_t size = sizeof(SYNCHRONIZATION_BARRIER);
	*ppBarrier = pState->allocator.pCalloc(1, size);
	InitializeSynchronizationBarrier(*ppBarrier, jobCount, -1);
}

bool uvsBarrierWait(void *pThreadPool, void *pBarrier) {
	return EnterSynchronizationBarrier(pBarrier, 0);
}

void uvsBarrierDestroy(void *pThreadPool, void *pBarrier) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	DeleteSynchronizationBarrier(pBarrier);
	pState->allocator.pFree(pBarrier);
}

void uvsJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **pArgs) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
	if (pState->jobStackSize > 0) {
		pState->jobStackSize--;
		*pJob = pState->jobStack[pState->jobStackSize];
		pState->jobStack[pState->jobStackSize] = NULL;
		*pArgs = pState->argStack[pState->jobStackSize];
		pState->argStack[pState->jobStackSize] = NULL;
	}
	else {
		*pJob = *pArgs = NULL;
	}
	ReleaseMutex(pState->jobMutex);
	return;
}

static bool checkRunFlag(ThreadPool *pState) {
	WaitForSingleObject(pState->jobMutex, INFINITE);
	bool run = pState->run;
	ReleaseMutex(pState->jobMutex);
	return run;
}

bool uvsGetAndDoJob(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	void (*pJob)(void *) = NULL;
	void *pJobArgs = NULL;
	uvsJobStackGetJob(pState, &pJob, &pJobArgs);
	if (!pJob) {
		return false;
	}
	pJob(pJobArgs);
	return true;
}

static unsigned long threadCorner(void *pArgs) {
	ThreadPool *pState = (ThreadPool *)pArgs;
	while(1) {
		if (!checkRunFlag(pState)) {
			break;
		}
		bool gotJob = uvsGetAndDoJob(pArgs);
		if (!gotJob) {
			Sleep(25);
		}
	}
	return 0;
}

int32_t uvsJobStackPushJobs(void *pThreadPool, int32_t jobAmount,
                             void (*pJob)(void *), void **pJobArgs) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	WaitForSingleObject(pState->jobMutex, INFINITE);
	int32_t nextTop = pState->jobStackSize + jobAmount;
	if (nextTop > MAX_THREADS) {
		ReleaseMutex(pState->jobMutex);
		return 1;
	}
	for (int32_t i = 0; i < jobAmount; ++i) {
		pState->argStack[pState->jobStackSize] = pJobArgs[i];
		pState->jobStack[pState->jobStackSize] = pJob;
		pState->jobStackSize++;
	}
	ReleaseMutex(pState->jobMutex);
	return 0;
}

void uvsThreadPoolInit(void **pThreadPool, int32_t *pThreadCount,
                        RuvmAlloc *pAllocator) {
	ThreadPool *pState = pAllocator->pCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->allocator = *pAllocator;
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
		pState->threads[i] = CreateThread(NULL, 0, &threadCorner, pState, 0,
				                  pState->threadIds + i);
	}
}

void uvsThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		WaitForSingleObject(pState->jobMutex, INFINITE);
		pState->run = 0;
		ReleaseMutex(pState->jobMutex);
		WaitForMultipleObjects(pState->threadAmount, pState->threads, 1, INFINITE);
	}
	CloseHandle(pState->jobMutex);
	pState->allocator.pFree(pState);
}

RuvmResult uvsThreadPoolSetCustom(RuvmContext context, RuvmThreadPool *pThreadPool) {
	if (!pThreadPool->pInit || !pThreadPool->pDestroy || !pThreadPool->pMutexGet ||
	    !pThreadPool->pMutexLock || !pThreadPool->pMutexUnlock || !pThreadPool->pMutexDestroy ||
		!pThreadPool->pJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return RUVM_ERROR;
	}
	context->threadPool.pDestroy(context);
	context->threadPool = *pThreadPool;
	return RUVM_SUCCESS;
}

void uvsThreadPoolSetDefault(RuvmContext context) {
	context->threadPool.pInit = uvsThreadPoolInit;
	context->threadPool.pDestroy = uvsThreadPoolDestroy;
	context->threadPool.pMutexGet = uvsMutexGet;
	context->threadPool.pMutexLock = uvsMutexLock;
	context->threadPool.pMutexUnlock = uvsMutexUnlock;
	context->threadPool.pMutexDestroy = uvsMutexDestroy;
	context->threadPool.pBarrierGet = uvsBarrierGet;
	context->threadPool.pBarrierWait = uvsBarrierWait;
	context->threadPool.pBarrierDestroy = uvsBarrierDestroy;
	context->threadPool.pJobStackGetJob = uvsJobStackGetJob;
	context->threadPool.pJobStackPushJobs = uvsJobStackPushJobs;
	context->threadPool.pGetAndDoJob = uvsGetAndDoJob;
}
