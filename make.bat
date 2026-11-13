@echo off
setlocal enabledelayedexpansion

set ROOT=%~dp0
set SRCDIR=%~dp0src
set THIRDPARTDIR=%~dp0thirdpart
set BUILDROOT=%ROOT%.build
set INSTALLROOT=%ROOT%.out
set BUILD_MODE=release
set ARCH_ARG=

:next
if "x%1" == "x" goto start
:: clean
if "x%1" == "xc" (
    rmdir /s /q %BUILDROOT% || exit /b 1
    exit /b
)
if "x%1" == "xr" (
    if exist %BUILDROOT% rmdir /s /q %BUILDROOT% || exit /b 1
    shift
    goto next
)
if "x%1" == "xb" (
    set BUILDROOT=%2
    shift
    shift
    goto next
)
if "x%1" == "xi" (
    set INSTALLROOT=%2
    shift
    shift
    goto next
)
if "x%1" == "xd" (
    set BUILD_MODE=debug
    shift
    goto next
)
if "x%1" == "xa" (
    set ARCH_ARG=%2
    shift
    shift
    goto next
)
goto usage

:start
echo 源目录: %ROOT%
echo build目录: %BUILDROOT%
echo build模式: %BUILD_MODE%

if not exist %INSTALLROOT% mkdir %INSTALLROOT%

set GEN_BUILD=1

if "x%ARCH_ARG%" == "x" (
    call :config 64 || goto err
    call :config 86 || goto err
) else (
    echo arch: %ARCH_ARG%
    :: a: 只生成 compile_commands.json（不构建）——单架构拿不到另一架构的产物，
    ::     主 exe 是 x86 且要内嵌 x64 blob（T8/D63），所以构建只在没有 a 时进行
    set GEN_COMPILE=1
    set GEN_BUILD=0
    call :config %ARCH_ARG% || goto err
)
exit /b

:: config arch
::   arch 只剩 64 / 86；98 已退役（D1）—— 老系统口径改由 legacy 变体（--variant=legacy）提供
:config
setlocal
if "x%1"=="x98" (
    echo error: arch 98 已退役，请用 make.bat a 86 构建，运行时用 --variant=legacy 选择老系统变体
    exit /b 1
)
if not "x%1"=="x64" if not "x%1"=="x86" (
    echo error: unknown arch %1 [只支持 64 / 86]
    exit /b 1
)
if "x%1"=="x64" (
    call :loadvcvars x64 || exit /b 1
) else (
    call :loadvcvars x86 || exit /b 1
)
call :build %1 || exit /b 1
exit /b

:: build arch
:build
set ARCH=%1
set BUILDDIR=%BUILDROOT%\%1\%BUILD_MODE%
if not exist %BUILDDIR% mkdir %BUILDDIR%
if not exist %BUILDDIR%\lua\include mkdir %BUILDDIR%\lua\include
if not exist %BUILDDIR%\luajit\include mkdir %BUILDDIR%\luajit\include
if not exist %BUILDDIR%\luajit\host mkdir %BUILDDIR%\luajit\host
if not exist %BUILDDIR%\src\common mkdir %BUILDDIR%\src\common
if not exist %BUILDDIR%\src\core mkdir %BUILDDIR%\src\core
if not exist %BUILDDIR%\src\scripting\lua mkdir %BUILDDIR%\src\scripting\lua
if not exist %BUILDDIR%\src\sys mkdir %BUILDDIR%\src\sys
if not exist %BUILDDIR%\datas mkdir %BUILDDIR%\datas
if not exist %BUILDDIR%\tools mkdir %BUILDDIR%\tools
if not exist %BUILDDIR%\pack mkdir %BUILDDIR%\pack
if not exist %BUILDDIR%\pever mkdir %BUILDDIR%\pever
if not exist %BUILDDIR%\launcher mkdir %BUILDDIR%\launcher
if not exist %BUILDDIR%\gen mkdir %BUILDDIR%\gen
if not exist %BUILDDIR%\src\sys\lzma mkdir %BUILDDIR%\src\sys\lzma
if not exist %BUILDDIR%\tests\fixtures\bin mkdir %BUILDDIR%\tests\fixtures\bin

if %ARCH% == 64 (
    set SUBSYS_VER=
    set PACK_BLOB_ESC=
    set PACK_FLAGS=-quiet --no-blob
) else (
    set SUBSYS_VER=,5.01
    set PACK_BLOB_ESC=%INSTALLROOT::=$:%\AutoDbg_64.exe
    set PACK_FLAGS=-quiet --blob=%INSTALLROOT%\AutoDbg_64.exe
)

> %BUILDDIR%\build.ninja echo # build.ninja - lualib ^(%1 %BUILD_MODE%^)  [generated, do not edit]
>> %BUILDDIR%\build.ninja echo ninja_required_version = 1.5
>> %BUILDDIR%\build.ninja echo # 若 cl 为中文输出，取消下行注释并核对前缀：
>> %BUILDDIR%\build.ninja echo msvc_deps_prefix = 注意: 包含文件:
>> %BUILDDIR%\build.ninja echo.

set CFLAGS=/nologo /c /W3 /GS- /Gy- /GR- /Oi- /I%ROOT%src\sys\include /I%BUILDDIR%\gen /D_CRT_SECURE_NO_DEPRECATE /D_NO_CRT_STDIO_INLINE
set LDFLAGS=/nologo /nodefaultlib /safeseh:no /incremental:no /stub:t /merge:.rdata=.text /emitpogophaseinfo /ignore:4108 /subsystem:console%SUBSYS_VER%
set MLEXE=ml64.exe
set MFLAGS=/nologo /c
set MACHINE=x64
:: MACHINE 还被用于文件名 InstrumentationCallback%MACHINE%.asm（实际文件是大写 X64/X86，靠文件系统大小写不敏感匹配）
set LINK_DEPS=
if %BUILD_MODE% == debug (
    set CFLAGS=%CFLAGS% /Zi /MTd /DDEBUG /D_DEBUG
    set LDFLAGS=%LDFLAGS% /debug
    set MFLAGS=%MFLAGS% /Zi
) else (
    set CFLAGS=%CFLAGS% /O1 /MT /DNDEBUG
)
set TIME_T_FLAGS=
set PATCHVER=
:: 注意：LUAJIT_NO_UNWIND 只给 x86（D19）——手工映射的 x86 模块不能用 SEH。
:: x64 反过来必须保持 LuaJIT 默认的 SEH 路径：内部 unwind 布局在 x64/WIN 上与 asm 不一致，
:: 会破坏 r14/r15（症状：lualib 烘焙块只装进第一个、循环变量被写成 -1）。
:: x64 的 .pdata 已由映射器注册（F5），SEH 在手工映射模块里可用。
set LJ_UNWIND=
if not %1 == 64 (
    set CFLAGS=%CFLAGS% /D_USE_32BIT_TIME_T /D_CRTIMP_TYPEINFO
    REM x86 影子 time.h 的 time_t 是 long，MSVC corecrt.h 需同步成 32 位
    set TIME_T_FLAGS=/D_USE_32BIT_TIME_T
    set MLEXE=ml.exe
    set MACHINE=x86
    set LINK_DEPS=src\sys\flt.obj
    set PATCHVER=1
    set LJ_UNWIND=/DLUAJIT_NO_UNWIND
    set TARGET_ARG=--target=i686-pc-windows-msvc
) else (
    set LINK_DEPS=src\sys\ntdll.lib src\sys\ucrtbase.lib
    set PATCHVER=
    set TARGET_ARG=--target=x86_64-pc-windows-msvc
)
:: 架构只剩 64 / 86，公共基线一律 modern 口径（UNICODE + /utf-8）；
:: legacy 口径由 CFLAGS_V9X（ccv9x / cxxv9x）承担，见 D18
set CHARSET_FLAGS=/utf-8
set CFLAGS=%CFLAGS% /D_UNICODE /DUNICODE /utf-8
:: legacy 变体专用 CFLAGS：/utf-8 与 /execution-charset:gbk 互斥（D8016），
:: 所以给 legacy 一套独立规则（ccv9x/cxxv9x），而不是在基线上追加
set CFLAGS_V9X=!CFLAGS:/utf-8=/source-charset:utf-8 /execution-charset:gbk!
set CFLAGS_V9X=!CFLAGS_V9X:/D_UNICODE /DUNICODE =!

