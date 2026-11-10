.686p
.xmm
.model flat

.data
align 10h
v1 dq 4330000000000000h
   dq 0
v5 dq 4530000000000000h
   dq 0
v2 dq 41F0000000000000h

_NaN dd 0FFFFFFFFh
_onept5 dd 3FC00000h

public ___isa_available
public ___isa_enabled
___isa_available dd ?
___isa_enabled dd 1

.code
public __ltod3
__ltod3 proc
    cmp     ___isa_available, 6
    jl     @f
    vmovd   xmm0, ecx
    vpinsrd xmm0, xmm0, edx, 1
    vcvtqq2pd xmm0, xmm0
    retn

@@:
    movd xmm0, ecx
    orps xmm0, xmmword ptr [v1]
    subsd xmm0, [v1]
    test edx,edx
    jz @f
    cvtsi2sd xmm1, edx
    mulsd xmm1,[v2]
    addsd xmm0,xmm1
@@: ret
__ltod3 endp

public __dtol3
__dtol3 proc
    cmp     ___isa_available, 6
    jl      @f
    vmovq   xmm0, xmm0
    vcvttpd2qq xmm0, xmm0
    vmovd   eax, xmm0
    vpextrd edx, xmm0, 1
    retn

@@:
    mov     ecx, esp
    add     esp, -8
    and     esp, -8
    movsd   qword ptr [esp], xmm0
    mov     eax, dword ptr [esp]
    mov     edx, dword ptr [esp+4]
    mov     esp, ecx
    btr     edx, 31
    sbb     ecx, ecx
    cmp     edx, 41E00000h
    jnb     @f
    cvttsd2si eax, xmm0
    cdq
    ret
@@: cmp edx, 43E00000h
    jnb @f
    test ecx, ecx
    jz _dtol3_getbits
    call _dtol3_getbits
    neg eax
    adc edx, 0
    neg edx
    ret
@@: jecxz @f
    ja @f
    test eax,eax
    jz L1
@@: cvttss2si ecx, [_NaN]
L1: mov edx, 80000000h
    xor eax, eax
    ret
__dtol3 endp

public __dtoul3
__dtoul3 proc
    cmp     ___isa_available, 6
    jl      @f
    vmovq   xmm0, xmm0
    vcvttpd2uqq xmm0, xmm0
    vmovd   eax, xmm0
    vpextrd edx, xmm0, 1
    retn

@@:
    mov     ecx, esp
    add     esp, 0FFFFFFF8h
    and     esp, 0FFFFFFF8h
    movsd   qword ptr [esp], xmm0
    mov     eax, dword ptr [esp]
    mov     edx, dword ptr [esp+4]
    mov     esp, ecx
    btr     edx, 1Fh
    jb      _dtoul3_neg
    cmp     edx, 41E00000h
    jnb     @f
    cvttsd2si eax, xmm0
    xor     edx, edx
    retn

@@:
    mov     ecx, edx
    bts     edx, 14h
    shr     ecx, 14h
    and     edx, 1FFFFFh
    sub     ecx, 433h
    jge     _dtoul3_large
    neg     ecx
    push    ebx
    xor     ebx, ebx
    shrd    ebx, eax, cl
    jz      @f
    cvttss2si ebx, [_onept5]

@@:
    pop     ebx
    shrd    eax, edx, cl
    shr     edx, cl
    retn


_dtoul3_large:
    cmp     ecx, 0Ch
    jnb     @f
    shld    edx, eax, cl
    shl     eax, cl
    retn


_dtoul3_neg:
    cmp     edx, 3FF00000h
    jnb     @f
    cvttsd2si eax, xmm0
    xor     eax, eax
    xor     edx, edx
    retn


@@:
    cvttss2si ecx, [_NaN]
    xor     eax, eax
    dec     eax
    cdq
    retn
__dtoul3 endp

