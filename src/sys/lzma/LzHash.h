/* =============================================================================
 * src/sys/lzma/LzHash.h —— 本目录只有解码侧编进产品；编码侧只进构建期工具 tools/pack，且必须带 /D_7ZIP_ST（不链 LzFindMt/Threads：上游 Threads.c 用 __try，违反 AGENTS §5 规则 4）。哈希表常量。
 *
 * 来源：tools/PackEXE/pe/lzma（Igor Pavlov，Public domain），P1 迁入（Plan §3.1）。
 * 与上游的差异：无（逐字节保留上游实现）。
 * ========================================================================== */

/* LzHash.h -- HASH functions for LZ algorithms
2015-04-12 : Igor Pavlov : Public domain */

#ifndef __LZ_HASH_H
#define __LZ_HASH_H

#define kHash2Size (1 << 10)
#define kHash3Size (1 << 16)
#define kHash4Size (1 << 20)

#define kFix3HashSize (kHash2Size)
#define kFix4HashSize (kHash2Size + kHash3Size)
#define kFix5HashSize (kHash2Size + kHash3Size + kHash4Size)

#define HASH2_CALC hv = cur[0] | ((UInt32)cur[1] << 8);

#define HASH3_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  hv = (temp ^ ((UInt32)cur[2] << 8)) & p->hashMask; }

#define HASH4_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  temp ^= ((UInt32)cur[2] << 8); \
  h3 = temp & (kHash3Size - 1); \
  hv = (temp ^ (p->crc[cur[3]] << 5)) & p->hashMask; }

#define HASH5_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  temp ^= ((UInt32)cur[2] << 8); \
  h3 = temp & (kHash3Size - 1); \
  temp ^= (p->crc[cur[3]] << 5); \
  h4 = temp & (kHash4Size - 1); \
  hv = (temp ^ (p->crc[cur[4]] << 3)) & p->hashMask; }

/* #define HASH_ZIP_CALC hv = ((cur[0] | ((UInt32)cur[1] << 8)) ^ p->crc[cur[2]]) & 0xFFFF; */
#define HASH_ZIP_CALC hv = ((cur[2] | ((UInt32)cur[0] << 8)) ^ p->crc[cur[1]]) & 0xFFFF;


#define MT_HASH2_CALC \
  h2 = (p->crc[cur[0]] ^ cur[1]) & (kHash2Size - 1);

#define MT_HASH3_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  h3 = (temp ^ ((UInt32)cur[2] << 8)) & (kHash3Size - 1); }

#define MT_HASH4_CALC { \
  UInt32 temp = p->crc[cur[0]] ^ cur[1]; \
  h2 = temp & (kHash2Size - 1); \
  temp ^= ((UInt32)cur[2] << 8); \
  h3 = temp & (kHash3Size - 1); \
  h4 = (temp ^ (p->crc[cur[3]] << 5)) & (kHash4Size - 1); }

#endif