:: 生成rules
>>%BUILDDIR%\build.ninja echo rule gendata
>>%BUILDDIR%\build.ninja echo   command = $lua %ROOT%tools\gen_lua_lib.lua $out $in
>>%BUILDDIR%\build.ninja echo   description = GEN DATA $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule genhookdata
>>%BUILDDIR%\build.ninja echo   command = $lua %ROOT%tools\gen_hook_data.lua $out $in
>>%BUILDDIR%\build.ninja echo   description = GEN HOOK DATA $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule cp
>>%BUILDDIR%\build.ninja echo   command = cmd /c copy /y $in $out ^> nul
>>%BUILDDIR%\build.ninja echo   description = copy from $in to $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule t
>>%BUILDDIR%\build.ninja echo   command = cmd /c echo MZ............................................................^> $out
>>%BUILDDIR%\build.ninja echo   description = gen $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule pever
>>%BUILDDIR%\build.ninja echo   command = cmd /c copy /y $in $out ^> nul ^&^& $pevertool -Q $out 4.0
>>%BUILDDIR%\build.ninja echo   description = PEVER $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule pack
>>%BUILDDIR%\build.ninja echo   command = $packtool $out $flags $in
>>%BUILDDIR%\build.ninja echo   description = PACK $out
>>%BUILDDIR%\build.ninja echo.
set CFLAGS_JSON=%CFLAGS:\=\\% /std:c17
>>%BUILDDIR%\build.ninja echo rule cc
>>%BUILDDIR%\build.ninja echo   command = cl.exe /showIncludes %CFLAGS% /std:c17 $flags /Fd$out.pdb /Fo$out $in
>>%BUILDDIR%\build.ninja echo   description = CC $out
>>%BUILDDIR%\build.ninja echo   deps = msvc
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule ccx
>>%BUILDDIR%\build.ninja echo   command = cl.exe /showIncludes $flags /Fd$out.pdb /Fo$out $in
>>%BUILDDIR%\build.ninja echo   description = CCX $out
>>%BUILDDIR%\build.ninja echo   deps = msvc
>>%BUILDDIR%\build.ninja echo.
set CXXFLAGS_JSON=%CFLAGS:\=\\% /std:c++20 /Zc:__cplusplus /Zc:preprocessor
>>%BUILDDIR%\build.ninja echo rule cxx
>>%BUILDDIR%\build.ninja echo   command = cl.exe /showIncludes %CFLAGS% /std:c++20 /Zc:__cplusplus /Zc:preprocessor $flags /Fd$out.pdb /Fo$out $in
>>%BUILDDIR%\build.ninja echo   description = CXX $out
>>%BUILDDIR%\build.ninja echo   deps = msvc
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule ccv9x
>>%BUILDDIR%\build.ninja echo   command = cl.exe /showIncludes !CFLAGS_V9X! /std:c17 $flags /Fd$out.pdb /Fo$out $in
>>%BUILDDIR%\build.ninja echo   description = CC9X $out
>>%BUILDDIR%\build.ninja echo   deps = msvc
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule cxxv9x
>>%BUILDDIR%\build.ninja echo   command = cl.exe /showIncludes !CFLAGS_V9X! /std:c++20 /Zc:__cplusplus /Zc:preprocessor $flags /Fd$out.pdb /Fo$out $in
>>%BUILDDIR%\build.ninja echo   description = CXX9X $out
>>%BUILDDIR%\build.ninja echo   deps = msvc
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule ld
>>%BUILDDIR%\build.ninja echo   command = link.exe %LDFLAGS% $flags /out:$out /pdb:$out.pdb $in $syslib
>>%BUILDDIR%\build.ninja echo   description = LD $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule ldx
>>%BUILDDIR%\build.ninja echo   command = link.exe /nologo $flags /out:$out /pdb:$out.pdb $in $syslib
>>%BUILDDIR%\build.ninja echo   description = LDX $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule as
>>%BUILDDIR%\build.ninja echo   command = cmd /c !MLEXE! %MFLAGS% /Fo$out $in ^> nul
>>%BUILDDIR%\build.ninja echo   description = AS $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule ar
>>%BUILDDIR%\build.ninja echo   command = lib.exe /nologo /machine:%MACHINE% /out:$out $in
>>%BUILDDIR%\build.ninja echo   description = AR $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule imp
>>%BUILDDIR%\build.ninja echo   command = cmd /c lib.exe /nologo /machine:%MACHINE% /out:$out /def:$in ^> nul
>>%BUILDDIR%\build.ninja echo   description = IMP $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule dasm
>>%BUILDDIR%\build.ninja echo   command = $lua $script -LN -D WIN -D JIT -D FFI -D ENDIAN_LE -D FPU $flags -o $out $in
>>%BUILDDIR%\build.ninja echo   description = DASM $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule genvm
>>%BUILDDIR%\build.ninja echo   command = $vm -m $mode -o $out $in
>>%BUILDDIR%\build.ninja echo   description = GENVM $mode $out
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo rule genver
>>%BUILDDIR%\build.ninja echo   command = $lua $script $in $out
>>%BUILDDIR%\build.ninja echo   description = GENVER $out
>>%BUILDDIR%\build.ninja echo   restat = 1
>>%BUILDDIR%\build.ninja echo.