public __dtoul3_legacy
__dtoul3_legacy proc
    add     esp, 0FFFFFFF8h
    movsd   qword ptr [esp], xmm0
    mov     edx, dword ptr [esp+4]
    add     esp, 8
    cmp     edx, 41E00000h
    jnb     @f
    cvttsd2si eax, xmm0
    xor     edx, edx
    retn
@@:
    cmp     edx, 43F00000h
    jnb     __dtol3
    jmp     __dtoul3
__dtoul3_legacy endp

_dtol3_getbits proc
    mov ecx, edx
    bts edx, 20
    shr ecx, 20
    and edx, 1FFFFFh
    sub ecx, 433h
    jge L2
    neg ecx
    push ebx
    xor ebx, ebx
    shrd ebx, eax, cl
    jz @f
    cvttss2si ebx, [_onept5]
@@: pop ebx
    shrd eax,edx,cl
    shr edx,cl
    ret
L2: shld edx, eax, cl
    shl eax, cl
    ret
_dtol3_getbits endp

public __ultod3
__ultod3 proc
    cmp     ___isa_available, 6
    jl      @f
    vmovd   xmm0, ecx
    vpinsrd xmm0, xmm0, edx, 1
    vcvtuqq2pd xmm0, xmm0
    retn

@@:
    movd xmm0, ecx
    orps xmm0, xmmword ptr [v1]
    subsd xmm0, [v1]
    test edx,edx
    jz @f
    movd xmm1, edx
    orps xmm1, xmmword ptr [v5]
    subsd xmm1, [v5]
    addsd xmm0,xmm1
@@: ret
__ultod3 endp

public __ftol2
public __ftol2_sse
extrn __ftol:near
__ftol2:
__ftol2_sse:
    jmp __ftol

public __byteswap_ulong
__byteswap_ulong proc
    mov  edx, [esp+4]
    mov  eax, edx
    push esi
    mov  esi, 0FF00h
    mov  ecx, edx
    and  eax, esi
    shl  ecx, 10h
    add  eax, ecx
    mov  ecx, edx
    shr  ecx, 8
    and  ecx, esi
    shl  eax, 8
    add  eax, ecx
    shr  edx, 18h
    add  eax, edx
    pop  esi
    retn
__byteswap_ulong endp

public __byteswap_uint64
__byteswap_uint64 proc
    mov     edi, edi
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    mov     esi, [ebp+0Ch]
    xor     edx, edx
    push    edi
    mov     edi, [ebp+8]
    mov     eax, esi
    mov     ecx, edi
    mov     ebx, edi
    shld    eax, ecx, 10h
    and     ebx, 0FF00h
    shl     ecx, 10h
    add     ebx, ecx
    push    0
    adc     edx, eax
    mov     eax, edi
    shld    edx, ebx, 10h
    and     eax, 0FF0000h
    shl     ebx, 10h
    add     ebx, eax
    mov     eax, edi
    pop     ecx
    adc     edx, ecx
    and     eax, 0FF000000h
    shld    edx, ebx, 10h
    push    0
    shl     ebx, 10h
    add     ebx, eax
    mov     eax, esi
    adc     edx, ecx
    shr     eax, 8
    shld    edx, ebx, 8
    and     eax, 0FF00h
    shl     ebx, 8
    add     ebx, eax
    mov     eax, esi
    adc     edx, ecx
    mov     ecx, edi
    shrd    ecx, eax, 8
    pop     eax
    and     ecx, 0FF000000h
    add     ebx, ecx
    mov     ecx, esi
    adc     edx, eax
    shrd    edi, ecx, 18h
    and     edi, 0FF0000h
    add     ebx, edi
    pop     edi
    adc     edx, eax
    shr     esi, 18h
    add     ebx, esi
    pop     esi
    adc     edx, eax
    mov     eax, ebx
    pop     ebx
    pop     ebp
    retn
__byteswap_uint64 endp

