/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <windows.h>
#include <intrin.h>

#include <pixenals_thread_utils.h>

typedef int32_t I32;
typedef int64_t I64;

struct PixthPlatform {
	PixalcFPtrs alloc;
	HANDLE threads[PIXTH_MAX_THREADS];
	DWORD threadIds[PIXTH_MAX_THREADS];
	I32 (*fpLoop)(void *);
} PixthPlatformIntern;

void pixthMutexGet(PixthPoolCtx *pCtx, void **pMutex) {
	*pMutex = CreateMutex(NULL, 0, NULL);
}

void pixthMutexLock(PixthPoolCtx *pCtx, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	WaitForSingleObject(mutex, INFINITE);
}

void pixthMutexUnlock(PixthPoolCtx *pCtx, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	ReleaseMutex(mutex);
}

void pixthMutexDestroy(PixthPoolCtx *pCtx, void *pMutex) {
	HANDLE mutex = (HANDLE)pMutex;
	CloseHandle(mutex);
}

I32 pixthAtomicSwapI32(PixthPoolCtx *pCtx, volatile I32 *ptr, I32 val) {
	return _InterlockedExchange(ptr, val);
}

I64 pixthAtomicSwapI64(PixthPoolCtx *pCtx, volatile I64 *ptr, I64 val) {
	return _InterlockedExchange64(ptr, val);
}

I32 pixthAtomicCmpAndSwapI32(PixthPoolCtx *pCtx, volatile I32 *ptr, I32 cmp, I32 val) {
	return _InterlockedCompareExchange(ptr, val, cmp);
}

I64 pixthAtomicCmpAndSwapI64(PixthPoolCtx *pCtx, volatile I64 *ptr, I64 cmp, I64 val) {
	return _InterlockedCompareExchange64(ptr, val, cmp);
}

void pixthSleep(PixthPoolCtx *pCtx, I32 nanosec) {
	Sleep(nanosec);
}

static
unsigned long threadLoop(void *pArgs) {
	return (unsigned long)(*(*(PixthPlatform **)pArgs))->fpLoop(pArgs);
}

I32 pixthThreadCountGet() {
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	return systemInfo.dwNumberOfProcessors;
}

PixErr pixthThreadPoolInitPlatform(
	const PixalcFPtrs *pAlloc,
	PixthPlatform *ppPlatform,
	I32 threadCount,
	I32 (*fpLoop)(void *)
) {
	PixErr err = PIX_ERR_SUCCESS;
	*ppPlatform = pAlloc->fpCalloc(1, sizeof(struct PixthPlatform));
	(*ppPlatform)->alloc = *pAlloc;
	(*ppPlatform)->fpLoop = fpLoop;
	for (I32 i = 0; i < threadCount; ++i) {
		(*ppPlatform)->threads[i] = CreateThread(
			NULL,
			0,
			&threadLoop,
			pixthArgGet((PixthPoolCtx *)(ppPlatform), i),
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