:: compile_commands.json 只在单架构模式（a 64 / a 86）生成。
:: 多变体：src/** 会被 flavor x variant 各编一次，所以同一源文件有 4 条 entry
:: （lua+modern 先写，其后 luajit+modern、lua+legacy、luajit+legacy）—— clangd 取第一条
:: （lua+modern，UNICODE + /utf-8）；legacy 与 luajit 的差异见 ccv9x/cxxv9x 与 /DJIT。
:: 条目成倍增长是预期行为，不是重复 bug。
if "x%GEN_COMPILE%" == "x1" echo [>%ROOT%compile_commands.json

:: 生成sys
>>%BUILDDIR%\build.ninja echo build t: t
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\main.obj: cc %SRCDIR::=$:%\sys\main.c
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\strtoull.obj: cc %SRCDIR::=$:%\sys\strtoull.c
:: destruct_eh.c: LuaJIT 的 __DestructExceptionObject（Win9x 的 msvcrt.dll 没有它，见文件头）
>>%BUILDDIR%\build.ninja echo build src\sys\destruct_eh.obj: cc %SRCDIR::=$:%\sys\destruct_eh.c
>>%BUILDDIR%\build.ninja echo.
if "x%GEN_COMPILE%" == "x1" echo {"directory":"%BUILDDIR:\=\\%","file":"%SRCDIR:\=\\%\\sys\\main.c","command":"cl.exe %TARGET_ARG% %CFLAGS_JSON% %SRCDIR:\=\\%\\sys\\main.c"}>>%ROOT%compile_commands.json
if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"%SRCDIR:\=\\%\\sys\\strtoull.c","command":"cl.exe %TARGET_ARG% %CFLAGS_JSON% %SRCDIR:\=\\%\\sys\\strtoull.c"}>>%ROOT%compile_commands.json
>>%BUILDDIR%\build.ninja echo build src\sys\ntdll.lib: imp %SRCDIR::=$:%\sys\ntdll.def
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\ucrtbase.lib: imp %SRCDIR::=$:%\sys\ucrtbase.def
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\flt.obj: as %SRCDIR::=$:%\sys\flt.asm
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\msvcrt.lib: imp %SRCDIR::=$:%\sys\msvcrt.def
>>%BUILDDIR%\build.ninja echo build src\sys\lzma\Alloc.obj: cc %SRCDIR::=$:%\sys\lzma\Alloc.c
>>%BUILDDIR%\build.ninja echo   flags = /D_7ZIP_ST
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\lzma\LzFind.obj: cc %SRCDIR::=$:%\sys\lzma\LzFind.c
>>%BUILDDIR%\build.ninja echo   flags = /D_7ZIP_ST
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\lzma\LzmaEnc.obj: cc %SRCDIR::=$:%\sys\lzma\LzmaEnc.c
>>%BUILDDIR%\build.ninja echo   flags = /D_7ZIP_ST
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\sys\lzma\LzmaDec.obj: cc %SRCDIR::=$:%\sys\lzma\LzmaDec.c
>>%BUILDDIR%\build.ninja echo   flags = /D_7ZIP_ST
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build src\common\allocator.obj: cxx %SRCDIR::=$:%\common\allocator.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build tools\pever.obj: cxx %ROOT::=$:%tools\pever\pever.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build tools\pack.obj: cxx %ROOT::=$:%tools\pack\pack.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\tools\pever.exe: ld tools\pever.obj src\common\allocator.obj src\sys\main.obj src\sys\msvcrt.lib %LINK_DEPS% ^| t
>>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib user32.lib comdlg32.lib
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\tools\pack.exe: ld tools\pack.obj src\sys\lzma\Alloc.obj src\sys\lzma\LzFind.obj src\sys\lzma\LzmaEnc.obj src\sys\lzma\LzmaDec.obj src\common\allocator.obj src\sys\main.obj src\sys\msvcrt.lib %LINK_DEPS% ^| t
>>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo.

REM 生成ScyllaHide（64 / 86 都要：modern 变体链接它，D1）
    
    REM 生成distorm
    set TP_CFLAGS=/DDISTORM_STATIC /DSUPPORT_64BIT_OFFSET /DDISTORM_LIGHT
    set OBJS=
    set EXCLUDE=
    call :build_ccs %SRCDIR%\ScyllaHide\3rdparty\distorm\*.c src\ScyllaHide\3rdparty\distorm
    call :build_ar src\ScyllaHide\distorm.lib
    
    REM 生成Scylla
    set TP_CFLAGS=/DWIN32_NO_STATUS /I%SRCDIR%\ScyllaHide /I%SRCDIR%\ScyllaHide\3rdparty
    :: ScyllaHide 的 psapi 用法必须按 PSAPI_VERSION=1 编：默认(=2) 会被 psapi.h 宏映射成 Vista+ 的
    :: K32EnumProcessModules / K32GetModuleFileNameExW，XP 上解析导入即失败（x64 用 K32*，不受影响）
    if %ARCH% == 86 set TP_CFLAGS=!TP_CFLAGS! /D_CRTIMP_TYPEINFO /DPSAPI_VERSION=1
    set EXCLUDE=VersionPatch.c
    set OBJS=
    call :build_ccs %SRCDIR%\ScyllaHide\Scylla\*.c src\ScyllaHide\Scylla
    call :build_ar src\ScyllaHide\Scylla.lib
    
    REM 生成HookLibrary
    if not exist %BUILDDIR%\src\ScyllaHide\HookLibrary mkdir %BUILDDIR%\src\ScyllaHide\HookLibrary
    set OBJS=
    set TP_CFLAGS=/nologo /c /GS- /MT /O1 /Oi- /std:c11 /DUNICODE /D_UNICODE /DWIN32_NO_STATUS /D_CRT_SECURE_NO_WARNINGS /I%SRCDIR%\ScyllaHide /I%SRCDIR%\ScyllaHide\3rdparty
    for %%i in (%SRCDIR%\ScyllaHide\HookLibrary\*.c) do (
        set SRC=%%i
        set OBJ=src\ScyllaHide\HookLibrary\%%~ni.obj
        set OBJS=!OBJS! !OBJ!
        echo build !OBJ!: ccx !SRC::=$:!>> %BUILDDIR%\build.ninja
        echo   flags = !TP_CFLAGS!>> %BUILDDIR%\build.ninja
        echo.>> %BUILDDIR%\build.ninja
        if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% !TP_CFLAGS:\=\\! !SRC:\=\\!"}>>%ROOT%compile_commands.json
    )
    set OBJS=!OBJS! src\ScyllaHide\Scylla\VersionPatch_hk.obj
    echo build src\ScyllaHide\Scylla\VersionPatch_hk.obj: ccx %SRCDIR::=$:%\ScyllaHide\Scylla\VersionPatch.c>> %BUILDDIR%\build.ninja
    echo   flags = !TP_CFLAGS!>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja
    if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRCDIR:\=\\!\\ScyllaHide\\Scylla\\VersionPatch.c","command":"cl.exe %TARGET_ARG% !TP_CFLAGS:\=\\! !SRCDIR:\=\\!\\ScyllaHide\\Scylla\\VersionPatch.c"}>>%ROOT%compile_commands.json

    set OBJS=!OBJS! src\ScyllaHide\HookLibrary\InstrumentationCallback%MACHINE%.obj
    echo build src\ScyllaHide\HookLibrary\InstrumentationCallback%MACHINE%.obj: as %SRCDIR::=$:%\ScyllaHide\HookLibrary\InstrumentationCallback%MACHINE%.asm>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja

    echo build src\ScyllaHide\HookLibrary.dll: ldx !OBJS!>> %BUILDDIR%\build.ninja
    echo   flags = /subsystem:windows%SUBSYS_VER% /nodefaultlib /incremental:no /manifest:no /debug:none /novcfeature /nocoffgrpinfo %SRCDIR%\ScyllaHide\3rdparty\ntdll\ntdllp_%MACHINE%.lib>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja

    echo build src\ScyllaHide\ScyllaHide\HookLibraryData.c ^| src\ScyllaHide\ScyllaHide\HookLibraryData.h : genhookdata src\ScyllaHide\HookLibrary.dll ^| %INSTALLROOT::=$:%\lua_%1.exe %ROOT::=$:%tools\gen_hook_data.lua>> %BUILDDIR%\build.ninja
    echo   lua=%INSTALLROOT%\lua_%1.exe>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja
    
    REM 生成ScyllaHide
    set TP_CFLAGS=/I%SRCDIR%\ScyllaHide /I%SRCDIR%\ScyllaHide\3rdparty /DWIN32_NO_STATUS /Isrc\ScyllaHide\ScyllaHide
    :: ScyllaHide 的 psapi 用法必须按 PSAPI_VERSION=1 编：默认(=2) 会被 psapi.h 宏映射成 Vista+ 的
    :: K32EnumProcessModules / K32GetModuleFileNameExW，XP 上解析导入即失败（x64 用 K32*，不受影响）
    if %ARCH% == 86 set TP_CFLAGS=!TP_CFLAGS! /D_CRTIMP_TYPEINFO /DPSAPI_VERSION=1
    set OBJS=
    set EXCLUDE=
    set ODEPS=src\ScyllaHide\ScyllaHide\HookLibraryData.h
    call :build_ccs %SRCDIR%\ScyllaHide\InjectorCLI\*.c src\ScyllaHide\InjectorCLI
    call :build_ccs %SRCDIR%\ScyllaHide\PluginGeneric\*.c src\ScyllaHide\PluginGeneric
    call :build_ccs %SRCDIR%\ScyllaHide\Scylla\VersionPatch.c src\ScyllaHide\Scylla
    call :build_ccs %SRCDIR%\ScyllaHide\ScyllaHide\*.c src\ScyllaHide\ScyllaHide
    call :build_ccs src\ScyllaHide\ScyllaHide\HookLibraryData.c src\ScyllaHide\ScyllaHide
    call :build_ar src\ScyllaHide\ScyllaHide.lib
    set ODEPS=
    set EXCLUDE=
    set TP_CFLAGS=
    set OBJS=

:: 生成lua
set OBJS=
for %%i in (%THIRDPARTDIR%\lua\*.c) do (
    if not %%~ni == luac (
        if not %%~ni == onelua (
            set SRC=%%i
            set OBJ=lua\%%~ni.obj
            if not %%~ni == lua set OBJS=!OBJS! !OBJ!
            echo build !OBJ!: cc !SRC::=$:! >> %BUILDDIR%\build.ninja
            echo. >> %BUILDDIR%\build.ninja
            if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% !CFLAGS_JSON! !SRC:\=\\!"}>>%ROOT%compile_commands.json
        )
    )
)
>>%BUILDDIR%\build.ninja echo build lua\lua.lib: ar !OBJS!
>>%BUILDDIR%\build.ninja echo.

if "x%PATCHVER%" == "x1" (
    >>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\lua_%1.exe: ld lua\lua.obj src\sys\main.obj %LINK_DEPS% lua\lua.lib src\sys\msvcrt.lib ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
    >>%BUILDDIR%\build.ninja echo.
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\lua_%1.exe: pever %BUILDDIR::=$:%\pever\lua_%1.exe ^| %BUILDDIR::=$:%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo.
) else (
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\lua_%1.exe: ld lua\lua.obj src\sys\main.obj %LINK_DEPS% lua\lua.lib src\sys\msvcrt.lib ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
    >>%BUILDDIR%\build.ninja echo.
)
>>%BUILDDIR%\build.ninja echo build lua\include\lua.h: cp %THIRDPARTDIR::=$:%\lua\lua.h
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build lua\include\luaconf.h: cp %THIRDPARTDIR::=$:%\lua\luaconf.h
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build lua\include\lualib.h: cp %THIRDPARTDIR::=$:%\lua\lualib.h
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build lua\include\lauxlib.h: cp %THIRDPARTDIR::=$:%\lua\lauxlib.h
>>%BUILDDIR%\build.ninja echo.
REM lua.hpp 内容固定：只在缺失时生成（每次都重写会让所有依赖它的编译单元每次全量重编）
if not exist %BUILDDIR%\lua\include\lua.hpp (
echo #ifndef _LUA_HPP_>%BUILDDIR%\lua\include\lua.hpp
echo #define _LUA_HPP_>>%BUILDDIR%\lua\include\lua.hpp
echo extern "C" {>>%BUILDDIR%\lua\include\lua.hpp
echo     #include "lua.h">>%BUILDDIR%\lua\include\lua.hpp
echo     #include "lualib.h">>%BUILDDIR%\lua\include\lua.hpp
echo     #include "lauxlib.h">>%BUILDDIR%\lua\include\lua.hpp
echo }>>%BUILDDIR%\lua\include\lua.hpp
echo #endif // _LUA_HPP_>>%BUILDDIR%\lua\include\lua.hpp
)

:: 生成luajit（minilua / buildvm 是构建期工具，走真 CRT，不进 .out）
:: luajit\host：buildvm.c 里 #include "buildvm_arch.h" 的搜索路径
:: luajit\include：生成的 luajit.h 必须排在子模块源码目录之前
:: luajit：生成物 lj_bcdef.h 等与 luajit.h 同名目录下的头，供 luajit.lib 的编译单元使用
:: /FIlj.h /DAUTODBG_WIN9X：GetModuleHandleExA 垫片必须覆盖 luajit.lib 的编译单元
::   （lj_clib.c / lib_package.c 的调用点在里面；只在 :buildautodbg 加是无效的）
set HOST_CFLAGS=/nologo /c /W3 /O2 /std:c11 /D_CRT_SECURE_NO_DEPRECATE /Iluajit\include /Iluajit\host /I%THIRDPARTDIR%\luajit\src /I%THIRDPARTDIR%\luajit\src\host /I%THIRDPARTDIR%\luajit\dynasm
set LJ_CFLAGS=/nologo /c /GS- /MT /O1 /Oi- /std:c11 %CHARSET_FLAGS% %TIME_T_FLAGS% /D_CRT_SECURE_NO_DEPRECATE /D_NO_CRT_STDIO_INLINE /FIlj.h /DAUTODBG_WIN9X %LJ_UNWIND% /Iluajit\include /Iluajit /I%SRCDIR%\sys\include /I%THIRDPARTDIR%\luajit\src
set LJ_LIBS=%THIRDPARTDIR::=$:%\luajit\src\lib_base.c %THIRDPARTDIR::=$:%\luajit\src\lib_math.c %THIRDPARTDIR::=$:%\luajit\src\lib_bit.c %THIRDPARTDIR::=$:%\luajit\src\lib_string.c %THIRDPARTDIR::=$:%\luajit\src\lib_table.c %THIRDPARTDIR::=$:%\luajit\src\lib_io.c %THIRDPARTDIR::=$:%\luajit\src\lib_os.c %THIRDPARTDIR::=$:%\luajit\src\lib_package.c %THIRDPARTDIR::=$:%\luajit\src\lib_debug.c %THIRDPARTDIR::=$:%\luajit\src\lib_jit.c %THIRDPARTDIR::=$:%\luajit\src\lib_ffi.c %THIRDPARTDIR::=$:%\luajit\src\lib_buffer.c
:: 滚动版本号 = luajit 子模块最新提交时间；git 不可用则留空文件（genversion 会告警，只写 build 目录）
:: 注意：--format 必须整体加引号，否则 cmd 会把 --format=%%ct 拆成两个参数
git -C %THIRDPARTDIR%\luajit show -s "--format=%%ct" > %BUILDDIR%\luajit\luajit_relver.tmp 2>nul
fc /b %BUILDDIR%\luajit\luajit_relver.tmp %BUILDDIR%\luajit\luajit_relver.txt >nul 2>nul || (
    move /y %BUILDDIR%\luajit\luajit_relver.tmp %BUILDDIR%\luajit\luajit_relver.txt >nul || exit /b 1
    REM 版本号变了就删掉 luajit.h 让 genver 重生成：否则它始终比 relver.txt 旧，ninja 会永远判定要重跑
    if exist %BUILDDIR%\luajit\include\luajit.h del %BUILDDIR%\luajit\include\luajit.h
)
if exist %BUILDDIR%\luajit\luajit_relver.tmp del %BUILDDIR%\luajit\luajit_relver.tmp
if %ARCH% == 64 (
    set DASC=vm_x64.dasc
    set DASMF=-D P64
) else (
    set DASC=vm_x86.dasc
    set DASMF=
)
:: -D NO_UNWIND 必须与 LJ_CFLAGS 的 LJ_UNWIND 严格对齐（依据 luajit\src\Makefile 的
:: DASM_AFLAGS：LJ_NO_UNWIND 1 → -D NO_UNWIND）：vm_x86.dasc 的 saveregs_ 与帧偏移按它分叉，
:: 缺了它 lj_vm 与 C 侧对栈帧的理解就不一致。
if not "x%LJ_UNWIND%" == "x" set DASMF=%DASMF% -D NO_UNWIND
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build luajit\host\minilua.obj: ccx %THIRDPARTDIR::=$:%\luajit\src\host\minilua.c
>>%BUILDDIR%\build.ninja echo   flags = %HOST_CFLAGS%
>>%BUILDDIR%\build.ninja echo.
if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"%THIRDPARTDIR:\=\\%\\luajit\\src\\host\\minilua.c","command":"cl.exe %TARGET_ARG% !HOST_CFLAGS:\=\\! %THIRDPARTDIR:\=\\%\\luajit\\src\\host\\minilua.c"}>>%ROOT%compile_commands.json
>>%BUILDDIR%\build.ninja echo build luajit\host\minilua.exe: ldx luajit\host\minilua.obj
>>%BUILDDIR%\build.ninja echo   flags = /subsystem:console%SUBSYS_VER%
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build luajit\host\buildvm_arch.h: dasm %THIRDPARTDIR::=$:%\luajit\src\%DASC% ^| luajit\host\minilua.exe %THIRDPARTDIR::=$:%\luajit\dynasm\dynasm.lua
>>%BUILDDIR%\build.ninja echo   lua = luajit\host\minilua.exe
>>%BUILDDIR%\build.ninja echo   script = %THIRDPARTDIR::=$:%\luajit\dynasm\dynasm.lua
>>%BUILDDIR%\build.ninja echo   flags = %DASMF%
>>%BUILDDIR%\build.ninja echo.
for %%i in (buildvm buildvm_asm buildvm_fold buildvm_lib buildvm_peobj) do (
    echo build luajit\host\%%i.obj: ccx %THIRDPARTDIR::=$:%\luajit\src\host\%%i.c ^| luajit\host\buildvm_arch.h luajit\include\luajit.h>> %BUILDDIR%\build.ninja
    echo   flags = %HOST_CFLAGS%>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja
    if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"%THIRDPARTDIR:\=\\%\\luajit\\src\\host\\%%i.c","command":"cl.exe %TARGET_ARG% !HOST_CFLAGS:\=\\! %THIRDPARTDIR:\=\\%\\luajit\\src\\host\\%%i.c"}>>%ROOT%compile_commands.json
)
>>%BUILDDIR%\build.ninja echo build luajit\host\buildvm.exe: ldx luajit\host\buildvm.obj luajit\host\buildvm_asm.obj luajit\host\buildvm_fold.obj luajit\host\buildvm_lib.obj luajit\host\buildvm_peobj.obj
>>%BUILDDIR%\build.ninja echo   flags = /subsystem:console%SUBSYS_VER%
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build luajit\lj_vm.obj: genvm ^| luajit\host\buildvm.exe
>>%BUILDDIR%\build.ninja echo   vm = luajit\host\buildvm.exe
>>%BUILDDIR%\build.ninja echo   mode = peobj
>>%BUILDDIR%\build.ninja echo.
for %%m in (bcdef ffdef libdef recdef) do (
    echo build luajit\lj_%%m.h: genvm %LJ_LIBS% ^| luajit\host\buildvm.exe>> %BUILDDIR%\build.ninja
    echo   vm = luajit\host\buildvm.exe>> %BUILDDIR%\build.ninja
    echo   mode = %%m>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja
)
>>%BUILDDIR%\build.ninja echo build luajit\lj_folddef.h: genvm %THIRDPARTDIR::=$:%\luajit\src\lj_opt_fold.c ^| luajit\host\buildvm.exe
>>%BUILDDIR%\build.ninja echo   vm = luajit\host\buildvm.exe
>>%BUILDDIR%\build.ninja echo   mode = folddef
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build luajit\include\luajit.h: genver %THIRDPARTDIR::=$:%\luajit\src\luajit_rolling.h %BUILDDIR::=$:%\luajit\luajit_relver.txt ^| luajit\host\minilua.exe %THIRDPARTDIR::=$:%\luajit\src\host\genversion.lua
>>%BUILDDIR%\build.ninja echo   lua = luajit\host\minilua.exe
>>%BUILDDIR%\build.ninja echo   script = %THIRDPARTDIR::=$:%\luajit\src\host\genversion.lua
>>%BUILDDIR%\build.ninja echo.
for %%h in (lua.h luaconf.h lualib.h lauxlib.h lua.hpp) do (
    echo build luajit\include\%%h: cp %THIRDPARTDIR::=$:%\luajit\src\%%h>> %BUILDDIR%\build.ninja
    echo.>> %BUILDDIR%\build.ninja
)
set ODEPS=luajit\include\luajit.h luajit\lj_bcdef.h luajit\lj_ffdef.h luajit\lj_libdef.h luajit\lj_recdef.h luajit\lj_folddef.h
set OBJS=
for %%i in (%THIRDPARTDIR%\luajit\src\*.c) do (
    if not %%~ni == luajit (
        if not %%~ni == ljamalg (
            set SRC=%%i
            set OBJ=luajit\%%~ni.obj
            set OBJS=!OBJS! !OBJ!
            echo build !OBJ!: ccx !SRC::=$:! ^| !ODEPS!>> %BUILDDIR%\build.ninja
            echo   flags = %LJ_CFLAGS%>> %BUILDDIR%\build.ninja
            echo.>> %BUILDDIR%\build.ninja
            if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% !LJ_CFLAGS:\=\\! !SRC:\=\\!"}>>%ROOT%compile_commands.json
        )
    )
)
set OBJS=!OBJS! luajit\lj_vm.obj
call :build_ar luajit\luajit.lib
>>%BUILDDIR%\build.ninja echo build luajit\luajit.obj: ccx %THIRDPARTDIR::=$:%\luajit\src\luajit.c ^| !ODEPS!
>>%BUILDDIR%\build.ninja echo   flags = %LJ_CFLAGS%
>>%BUILDDIR%\build.ninja echo.
if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"%THIRDPARTDIR:\=\\%\\luajit\\src\\luajit.c","command":"cl.exe %TARGET_ARG% !LJ_CFLAGS:\=\\! %THIRDPARTDIR:\=\\%\\luajit\\src\\luajit.c"}>>%ROOT%compile_commands.json

if "x%PATCHVER%" == "x1" (
    >>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\luajit_%ARCH%.exe: ld luajit\luajit.obj src\sys\main.obj %LINK_DEPS% luajit\luajit.lib src\sys\msvcrt.lib src\sys\strtoull.obj src\sys\destruct_eh.obj ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
    >>%BUILDDIR%\build.ninja echo.
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\luajit_%ARCH%.exe: pever %BUILDDIR::=$:%\pever\luajit_%ARCH%.exe ^| %BUILDDIR::=$:%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo.
) else (
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\luajit_%ARCH%.exe: ld luajit\luajit.obj src\sys\main.obj %LINK_DEPS% luajit\luajit.lib src\sys\msvcrt.lib src\sys\strtoull.obj src\sys\destruct_eh.obj ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
    >>%BUILDDIR%\build.ninja echo.
)

:: 两个变体都产 DLL；legacy 只有 32 位需要（x64 全是 NT5+，§2.1）
call :buildautodbg lua || exit /b 1
call :buildautodbg luajit || exit /b 1
if "%ARCH%" == "86" (
    call :buildautodbg lua legacy || exit /b 1
    call :buildautodbg luajit legacy || exit /b 1
)
:: 交付物只有一个 exe：启动器 + 载荷（64 装 2 个模块，86 装 4 个模块，D14 单流）
call :buildlauncher %ARCH% || exit /b 1
if "x%ARCH%" == "x86" call :buildfixture || exit /b 1

:: default 只列交付物 + 自举 host；旧的 AutoDbg_<flavor>_<arch>.exe 已退役（D8）
set DEFS=
set DEFS=%DEFS% %INSTALLROOT::=$:%\lua_%ARCH%.exe
set DEFS=%DEFS% %INSTALLROOT::=$:%\luajit_%ARCH%.exe
set DEFS=%DEFS% %INSTALLROOT::=$:%\AutoDbg_%ARCH%.exe
if "x%ARCH%" == "x86" set DEFS=%DEFS% %INSTALLROOT::=$:%\fixtures\x86_loop.exe
if "x%ARCH%" == "x86" set DEFS=%DEFS% %INSTALLROOT::=$:%\fixtures\x86_spawn.exe
>>%BUILDDIR%\build.ninja echo default %DEFS%
>>%BUILDDIR%\build.ninja echo.

if "x%GEN_COMPILE%" == "x1" echo ]>>%ROOT%compile_commands.json
:: 版本宏：每次都算一遍（内容没变就不碰文件）
if "x%GEN_BUILD%" == "x1" (
    call :genversion || exit /b 1
    ninja -C %BUILDDIR%
)

exit /b

:: ---------------------------------------------------------------------------
:: :genversion —— 生成 <build>\gen\autodbg_build.h（版本宏，见 src/sys/version.h）
:: 用法: call :genversion
:: defines: BUILDDIR（:build 里已指向 %BUILDROOT%\%arch%\%mode%）
:: 版本号 = git describe --tags --always --dirty：
::   HEAD 正好在 tag 上 -> v0.1.0；tag 之后 N 个提交 -> v0.1.0-3-g1fa399c；
::   还没有任何 tag（当前情况）-> 1fa399c；工作区有改动 -> 追加 -dirty
:: 生成物只写 ASCII（cmd 的代码页不可控，中文注释进了 /utf-8 编译单元会报警告）。
:: 内容没变就不碰文件，否则每次构建都会触发包含它的 TU 重编。
:genversion
setlocal enableextensions enabledelayedexpansion
set "VER="
set "VSRC=unknown"
for /f "delims=" %%v in ('git -C "%ROOT%." describe --tags --always --dirty 2^>nul') do set "VER=%%v"
if not "x!VER!" == "x" (
    set "VSRC=commit"
    for /f "delims=" %%v in ('git -C "%ROOT%." describe --tags --abbrev=0 2^>nul') do set "VSRC=describe"
    for /f "delims=" %%v in ('git -C "%ROOT%." describe --tags --exact-match 2^>nul') do set "VSRC=tag"
) else (
    set "VER=unknown"
)
set "VDIRTY=0"
if not "!VER:-dirty=!" == "!VER!" set "VDIRTY=1"
set "VHDR=%BUILDDIR%\gen\autodbg_build.h"
> "!VHDR!.tmp" echo /* generated by make.bat :genversion (git describe) - do not edit */
>> "!VHDR!.tmp" echo #ifndef AUTODBG_GEN_BUILD_H
>> "!VHDR!.tmp" echo #define AUTODBG_GEN_BUILD_H
>> "!VHDR!.tmp" echo #define AUTODBG_VERSION "!VER!"
>> "!VHDR!.tmp" echo #define AUTODBG_VERSION_SOURCE "!VSRC!"
>> "!VHDR!.tmp" echo #define AUTODBG_VERSION_DIRTY !VDIRTY!
>> "!VHDR!.tmp" echo #endif
fc /b "!VHDR!" "!VHDR!.tmp" >nul 2>&1
if errorlevel 1 (
    move /y "!VHDR!.tmp" "!VHDR!" >nul
) else (
    del /q "!VHDR!.tmp" >nul 2>&1
)
endlocal & exit /b 0

