/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

#include <pixenals_thread_utils.h>

typedef int32_t I32;

struct PixthPlatform {
	PixalcFPtrs alloc;
	HANDLE threads[PIX_THREAD_MAX_THREADS];
	DWORD threadIds[PIX_THREAD_MAX_THREADS];
	I32 (*fpLoop)(void *);
};

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

void pixthSleep(void *pThreadPool, I32 nanosec) {
	Sleep(nanosec);
}

static
unsigned long threadLoop(void *pArgs) {
	return (unsigned long)(*(*(PixthPlatform **)pArgs))->fpLoop(pArgs);
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
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	*pThreadCount = systemInfo.dwNumberOfProcessors;
	if (*pThreadCount > PIX_THREAD_MAX_THREADS) {
		*pThreadCount = PIX_THREAD_MAX_THREADS;
	}
	PIX_ERR_RETURN_IFNOT_COND(err, *pThreadCount > 1, "");
	for (I32 i = 0; i < *pThreadCount; ++i) {
		(*ppPlatform)->threads[i] = CreateThread(
			NULL,
			0,
			&threadLoop,
			pixthArgGet(ppPlatform, i),
			0,
			(*ppPlatform)->threadIds + i
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
		WaitForMultipleObjects(threadCount, pPlatform->threads, 1, INFINITE);
	}
	pPlatform->alloc.fpFree(pPlatform);
}