public __alldiv
__alldiv proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    edi
push    esi
push    ebx
xor     edi, edi
mov     eax, [esp+0Ch+arg_4]
or      eax, eax
jge     loc_7C92145C
inc     edi
mov     edx, [esp+0Ch+arg_0]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+0Ch+arg_4], eax
mov     [esp+0Ch+arg_0], edx

loc_7C92145C:
mov     eax, [esp+0Ch+arg_C]
or      eax, eax
jge     loc_7C921478
inc     edi
mov     edx, [esp+0Ch+arg_8]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+0Ch+arg_C], eax
mov     [esp+0Ch+arg_8], edx

loc_7C921478:
or      eax, eax
jnz     loc_7C921494
mov     ecx, [esp+0Ch+arg_8]
mov     eax, [esp+0Ch+arg_4]
xor     edx, edx
div     ecx
mov     ebx, eax
mov     eax, [esp+0Ch+arg_0]
div     ecx
mov     edx, ebx
jmp     loc_7C9214D5

loc_7C921494:
mov     ebx, eax
mov     ecx, [esp+0Ch+arg_8]
mov     edx, [esp+0Ch+arg_4]
mov     eax, [esp+0Ch+arg_0]

loc_7C9214A2:
shr     ebx, 1
rcr     ecx, 1
shr     edx, 1
rcr     eax, 1
or      ebx, ebx
jnz     loc_7C9214A2
div     ecx
mov     esi, eax
mul     [esp+0Ch+arg_C]
mov     ecx, eax
mov     eax, [esp+0Ch+arg_8]
mul     esi
add     edx, ecx
jb      loc_7C9214D0
cmp     edx, [esp+0Ch+arg_4]
ja      loc_7C9214D0
jb      loc_7C9214D1
cmp     eax, [esp+0Ch+arg_0]
jbe     loc_7C9214D1

loc_7C9214D0:
dec     esi

loc_7C9214D1:
xor     edx, edx
mov     eax, esi

loc_7C9214D5:
dec     edi
jnz     loc_7C9214DF
neg     edx
neg     eax
sbb     edx, 0

loc_7C9214DF:
pop     ebx
pop     esi
pop     edi
retn    10h
__alldiv endp


public __alldvrm
__alldvrm proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    edi
push    esi
push    ebp
xor     edi, edi
xor     ebp, ebp
mov     eax, [esp+0Ch+arg_4]
or      eax, eax
jge     loc_7C921509
inc     edi
inc     ebp
mov     edx, [esp+0Ch+arg_0]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+0Ch+arg_4], eax
mov     [esp+0Ch+arg_0], edx

loc_7C921509:
mov     eax, [esp+0Ch+arg_C]
or      eax, eax
jge     loc_7C921525
inc     edi
mov     edx, [esp+0Ch+arg_8]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+0Ch+arg_C], eax
mov     [esp+0Ch+arg_8], edx

loc_7C921525:
or      eax, eax
jnz     loc_7C921551
mov     ecx, [esp+0Ch+arg_8]
mov     eax, [esp+0Ch+arg_4]
xor     edx, edx
div     ecx
mov     ebx, eax
mov     eax, [esp+0Ch+arg_0]
div     ecx
mov     esi, eax
mov     eax, ebx
mul     [esp+0Ch+arg_8]
mov     ecx, eax
mov     eax, esi
mul     [esp+0Ch+arg_8]
add     edx, ecx
jmp     loc_7C921598

loc_7C921551:
mov     ebx, eax
mov     ecx, [esp+0Ch+arg_8]
mov     edx, [esp+0Ch+arg_4]
mov     eax, [esp+0Ch+arg_0]

loc_7C92155F:
shr     ebx, 1
rcr     ecx, 1
shr     edx, 1
rcr     eax, 1
or      ebx, ebx
jnz     loc_7C92155F
div     ecx
mov     esi, eax
mul     [esp+0Ch+arg_C]
mov     ecx, eax
mov     eax, [esp+0Ch+arg_8]
mul     esi
add     edx, ecx
jb      loc_7C92158D
cmp     edx, [esp+0Ch+arg_4]
ja      loc_7C92158D
jb      loc_7C921596
cmp     eax, [esp+0Ch+arg_0]
jbe     loc_7C921596