:: buildautodbg <lua^|luajit>
:buildautodbg
setlocal
:: 生成datas（每个变体一个目录，否则两个变体会生成同一个 datas\datas.h）
set VARIANT=%2
if "x!VARIANT!" == "x" set VARIANT=modern
:: legacy = ANSI + NO_SCYLLA_HIDE（原 98 架构的编译口径，D1/D18）
set LEGACY_BUILD=
if "!VARIANT!" == "legacy" set LEGACY_BUILD=1
set FLAVORDIR=%1_!VARIANT!
set DLLNAME=AutoDbg_%1_%ARCH%.dll
if "x!LEGACY_BUILD!" == "x1" set DLLNAME=AutoDbgLegacy_%1_%ARCH%.dll
for %%m in (common,core,scripting\lua) do if not exist %BUILDDIR%\!FLAVORDIR!\src\%%m mkdir %BUILDDIR%\!FLAVORDIR!\src\%%m
set DATASDIR=datas_%1
if not exist %BUILDDIR%\!DATASDIR! mkdir %BUILDDIR%\!DATASDIR!
:: datas 是 flavor 级共享产物：只有 modern（每个架构的主构建）负责生成，legacy 复用它
if "!VARIANT!" == "modern" (
    set LUAFILES=
    for %%i in (%ROOT%lualib\*.lua) do (
        set FN=%%i
        set LUAFILES=!LUAFILES! !FN::=$:!
    )
    >>%BUILDDIR%\build.ninja echo build !DATASDIR!\datas_%1.c ^| !DATASDIR!\datas.h: gendata !LUAFILES! ^| %INSTALLROOT::=$:%\%1_%ARCH%.exe %ROOT::=$:%tools\gen_lua_lib.lua
    >>%BUILDDIR%\build.ninja echo   lua = %INSTALLROOT%\%1_%ARCH%.exe
    >>%BUILDDIR%\build.ninja echo.
    >>%BUILDDIR%\build.ninja echo build !DATASDIR!\datas_%1.obj: cc !DATASDIR!\datas_%1.c
    >>%BUILDDIR%\build.ninja echo.
    if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!DATASDIR!\\datas_%1.c","command":"cl.exe %TARGET_ARG% !CFLAGS_JSON! datas\\datas_%1.c"}>>%ROOT%compile_commands.json
)

