/* =============================================================================
 * src/sys/lzma/Alloc.h —— ISzAlloc 接口（g_Alloc 声明）。
 *
 * 来源：tools/PackEXE/pe/lzma（Igor Pavlov，Public domain），P1 迁入（Plan §3.1）。
 * 与上游的差异：无（逐字节保留上游实现）。
 * ========================================================================== */

/* Alloc.h -- Memory allocation functions
2018-02-19 : Igor Pavlov : Public domain */

#ifndef __COMMON_ALLOC_H
#define __COMMON_ALLOC_H

#include "7zTypes.h"

EXTERN_C_BEGIN

void *MyAlloc(size_t size);
void MyFree(void *address);

#ifdef _WIN32

void SetLargePageSize();

void *MidAlloc(size_t size);
void MidFree(void *address);
void *BigAlloc(size_t size);
void BigFree(void *address);

#else

#define MidAlloc(size) MyAlloc(size)
#define MidFree(address) MyFree(address)
#define BigAlloc(size) MyAlloc(size)
#define BigFree(address) MyFree(address)

#endif

extern const ISzAlloc g_Alloc;
extern const ISzAlloc g_BigAlloc;
extern const ISzAlloc g_MidAlloc;
extern const ISzAlloc g_AlignedAlloc;


typedef struct
{
  ISzAlloc vt;
  ISzAllocPtr baseAlloc;
  unsigned numAlignBits; /* ((1 << numAlignBits) >= sizeof(void *)) */
  size_t offset;         /* (offset == (k * sizeof(void *)) && offset < (1 << numAlignBits) */
} CAlignOffsetAlloc;

void AlignOffsetAlloc_CreateVTable(CAlignOffsetAlloc *p);


EXTERN_C_END

#endif