loc_7C92158D:
dec     esi
sub     eax, [esp+0Ch+arg_8]
sbb     edx, [esp+0Ch+arg_C]

loc_7C921596:
xor     ebx, ebx

loc_7C921598:
sub     eax, [esp+0Ch+arg_0]
sbb     edx, [esp+0Ch+arg_4]
dec     ebp
jns     loc_7C9215AA
neg     edx
neg     eax
sbb     edx, 0

loc_7C9215AA:
mov     ecx, edx
mov     edx, ebx
mov     ebx, ecx
mov     ecx, eax
mov     eax, esi
dec     edi
jnz     loc_7C9215BE
neg     edx
neg     eax
sbb     edx, 0

loc_7C9215BE:
pop     ebp
pop     esi
pop     edi
retn    10h
__alldvrm endp


public __allmul
__allmul proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

mov     eax, [esp+arg_4]
mov     ecx, [esp+arg_C]
or      ecx, eax
mov     ecx, [esp+arg_8]
jnz     loc_7C9215DD
mov     eax, [esp+arg_0]
mul     ecx
retn    10h

loc_7C9215DD:
push    ebx
mul     ecx
mov     ebx, eax
mov     eax, [esp+4+arg_0]
mul     [esp+4+arg_C]
add     ebx, eax
mov     eax, [esp+4+arg_0]
mul     ecx
add     edx, ebx
pop     ebx
retn    10h
__allmul endp


public __allrem
__allrem proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    ebx
push    edi
xor     edi, edi
mov     eax, [esp+8+arg_4]
or      eax, eax
jge     loc_7C921655
inc     edi
mov     edx, [esp+8+arg_0]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+8+arg_4], eax
mov     [esp+8+arg_0], edx

loc_7C921655:
mov     eax, [esp+8+arg_C]
or      eax, eax
jge     loc_7C921670
mov     edx, [esp+8+arg_8]
neg     eax
neg     edx
sbb     eax, 0
mov     [esp+8+arg_C], eax
mov     [esp+8+arg_8], edx

loc_7C921670:
or      eax, eax
jnz     loc_7C921691
mov     ecx, [esp+8+arg_8]
mov     eax, [esp+8+arg_4]
xor     edx, edx
div     ecx
mov     eax, [esp+8+arg_0]
div     ecx
mov     eax, edx
xor     edx, edx
dec     edi
js      loc_7C9216E4
jmp     loc_7C9216DD
jmp     loc_7C9216E4

loc_7C921691:
mov     ebx, eax
mov     ecx, [esp+8+arg_8]
mov     edx, [esp+8+arg_4]
mov     eax, [esp+8+arg_0]

loc_7C92169F:
shr     ebx, 1
rcr     ecx, 1
shr     edx, 1
rcr     eax, 1
or      ebx, ebx
jnz     loc_7C92169F
div     ecx
mov     ecx, eax
mul     [esp+8+arg_C]
xchg    eax, ecx
mul     [esp+8+arg_8]
add     edx, ecx
jb      loc_7C9216CA
cmp     edx, [esp+8+arg_4]
ja      loc_7C9216CA
jb      loc_7C9216D2
cmp     eax, [esp+8+arg_0]
jbe     loc_7C9216D2

loc_7C9216CA:
sub     eax, [esp+8+arg_8]
sbb     edx, [esp+8+arg_C]

loc_7C9216D2:
sub     eax, [esp+8+arg_0]
sbb     edx, [esp+8+arg_4]
dec     edi
jns     loc_7C9216E4

loc_7C9216DD:
neg     edx
neg     eax
sbb     edx, 0

loc_7C9216E4:
pop     edi
pop     ebx
retn    10h
__allrem endp