:: 生成autodbg lua版
set AD_FLAGS=/I%BUILDDIR%\gen /I%SRCDIR% /I%SRCDIR%\common /I%SRCDIR%\core /I%SRCDIR%\scripting\lua /I!DATASDIR! /I%1\include
set AD_FLAGS=!AD_FLAGS! /D_CRT_NO_TIME_T /DWIN32_LEAN_AND_MEAN
set RULECC=cc
set RULECXX=cxx
if "x!LEGACY_BUILD!" == "x1" (
    set RULECC=ccv9x
    set RULECXX=cxxv9x
)
if "x!LEGACY_BUILD!" == "x1" (
    set AD_FLAGS=!AD_FLAGS! /DNO_SCYLLA_HIDE
)
set SYSLIBS=shell32.lib comdlg32.lib kernel32.lib user32.lib gdi32.lib shlwapi.lib ws2_32.lib advapi32.lib
set ODEPS=%1\include\lua.h %1\include\luaconf.h %1\include\lualib.h %1\include\lauxlib.h %1\include\lua.hpp
set OBJS=
if %1 == luajit (
    REM win9x_shim.obj 由下面 common\*.c 的循环编出来（此处不再重复加入，否则 LNK4042）
    REM LuaJIT 的 tonumber(s,base) 用到 strtoull：零 CRT 下由 src\sys\strtoull.c 提供
    set OBJS=!OBJS! src\sys\strtoull.obj src\sys\destruct_eh.obj
    set AD_FLAGS=!AD_FLAGS! /FIlj.h /DAUTODBG_WIN9X /DJIT %LJ_UNWIND%
)
if "x!LEGACY_BUILD!" == "x1" (
    set LINK_DEPS=!LINK_DEPS! src\sys\ntdll.lib
) else (
    set AD_FLAGS=!AD_FLAGS! /I%SRCDIR%\ScyllaHide\ScyllaHide
    set LINK_DEPS=!LINK_DEPS! src\ScyllaHide\ScyllaHide.lib src\ScyllaHide\Scylla.lib src\ScyllaHide\distorm.lib
    if %ARCH% == 86 (
        set LINK_DEPS=!LINK_DEPS! %SRCDIR::=$:%\ScyllaHide\3rdparty\ntdll\ntdll_x86.lib
        set AD_FLAGS=!AD_FLAGS! /DPSAPI_VERSION=1
        set SYSLIBS=!SYSLIBS! psapi.lib
    ) else (
        set LINK_DEPS=!LINK_DEPS! %SRCDIR::=$:%\ScyllaHide\3rdparty\ntdll\ntdll_x64.lib
    )
)


