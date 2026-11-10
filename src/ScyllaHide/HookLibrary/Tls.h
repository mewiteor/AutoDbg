#pragma once

#include <ntdll/ntdll.h>

// Hackplementation of thread local storage without using the CRT or LdrpAllocateTls

#define TebAllocationSize ((sizeof(TEB) + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1)))

#ifdef _WIN64
// On x64 we can freely write past the end of the TEB since 2 zeroed pages are allocated for it. Leave some headroom for the TEB to grow in future Windows versions
#define TebPadding 0x200 // +512
#else
// On x86 and Wow64 we have a problem because sizeof(TEB) == PAGE_SIZE == TebAllocationSize, i.e. there are no spare zeroes past the end of the TEB, at least on Win 10.
// Instead abuse the SpareBytes field for this. Because (1) this field has a slightly different offset on different versions of Windows (+1AC for 7 vs +1B9 for 10),
// and (2) this field is not pointer-aligned, round the address up to pointer alignment. The offset is negative from the end since we are writing to the TEB, not past it
#define TebPaddingFromEnd ((LONG_PTR)(TebAllocationSize) - (LONG_PTR)FIELD_OFFSET(TEB, SpareBytes)) // 4096 - 441 = 3655
#define TebPadding (((-1 * TebPaddingFromEnd) + (LONG_PTR)_Alignof(PVOID) - 1) & (~((LONG_PTR)_Alignof(PVOID) - 1))) // ALIGN_UP(-1 * 3655, PVOID) = -3652
static_assert(TebPadding == -3652, "You touched ntdll.h didn't you?");
#endif

// To create a TLS variable, declare it here
typedef enum
{
	InstrumentationCallbackDisabled, // The only TLS variable we currently actually use...
	MaxTlsVariable // Must be last
} TlsVariable;

#define TebOffset(Variable) (((LONG_PTR)(sizeof(TEB)) + TebPadding) + ((ULONG_PTR)(Variable) * _Alignof(PVOID)))

static_assert(TebOffset(MaxTlsVariable) <= TebAllocationSize - sizeof(PVOID), "TLS variable offsets exceed TEB allocation size");
static_assert((ULONG_PTR)MaxTlsVariable - 1 <= 5, "All out of TEB SpareBytes, find some new field to abuse"); // Only really applies to x86, but check on both

FORCEINLINE
volatile
LONG*
TlsGetInstrumentationCallbackDisabled(
	void
	)
{
	return (volatile LONG *)((ULONG_PTR)NtCurrentTeb() + TebOffset(InstrumentationCallbackDisabled));
}