public __allshl
__allshl proc
cmp     cl, 40h ; '@'
jnb     loc_7C921703
cmp     cl, 20h ; ' '
jnb     loc_7C9216F9
shld    edx, eax, cl
shl     eax, cl
retn

loc_7C9216F9:
mov     edx, eax
xor     eax, eax
and     cl, 1Fh
shl     edx, cl
retn

loc_7C921703:
xor     eax, eax
xor     edx, edx
retn
__allshl endp


public __allshr
__allshr proc
cmp     cl, 40h ; '@'
jnb     loc_7C921723
cmp     cl, 20h ; ' '
jnb     loc_7C921718
shrd    eax, edx, cl
sar     edx, cl
retn

loc_7C921718:
mov     eax, edx
sar     edx, 1Fh
and     cl, 1Fh
sar     eax, cl
retn

loc_7C921723:
sar     edx, 1Fh
mov     eax, edx
retn
__allshr endp


public __aulldiv
__aulldiv proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    ebx
push    esi
mov     eax, [esp+8+arg_C]
or      eax, eax
jnz     loc_7C92174B
mov     ecx, [esp+8+arg_8]
mov     eax, [esp+8+arg_4]
xor     edx, edx
div     ecx
mov     ebx, eax
mov     eax, [esp+8+arg_0]
div     ecx
mov     edx, ebx
jmp     loc_7C92178C

loc_7C92174B:
mov     ecx, eax
mov     ebx, [esp+8+arg_8]
mov     edx, [esp+8+arg_4]
mov     eax, [esp+8+arg_0]

loc_7C921759:
shr     ecx, 1
rcr     ebx, 1
shr     edx, 1
rcr     eax, 1
or      ecx, ecx
jnz     loc_7C921759
div     ebx
mov     esi, eax
mul     [esp+8+arg_C]
mov     ecx, eax
mov     eax, [esp+8+arg_8]
mul     esi
add     edx, ecx
jb      loc_7C921787
cmp     edx, [esp+8+arg_4]
ja      loc_7C921787
jb      loc_7C921788
cmp     eax, [esp+8+arg_0]
jbe     loc_7C921788

loc_7C921787:
dec     esi

loc_7C921788:
xor     edx, edx
mov     eax, esi

loc_7C92178C:
pop     esi
pop     ebx
retn    10h
__aulldiv endp


public __aulldvrm
__aulldvrm proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    esi
mov     eax, [esp+4+arg_C]
or      eax, eax
jnz     loc_7C9217C2
mov     ecx, [esp+4+arg_8]
mov     eax, [esp+4+arg_4]
xor     edx, edx
div     ecx
mov     ebx, eax
mov     eax, [esp+4+arg_0]
div     ecx
mov     esi, eax
mov     eax, ebx
mul     [esp+4+arg_8]
mov     ecx, eax
mov     eax, esi
mul     [esp+4+arg_8]
add     edx, ecx
jmp     loc_7C921809

loc_7C9217C2:
mov     ecx, eax
mov     ebx, [esp+4+arg_8]
mov     edx, [esp+4+arg_4]
mov     eax, [esp+4+arg_0]

loc_7C9217D0:
shr     ecx, 1
rcr     ebx, 1
shr     edx, 1
rcr     eax, 1
or      ecx, ecx
jnz     loc_7C9217D0
div     ebx
mov     esi, eax
mul     [esp+4+arg_C]
mov     ecx, eax
mov     eax, [esp+4+arg_8]
mul     esi
add     edx, ecx
jb      loc_7C9217FE
cmp     edx, [esp+4+arg_4]
ja      loc_7C9217FE
jb      loc_7C921807
cmp     eax, [esp+4+arg_0]
jbe     loc_7C921807

loc_7C9217FE:
dec     esi
sub     eax, [esp+4+arg_8]
sbb     edx, [esp+4+arg_C]

loc_7C921807:
xor     ebx, ebx