for %%i in (common,core,scripting\lua) do (
    for %%j in (%SRCDIR%\%%i\*.c,%SRCDIR%\%%i\*.cpp) do (
        set SRC=%%j
        set OBJ=!FLAVORDIR!\src\%%i\%%~nj.obj
        set OBJS=!OBJS! !OBJ!
        if %%j == %SRCDIR%\scripting\lua\state.cpp set ODEPS=!ODEPS! !DATASDIR!\datas.h
        if %%~xj == .cpp (
            echo build !OBJ!: !RULECXX! !SRC::=$:! ^| !ODEPS!>> %BUILDDIR%\build.ninja
            if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% !CXXFLAGS_JSON! !AD_FLAGS:\=\\! !SRC:\=\\!"}>>%ROOT%compile_commands.json
        ) else (
            echo build !OBJ!: !RULECC! !SRC::=$:! ^| !ODEPS!>> %BUILDDIR%\build.ninja
            if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% !CFLAGS_JSON! !AD_FLAGS:\=\\! !SRC:\=\\!"}>>%ROOT%compile_commands.json
        )
        
        echo   flags = %AD_FLAGS% >> %BUILDDIR%\build.ninja
        echo.>> %BUILDDIR%\build.ninja
    )
)

>>%BUILDDIR%\build.ninja echo.
:: 产品模块永远是 DLL（交付物只有 AutoDbg_<arch>.exe，D8）：
:: x86 先链到 pever\ 再打补丁到 .out（D12：子系统版本 4.0）；x64 保持 6.0，直接落 .out
if "x%PATCHVER%" == "x1" (
    >>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\!DLLNAME!: ldx !OBJS! src\sys\msvcrt.lib !LINK_DEPS! %1\%1.lib !DATASDIR!\datas_%1.obj ^| t
    >>%BUILDDIR%\build.ninja echo   flags = /dll /noentry /nodefaultlib /noimplib /noexp /incremental:no /manifest:no /debug:none /novcfeature /nocoffgrpinfo /subsystem:windows%SUBSYS_VER%
    >>%BUILDDIR%\build.ninja echo   syslib = %SYSLIBS%
    >>%BUILDDIR%\build.ninja echo.
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\!DLLNAME!: pever %BUILDDIR::=$:%\pever\!DLLNAME! ^| %BUILDDIR::=$:%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo.
) else (
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\!DLLNAME!: ldx !OBJS! src\sys\msvcrt.lib !LINK_DEPS! %1\%1.lib !DATASDIR!\datas_%1.obj ^| t
    >>%BUILDDIR%\build.ninja echo   flags = /dll /noentry /nodefaultlib /noimplib /noexp /incremental:no /manifest:no /debug:none /novcfeature /nocoffgrpinfo /subsystem:windows%SUBSYS_VER%
    >>%BUILDDIR%\build.ninja echo   syslib = %SYSLIBS%
    >>%BUILDDIR%\build.ninja echo.
)

