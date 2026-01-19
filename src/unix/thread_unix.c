/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#ifdef __APPLE_CC__
	#include <sys/sysctl.h>
#else
	#include <sys/sysinfo.h>
#endif

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <pixenals_thread_utils.h>

typedef int32_t I32;

struct PixthPlatform {
	PixalcFPtrs alloc;
	pthread_t threads[PIX_THREAD_MAX_THREADS];
	I32 (*fpLoop)(void *);
};

void pixthMutexGet(void *pThreadPool, void **ppMutex) {
	PixalcFPtrs *pAlloc = *(PixalcFPtrs **)pThreadPool;
	*ppMutex = pAlloc->fpMalloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(*ppMutex, NULL);
}

void pixthMutexLock(void *pThreadPool, void *pMutex) {
	pthread_mutex_lock(pMutex);
}

void pixthMutexUnlock(void *pThreadPool, void *pMutex) {
	pthread_mutex_unlock(pMutex);
}

void pixthMutexDestroy(void *pThreadPool, void *pMutex) {
	PixalcFPtrs *pAlloc = *(PixalcFPtrs **)pThreadPool;
	pthread_mutex_destroy(pMutex);
	pAlloc->fpFree(pMutex);
}

void pixthSleep(void *pThreadPool, I32 nanosec) {
	nanosleep(&(struct timespec){0, nanosec}, NULL);
}

static
void *threadLoop(void *pArgs) {
	(*(*(PixthPlatform **)pArgs))->fpLoop(pArgs);
	return NULL;
}

PixErr pixthThreadPoolInitPlatform(
	const PixalcFPtrs *pAlloc,
	PixthPlatform *ppPlatform,
	I32 *pThreadCount,
	I32 (*fpLoop)(void *)
) {
	PixErr err = PIX_ERR_SUCCESS;
	*ppPlatform = pAlloc->fpCalloc(1, sizeof(struct PixthPlatform));
	(*ppPlatform)->alloc = *pAlloc;
	(*ppPlatform)->fpLoop = fpLoop;
#ifdef __APPLE_CC__
	uint64_t count = 0;
	size_t size = sizeof(uint64_t);
	I32 result = sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0);
	PIX_ERR_RETURN_IFNOT_COND(err, result >= 0, "unable to get core count");
	*pThreadCount = count;
#else
	*pThreadCount = get_nprocs();
#endif
	if (*pThreadCount > PIX_THREAD_MAX_THREADS) {
		*pThreadCount = PIX_THREAD_MAX_THREADS;
	}
	PIX_ERR_RETURN_IFNOT_COND(err, *pThreadCount > 1, "");
	for (I32 i = 0; i < *pThreadCount; ++i) {
		pthread_create(
			(*ppPlatform)->threads + i,
			NULL,
			threadLoop,
			pixthArgGet(ppPlatform, i)
		);
	}
	return err;
}

void pixthPlatformDestroy(
	PixthPlatform pPlatform,
	I32 threadCount
) {
	if (threadCount > 0) {
		PIX_ERR_ASSERT("", threadCount != 1);
		for (I32 i = 0; i < threadCount; ++i) {
			pthread_join(pPlatform->threads[i], NULL);
		}
	}
	pPlatform->alloc.fpFree(pPlatform);
}