loc_7C921809:
sub     eax, [esp+4+arg_0]
sbb     edx, [esp+4+arg_4]
neg     edx
neg     eax
sbb     edx, 0
mov     ecx, edx
mov     edx, ebx
mov     ebx, ecx
mov     ecx, eax
mov     eax, esi
pop     esi
retn    10h
__aulldvrm endp


public __aullrem
__aullrem proc

arg_0= dword ptr  4
arg_4= dword ptr  8
arg_8= dword ptr  0Ch
arg_C= dword ptr  10h

push    ebx
mov     eax, [esp+4+arg_C]
or      eax, eax
jnz     loc_7C921847
mov     ecx, [esp+4+arg_8]
mov     eax, [esp+4+arg_4]
xor     edx, edx
div     ecx
mov     eax, [esp+4+arg_0]
div     ecx
mov     eax, edx
xor     edx, edx
jmp     loc_7C921897

loc_7C921847:
mov     ecx, eax
mov     ebx, [esp+4+arg_8]
mov     edx, [esp+4+arg_4]
mov     eax, [esp+4+arg_0]

loc_7C921855:
shr     ecx, 1
rcr     ebx, 1
shr     edx, 1
rcr     eax, 1
or      ecx, ecx
jnz     loc_7C921855
div     ebx
mov     ecx, eax
mul     [esp+4+arg_C]
xchg    eax, ecx
mul     [esp+4+arg_8]
add     edx, ecx
jb      loc_7C921880
cmp     edx, [esp+4+arg_4]
ja      loc_7C921880
jb      loc_7C921888
cmp     eax, [esp+4+arg_0]
jbe     loc_7C921888

loc_7C921880:
sub     eax, [esp+4+arg_8]
sbb     edx, [esp+4+arg_C]

loc_7C921888:
sub     eax, [esp+4+arg_0]
sbb     edx, [esp+4+arg_4]
neg     edx
neg     eax
sbb     edx, 0

loc_7C921897:
pop     ebx
retn    10h
__aullrem endp


public __aullshr
__aullshr proc
cmp     cl, 40h ; '@'
jnb     loc_7C9218B5
cmp     cl, 20h ; ' '
jnb     loc_7C9218AB
shrd    eax, edx, cl
shr     edx, cl
retn

loc_7C9218AB:
mov     eax, edx
xor     edx, edx
and     cl, 1Fh
shr     eax, cl
retn

loc_7C9218B5:
xor     eax, eax
xor     edx, edx
retn
__aullshr endp

public __chkstk
__chkstk proc
    cmp     eax, 1000h
    jnb     @f
    neg     eax
    add     eax, esp
    add     eax, 4
    test    [eax], eax
    xchg    eax, esp
    mov     eax, [eax]
    push    eax
    retn

@@:
    push    ecx
    lea     ecx, [esp+8]

@@:
    sub     ecx, 1000h
    sub     eax, 1000h
    test    [ecx], eax
    cmp     eax, 1000h
    jnb     @b
    sub     ecx, eax
    mov     eax, esp
    test    [ecx], eax
    mov     esp, ecx
    mov     ecx, [eax]
    mov     eax, [eax+4]
    push    eax
    retn
__chkstk endp

public __ftol3
__ftol3 proc
    cmp     ___isa_available, 6
    jl      @f
    vbroadcastss xmm0, xmm0
    vcvttps2qq xmm0, xmm0
    vmovd   eax, xmm0
    vpextrd edx, xmm0, 1
    retn

@@:
    movd    eax, xmm0
    cdq
    shl     eax, 1
    cmp     eax, 9E000000h
    jnb     @f
    cvttss2si eax, xmm0
    cdq
    retn

@@:
    cmp     eax, 0BE000000h
    jnb     @f
    mov     ecx, eax
    bts     eax, 18h
    shr     ecx, 18h
    shl     eax, 7
    sub     cl, 0BEh
    xor     eax, edx
    sub     eax, edx
    shld    edx, eax, cl
    shl     eax, cl
    retn

@@:
    jnz     @f
    test    edx, edx
    js      _ftol3_retmin