exit /b

:: buildlauncher <arch>
::   产出 %INSTALLROOT%\AutoDbg_<arch>.exe：零 CRT 启动器 + 打包模块
::     64 → 2 个模块（modern × {lua, luajit}）
::     86 → 4 个模块（modern + legacy × {lua, luajit}）
::   defines: ARCH, BUILDDIR, LINK_DEPS, PATCHVER
:buildlauncher
setlocal
set LOBJ=launcher
set PACKMOD=%INSTALLROOT::=$:%\AutoDbg_lua_%ARCH%.dll %INSTALLROOT::=$:%\AutoDbg_luajit_%ARCH%.dll
if "%ARCH%" == "86" set PACKMOD=!PACKMOD! %INSTALLROOT::=$:%\AutoDbgLegacy_lua_%ARCH%.dll %INSTALLROOT::=$:%\AutoDbgLegacy_luajit_%ARCH%.dll
>>%BUILDDIR%\build.ninja echo build pack\packdata.c: pack !PACKMOD! ^| %BUILDDIR::=$:%\tools\pack.exe !PACK_BLOB_ESC!
>>%BUILDDIR%\build.ninja echo   packtool = %BUILDDIR%\tools\pack.exe
>>%BUILDDIR%\build.ninja echo   flags = !PACK_FLAGS!
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build pack\packdata.obj: cc pack\packdata.c
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build !LOBJ!\pack_entry.obj: cc %SRCDIR::=$:%\sys\pack\pack_entry.c
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build !LOBJ!\launcher.obj: cxx %SRCDIR::=$:%\sys\pack\launcher.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build !LOBJ!\payload.obj: cxx %SRCDIR::=$:%\sys\pack\payload.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build !LOBJ!\module_loader.obj: cxx %SRCDIR::=$:%\sys\pack\module_loader.cpp
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build !LOBJ!\cmdline.obj: cc %SRCDIR::=$:%\common\cmdline.c
>>%BUILDDIR%\build.ninja echo.
set LOBJS=!LOBJ!\pack_entry.obj !LOBJ!\launcher.obj !LOBJ!\payload.obj !LOBJ!\module_loader.obj !LOBJ!\cmdline.obj pack\packdata.obj src\common\allocator.obj src\sys\lzma\LzmaDec.obj src\sys\lzma\Alloc.obj src\sys\msvcrt.lib %LINK_DEPS%
:: x86 的启动器也要 4.0 子系统版本（D12）；x64 保持 6.0
if "x%PATCHVER%" == "x1" (
    >>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\AutoDbg_%ARCH%.exe: ld !LOBJS! ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib user32.lib comdlg32.lib
    >>%BUILDDIR%\build.ninja echo.
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\AutoDbg_%ARCH%.exe: pever %BUILDDIR::=$:%\pever\AutoDbg_%ARCH%.exe ^| %BUILDDIR::=$:%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
    >>%BUILDDIR%\build.ninja echo.
) else (
    >>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\AutoDbg_%ARCH%.exe: ld !LOBJS! ^| t
    >>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib user32.lib comdlg32.lib
    >>%BUILDDIR%\build.ninja echo.
)
exit /b

:: call :build_ar outlib
:: defines: OBJS
:build_ar
if not exist %~dp1 mkdir %~dp1
>>%BUILDDIR%\build.ninja echo build %1: ar !OBJS!
>>%BUILDDIR%\build.ninja echo.
exit /b

:: call :build_ccs srcroot buildroot
:: defines: TP_CFLAGS
:: defines: EXCLUDE
:: defines: OBJS
:: defines: ODEPS
:build_ccs
if not exist %2 mkdir %2
REM @echo on
for %%i in (%1) do (
    (echo "|%EXCLUDE%|" | findstr /i "|%%~nxi|") >nul || (
        set SRC=%%i
        set OBJ=%2\%%~ni.obj
        set OBJS=!OBJS! !OBJ!
        if "x%ODEPS%" == "x" (
            echo build !OBJ!: cc !SRC::=$:!>> %BUILDDIR%\build.ninja
        ) else (
            echo build !OBJ!: cc !SRC::=$:! ^| %ODEPS%>> %BUILDDIR%\build.ninja
        )
        echo   flags = !TP_CFLAGS!>> %BUILDDIR%\build.ninja
        echo.>> %BUILDDIR%\build.ninja
        if "x%GEN_COMPILE%" == "x1" echo ,{"directory":"%BUILDDIR:\=\\%","file":"!SRC:\=\\!","command":"cl.exe %TARGET_ARG% %CFLAGS_JSON% !TP_CFLAGS:\=\\! !SRC:\=\\!"}>>%ROOT%compile_commands.json
    )
)
REM @echo off
exit /b

:loadvcvars
for /F "delims=" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property InstallationPath') do (
    call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %1 && exit /b
)
exit /b 1

:: buildfixture
::   usage: call :buildfixture   (only called for arch 86, T7.2)
::   defines: ARCH, ROOT, BUILDDIR, INSTALLROOT, LINK_DEPS
::   x86 debug-target fixtures from tests\fixtures\bin (plan_t7.md T7.2 / T7 slice C2b).
::     Not deliverables: they go into DEFS only (10.8), so :usage text is not rewritten.
::       x86_loop.c  -> single process, fixed int3 site pattern (pump.* gate)
::       x86_spawn.c -> process tree: spawns itself as a child, one tag value per
::                      process (route.* / d:proc / p:v gate, C2b)
::     Import table must pass the w95 flavour (scan_dir.py picks w95 under .out/fixtures/)
::     => no NT-only call in the source. x86 product => patched to 4.0 by pever (D12).
:buildfixture
if not "x%ARCH%" == "x86" exit /b 0

>>%BUILDDIR%\build.ninja echo build tests\fixtures\bin\x86_loop.obj: cc %ROOT::=$:%tests\fixtures\bin\x86_loop.c
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\x86_loop.exe: ld tests\fixtures\bin\x86_loop.obj src\sys\main.obj src\sys\msvcrt.lib %LINK_DEPS% ^| t
>>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\fixtures\x86_loop.exe: pever %BUILDDIR::=$:%\pever\x86_loop.exe ^| %BUILDDIR::=$:%\tools\pever.exe
>>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
>>%BUILDDIR%\build.ninja echo.

>>%BUILDDIR%\build.ninja echo build tests\fixtures\bin\x86_spawn.obj: cc %ROOT::=$:%tests\fixtures\bin\x86_spawn.c
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %BUILDDIR::=$:%\pever\x86_spawn.exe: ld tests\fixtures\bin\x86_spawn.obj src\sys\main.obj src\sys\msvcrt.lib %LINK_DEPS% ^| t
>>%BUILDDIR%\build.ninja echo   syslib = kernel32.lib
>>%BUILDDIR%\build.ninja echo.
>>%BUILDDIR%\build.ninja echo build %INSTALLROOT::=$:%\fixtures\x86_spawn.exe: pever %BUILDDIR::=$:%\pever\x86_spawn.exe ^| %BUILDDIR::=$:%\tools\pever.exe
>>%BUILDDIR%\build.ninja echo   pevertool = %BUILDDIR%\tools\pever.exe
>>%BUILDDIR%\build.ninja echo.
exit /b

:usage
echo %0 [option]
echo option:
echo     c: 清除构建目录
echo     r: 重新构建
echo     b ^<buildpath^>: 指定构建目录
echo     i ^<installpath^>: 指定输出目录
echo     d: debug模式，默认release模式
echo     a ^<arch^>: 只生成该架构的 compile_commands.json ^(不构建^)
echo 说明: arch 98 已退役 —— 老系统口径改由运行时 --variant=legacy 选择
echo 产物: AutoDbg_64.exe / AutoDbg_86.exe ^(单文件，内含 flavor x variant 全部模块^)
exit /b

:err
echo error
exit /b 1