@@:
    cvttss2si ecx, [_NaN]

_ftol3_retmin:
    mov     edx, 80000000h
    xor     eax, eax
    retn
__ftol3 endp

__ftoul3 proc
    cmp     ___isa_available, 6
    jl      @f
    vbroadcastss xmm0, xmm0
    vcvttps2uqq xmm0, xmm0
    vmovd   eax, xmm0
    vpextrd edx, xmm0, 1
    retn

@@:
    movd    eax, xmm0
    shl     eax, 1
    jb      _ftoul3_neg
    cmp     eax, 9E000000h
    jnb     @f

_ftoul3_cvt:
    cvttss2si eax, xmm0
    xor     edx, edx
    retn

@@:
    cmp     eax, 0BF000000h
    jnb     _ftoul3_NaN
    mov     ecx, eax
    bts     eax, 18h
    shr     ecx, 18h
    shl     eax, 7
    sub     cl, 0BEh
    jns     @f
    xor     edx, edx
    shld    edx, eax, cl
    shl     eax, cl
    retn

@@:
    mov     edx, eax
    xor     eax, eax
    retn

_ftoul3_neg:
    cmp     eax, 7F000000h
    jb      _ftoul3_cvt

_ftoul3_NaN:
    cvttss2si ecx, [_NaN]
    cmc
    sbb     eax, eax
    cdq
    retn
__ftoul3 endp

__ftoul3_legacy proc
    movd    eax, xmm0
    cmp     eax, 4F000000h
    jnb     @f
    cvttss2si eax, xmm0
    xor     edx, edx
    retn

@@:
    cmp     eax, 5F800000h
    jnb     __ftol3
    jmp     __ftoul3
__ftoul3_legacy endp

__ltof3 proc
    cmp     ___isa_available, 6
    jl      @f
    vmovd   xmm0, ecx
    vpinsrd xmm0, xmm0, edx, 1
    vcvtqq2ps xmm0, xmm0
    retn

@@:
    bt      ecx, 1Fh
    sbb     eax, eax
    cmp     eax, edx
    jnz     _ltof3_mt32
    cvtsi2ss xmm0, ecx
    retn

_ltof3_mt32:
    mov     eax, esp
    add     esp, 0FFFFFFF0h
    and     esp, 0FFFFFFF0h
    mov     [esp], ecx
    mov     [esp+4], edx
    fild    qword ptr [esp]
    fstp    dword ptr [esp]
    movss   xmm0, dword ptr [esp]
    mov     esp, eax
    retn
__ltof3 endp

__ultof3 proc
    cmp     ___isa_available, 6
    jl      @f
    vmovd   xmm0, ecx
    vpinsrd xmm0, xmm0, edx, 1
    vcvtuqq2ps xmm0, xmm0
    retn

@@:
    test    edx, edx
    js      _ultof3_mt63
    jnz     @f
    test    ecx, ecx
    js      @f
    cvtsi2ss xmm0, ecx
    retn

@@:
    mov     eax, esp
    add     esp, 0FFFFFFF0h
    and     esp, 0FFFFFFF0h
    mov     [esp], ecx
    mov     [esp+4], edx
    fild    qword ptr [esp]
    fstp    dword ptr [esp]
    movss   xmm0, dword ptr [esp]
    mov     esp, eax
    retn

_ultof3_mt63:
    push    ebp
    mov     ebp, esp
    add     esp, 0FFFFFFF0h
    and     esp, 0FFFFFFF0h
    mov     eax, ecx
    and     eax, 1
    shrd    ecx, edx, 1
    or      ecx, eax
    shr     edx, 1
    mov     [esp], ecx
    mov     [esp+4], edx
    fild    qword ptr [esp]
    fstp    dword ptr [esp]
    movss   xmm0, dword ptr [esp]
    addss   xmm0, xmm0
    mov     esp, ebp
    pop     ebp
    retn
__ultof3 endp

end
