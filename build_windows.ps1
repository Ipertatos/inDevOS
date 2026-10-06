<#
.SYNOPSIS
    Native Windows build/boot script for inDevOS.

.DESCRIPTION
    Builds the kernel (kernel_final.bin), the bootable image (os.iso) and can
    run a headless boot smoke test -- all on plain Windows, with no MSYS2, WSL
    or Cygwin required.

    Why clang + ld.lld instead of the MinGW toolchain:
      * MinGW-w64's `ld` only implements the i386pe/i386pep (PE/COFF)
        emulations, so it cannot link the ELF64 kernel this project produces
        (`-melf_x86_64`) -- the link fails with "unsupported emulation".
      * MinGW's `gcc` emits COFF objects, not the ELF64 objects linker.ld and
        the Limine protocol expect.
      clang can emit ELF64 via --target=x86_64-unknown-none-elf and LLVM's
      ld.lld links it, so this script uses those.  The Linux/CI build keeps
      using the Makefile (gcc + ld + make).

.PARAMETER Clean
    Delete all build artifacts before building.

.PARAMETER EnableLua
    Compile the vendored Lua 5.4 runtime into the kernel (default: true).

.PARAMETER Target
    all (default) | kernel | iso | boot | clean | install-iso

.PARAMETER BootTest
    After building, boot os.iso headlessly in QEMU and dump the serial log.
    Boot output is written to serial-win.log.

.PARAMETER BootTimeoutSec
    How long the headless boot test is allowed to run (default: 60).

.EXAMPLE
    .\build_windows.ps1 -Clean
.EXAMPLE
    .\build_windows.ps1 -Target iso
.EXAMPLE
    .\build_windows.ps1 -BootTest -BootTimeoutSec 90
#>

[CmdletBinding()]
param(
    [switch]$Clean,
    [bool]$EnableLua = $true,
    [ValidateSet("all", "kernel", "iso", "boot", "clean", "install-iso")]
    [string]$Target = "all",
    [switch]$BootTest,
    [int]$BootTimeoutSec = 60,
    [switch]$KeepGoing,
    [switch]$Help,
    # Optional explicit tool paths (auto-detected when omitted)
    [string]$Clang,
    [string]$Lld,
    [string]$Nasm,
    [string]$Xorriso,
    [string]$Qemu,
    [string]$Limine,
    [string]$Python
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
if (-not $Root) { $Root = (Get-Location).Path }
Set-Location $Root

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------
if ($Help) {
    Get-Help $PSCommandPath -Detailed
    exit 0
}

function Write-Step($m) { Write-Host "[INFO]  $m" -ForegroundColor Green }
function Write-Warn2($m) { Write-Host "[WARN]  $m" -ForegroundColor Yellow }
function Write-Err($m) { Write-Host "[ERROR] $m" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------
function Find-Tool {
    param([string[]]$Candidates, [string]$Friendly, [string]$Hint, [switch]$Optional)
    foreach ($c in $Candidates) {
        if (-not $c) { continue }
        if (Test-Path $c) { return (Resolve-Path $c).Path }
        $cmd = Get-Command $c -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    if ($Optional) { Write-Warn2 "$Friendly not found - $Hint"; return $null }
    throw "$Friendly not found. $Hint"
}

Write-Step "Resolving toolchain..."
$Clang = Find-Tool @($Clang, "clang") "clang compiler" `
    "Install LLVM/clang (winget install LLVM.LLVM) or pass -Clang <path>."
$Lld = Find-Tool @($Lld, "ld.lld") "ld.lld linker" `
    "Install LLVM (ships ld.lld) or pass -Lld <path>."
$Nasm = Find-Tool @($Nasm, "nasm") "nasm assembler" `
    "Install NASM (winget install NASM.NASM) or pass -Nasm <path>."
$Xorriso = Find-Tool @($Xorriso, "xorriso") "xorriso" `
    "Needed for 'iso'/'all' targets; pass -Xorriso <path>." -Optional
$Qemu = Find-Tool @($Qemu, "qemu-system-x86_64") "QEMU" `
    "Needed for -BootTest; pass -Qemu <path>." -Optional
$Limine = Find-Tool @($Limine, (Join-Path $Root "limine\limine.exe")) "limine.exe" `
    "Expected at limine\limine.exe (run: git submodule update --init limine)." -Optional

# Python is only needed to generate the kernel symbol table.
$PythonExe = $null
$PythonArgs = @()
foreach ($cand in @($Python, "py", "python", "python3")) {
    if (-not $cand) { continue }
    $cmd = Get-Command $cand -ErrorAction SilentlyContinue
    if ($cmd) { $PythonExe = $cmd.Source; break }
}
if ($PythonExe -and (Split-Path $PythonExe -Leaf) -match '^py(\.exe)?$') { $PythonArgs = @("-3") }

# llvm-objdump lives next to ld.lld and is the only ELF-capable symbol dumper
# available on Windows (MinGW's nm cannot read ELF).
$Objdump = Join-Path (Split-Path $Lld -Parent) "llvm-objdump.exe"
if (-not (Test-Path $Objdump)) {
    $Objdump = Find-Tool @("llvm-objdump") "llvm-objdump" `
        "Install LLVM (ships llvm-objdump) or pass -Lld pointing at an LLVM bin dir." -Optional
}

Write-Host ("  clang    : {0}" -f $Clang)
Write-Host ("  ld.lld   : {0}" -f $Lld)
Write-Host ("  nasm     : {0}" -f $Nasm)
Write-Host ("  xorriso  : {0}" -f $(if ($Xorriso) { $Xorriso } else { "(missing)" }))
Write-Host ("  qemu     : {0}" -f $(if ($Qemu) { $Qemu } else { "(missing)" }))
Write-Host ("  objdump  : {0}" -f $(if ($Objdump) { $Objdump } else { "(missing)" }))
Write-Host ("  python   : {0} {1}" -f $(if ($PythonExe) { $PythonExe } else { "(missing)" }), ($PythonArgs -join " "))

# ---------------------------------------------------------------------------
# Compile/link flags -- kept in sync with the Makefile's BASE_CFLAGS.
# The one addition is --target=x86_64-unknown-none-elf, needed because clang on
# Windows defaults to a PE/COFF target.
# ---------------------------------------------------------------------------
$TargetTriple = "--target=x86_64-unknown-none-elf"

$BaseFlags = @(
    $TargetTriple
    "-g"
    "-O2"                                   # Makefile parity: -O0 (old script) is a big perf loss
    "-fno-strict-aliasing"
    "-fno-omit-frame-pointer"
    "-fno-stack-protector"
    "-U_FORTIFY_SOURCE"
    "-D_FORTIFY_SOURCE=0"
    "-m64"
    "-ffreestanding"
    "-fno-pie"
    "-mno-red-zone"                         # required: no red zone across interrupts
    "-mgeneral-regs-only"                   # no SSE before fpu_sse_init()
    "-mcmodel=kernel"
    "-fcommon"
    "-Wall"
    "-Wextra"
    "-Wno-unused-parameter"
    "-Wno-unused-function"
    "-Wno-unused-variable"
    "-Wno-sign-compare"
    "-Werror=implicit-function-declaration"
    "-Werror=return-type"
    "-Ilimine"
    "-Isrc/libc"                            # freestanding C library headers (no host libc)
)
$LuaFlags = @("-DENABLE_LUA", "-Ithird_party/lua-5.4.7/src")
$LdFlags = @("-T", "linker.ld", "-melf_x86_64", "-z", "noexecstack")
$AsFlags = @("-g", "-F", "dwarf", "-f", "elf64")

# Lua 5.4 core + selected stdlibs (mirrors the Makefile's LUA_CORE_SRC list).
$LuaFiles = @(
    "lapi.c", "lcode.c", "lctype.c", "ldebug.c", "ldo.c", "ldump.c",
    "lfunc.c", "lgc.c", "llex.c", "lmem.c", "lobject.c", "lopcodes.c",
    "lparser.c", "lstate.c", "lstring.c", "ltable.c", "ltm.c", "lundump.c",
    "lvm.c", "lzio.c", "lauxlib.c", "lstrlib.c", "ltablib.c", "lmathlib.c"
)
# Resolve the vendored Lua tree by looking for the sources instead of trusting
# the unversioned "lua-5.4" name (on some checkouts that is a junction/symlink
# to the real lua-5.4.x directory, and NTFS junctions can be unusable).
$LuaSrcDir = $null
foreach ($cand in @(Get-ChildItem -Path (Join-Path $Root "third_party") -Directory -Filter "lua-5.4*" -ErrorAction SilentlyContinue | Sort-Object Name)) {
    if (Test-Path (Join-Path $cand.FullName "src\lapi.c")) {
        $LuaSrcDir = "third_party/$($cand.Name)/src"
        break
    }
}
if (-not $LuaSrcDir) { $LuaSrcDir = "third_party/lua-5.4/src" }

# Files that genuinely need the SSE/double ABI: drop -mgeneral-regs-only and
# allow SSE2, exactly like the Makefile's per-object overrides.
function Get-FlagsForSource {
    param([string]$RelPath)
    $leaf = Split-Path $RelPath -Leaf
    $needsSse = ($leaf -eq "term.c") -or ($leaf -eq "libc_shim.c") -or
                ($leaf -like "sdl_*.c") -or ($RelPath -like "third_party*")
    $flags = New-Object System.Collections.Generic.List[string]
    $flags.AddRange([string[]]$BaseFlags)
    if ($EnableLua) { $flags.AddRange([string[]]$LuaFlags) }
    if ($needsSse) {
        $flags.Remove("-mgeneral-regs-only") | Out-Null
        $flags.Add("-msse2")
    }
    return $flags.ToArray()
}

function Get-ObjectPath {
    param([string]$RelPath)
    $p = $RelPath -replace '\\', '/'
    if ($p -like 'third_party/*') {
        return ("obj/lua/" + [System.IO.Path]::GetFileNameWithoutExtension($p) + ".o")
    }
    return ("obj/" + ($p -replace '\.(c|asm)$', '.o'))
}

# ---------------------------------------------------------------------------
# Native invocation helper.
# Runs the tool through cmd.exe so output redirection stays inside cmd (this
# also keeps very long link command lines off PowerShell's parser).
# ---------------------------------------------------------------------------
$BuildLog = Join-Path $Root "obj\.build.log"

function Invoke-Native {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [string]$LogFile = $BuildLog,
        [switch]$NoAppend
    )
    $quoted = foreach ($a in $Arguments) {
        if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
    }
    $line = '"' + $Exe + '" ' + ($quoted -join ' ')
    $redir = if ($NoAppend) { '>' } else { '>>' }
    $full = "$line $redir `"$LogFile`" 2>&1"
    # NOTE: never pipe, redirect or capture the output of a native command from
    # PowerShell here -- stdout/stderr are redirected to $LogFile *inside* cmd,
    # and PowerShell-level redirection of native programs is restricted in some
    # sandboxes ("StandardOutputEncoding is only supported when standard output
    # is redirected").  The exit code is published via $script:NativeExit
    # instead of a return value, because assigning a function's output would
    # capture this native call's stdout and hit the same restriction.
    & cmd.exe /d /c $full
    $script:NativeExit = $LASTEXITCODE
}

function Get-LogTail {
    param([int]$Lines = 40)
    if (Test-Path $BuildLog) { Get-Content $BuildLog -Tail $Lines }
}

# ---------------------------------------------------------------------------
# Toolchain sanity check: the compiler must emit ELF64 and the linker must be
# able to link it.  Failing here with a clear message beats a confusing wall of
# "file format not recognized" errors later.
# ---------------------------------------------------------------------------
function Test-Toolchain {
    Write-Step "Verifying toolchain can produce ELF64..."
    $probeC = Join-Path $Root "obj\.probe.c"
    $probeO = Join-Path $Root "obj\.probe.o"
    Set-Content -Path $probeC -Value 'int probe(void){return 42;}' -Encoding ASCII
    if (-not (Test-Path (Join-Path $Root "obj"))) { New-Item -ItemType Directory -Path (Join-Path $Root "obj") | Out-Null }

    Invoke-Native $Clang (@($TargetTriple, "-ffreestanding", "-c", "-o", $probeO, $probeC)) $BuildLog -NoAppend
    if ($script:NativeExit -ne 0 -or -not (Test-Path $probeO)) {
        Write-Err "clang could not compile a probe file:"
        Get-LogTail 20 | ForEach-Object { Write-Host "    $_" }
        throw "clang toolchain check failed"
    }
    $magic = [System.IO.File]::ReadAllBytes($probeO)[0..3]
    if (-not ($magic[0] -eq 0x7f -and $magic[1] -eq 0x45 -and $magic[2] -eq 0x4c -and $magic[3] -eq 0x46)) {
        throw "clang produced a non-ELF object (missing --target ELF support?). Wrong clang: $Clang"
    }
    $probeBin = Join-Path $Root "obj\.probe.bin"
    Invoke-Native $Lld (@("-melf_x86_64", "-o", $probeBin, $probeO)) $BuildLog
    if ($script:NativeExit -ne 0) {
        Write-Err "ld.lld could not link an ELF64 object - it may be a PE-only linker:"
        Get-LogTail 20 | ForEach-Object { Write-Host "    $_" }
        throw "linker toolchain check failed ($Lld)"
    }
    Remove-Item $probeC, $probeO, $probeBin -ErrorAction SilentlyContinue
    Write-Step "Toolchain OK (ELF64 capable)."
}

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------
function Invoke-Clean {
    Write-Step "Cleaning build artifacts..."
    foreach ($p in @("obj", "iso", "boot.bin", "kernel_final.bin", "os.iso", "os_full.iso",
                     "kernel.sym", "src\kernel_symtab.c", "serial-win.log")) {
        $full = Join-Path $Root $p
        if (Test-Path $full) {
            Remove-Item $full -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "  removed $p"
        }
    }
}

# ---------------------------------------------------------------------------
# Source discovery
# ---------------------------------------------------------------------------
function Get-BuildInputs {
    $cSources = Get-ChildItem -Path (Join-Path $Root "src") -Recurse -Filter *.c |
        Where-Object { $_.Name -ne "kernel_symtab.c" } |
        ForEach-Object { $_.FullName.Substring($Root.Length + 1) }

    $asmSources = Get-ChildItem -Path (Join-Path $Root "src") -Recurse -Filter *.asm |
        ForEach-Object { $_.FullName.Substring($Root.Length + 1) }

    $luaSources = @()
    if ($EnableLua) {
        foreach ($f in $LuaFiles) {
            $rel = "$LuaSrcDir/$f"
            if (-not (Test-Path (Join-Path $Root ($rel -replace '/', '\')))) {
                throw "Missing Lua source $rel. Re-vendor Lua 5.4 (scripts/fetch_lua.sh) or build with -EnableLua:`$false."
            }
            $luaSources += $rel
        }
    }
    return @{ C = @($cSources); Asm = @($asmSources); Lua = @($luaSources) }
}

# ---------------------------------------------------------------------------
# Compile
# ---------------------------------------------------------------------------
function Build-Objects {
    param([switch]$Force, [switch]$KeepGoing)

    $inputs = Get-BuildInputs
    $all = @($inputs.C) + @($inputs.Lua) + @($inputs.Asm)
    Write-Step ("Compiling {0} C, {1} Lua and {2} asm sources..." -f $inputs.C.Count, $inputs.Lua.Count, $inputs.Asm.Count)

    # Coarse but safe header-dependency check: if any header changed after an
    # object was built, rebuild that object.
    $newestHeader = Get-ChildItem -Path (Join-Path $Root "src") -Recurse -Include *.h -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1

    $objects = New-Object System.Collections.Generic.List[string]
    $compiled = 0; $skipped = 0; $failed = @()

    foreach ($rel in $all) {
        $srcFull = Join-Path $Root ($rel -replace '/', '\')
        $objRel = Get-ObjectPath $rel
        $objFull = Join-Path $Root ($objRel -replace '/', '\')
        $objects.Add($objRel) | Out-Null

        $objDir = Split-Path $objFull -Parent
        if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir -Force | Out-Null }

        if (-not $Force -and (Test-Path $objFull)) {
            $objTime = (Get-Item $objFull).LastWriteTime
            $srcTime = (Get-Item $srcFull).LastWriteTime
            if ($objTime -gt $srcTime -and (-not $newestHeader -or $objTime -gt $newestHeader.LastWriteTime)) {
                $skipped++
                continue
            }
        }

        $isAsm = $rel -like "*.asm"
        if ($isAsm) {
            Invoke-Native $Nasm ($AsFlags + @("-o", $objFull, $srcFull))
        } else {
            $flags = Get-FlagsForSource $rel
            Invoke-Native $Clang (@($flags) + @("-c", "-o", $objFull, $srcFull))
        }

        if ($script:NativeExit -ne 0 -or -not (Test-Path $objFull)) {
            $failed += $rel
            Write-Err "Failed to build $rel"
            if (-not $KeepGoing) {
                Write-Host "--- last 30 lines of build log ---"
                Get-LogTail 30 | ForEach-Object { Write-Host "    $_" }
                exit 1
            }
        }
        $compiled++
    }

    if ($failed.Count -gt 0) {
        Write-Host ""
        Write-Err ("{0} source file(s) failed to compile:" -f $failed.Count)
        $failed | Sort-Object | ForEach-Object { Write-Host "    $_" }
        Write-Host ""
        Write-Host "--- compiler errors ---"
        if (Test-Path $BuildLog) {
            Select-String -Path $BuildLog -Pattern ': error:' |
                Select-Object -First 120 |
                ForEach-Object { Write-Host ("    " + $_.Line.Trim()) }
        }
        exit 1
    }

    Write-Step ("Objects: {0} compiled, {1} up to date, {2} total." -f $compiled, $skipped, $objects.Count)
    # Published via a script-scope variable rather than a return value: returning
    # a value would make callers capture this function's output stream, which
    # contains the native compile commands' stdout.
    $script:BuiltObjects = $objects.ToArray()
}

# ---------------------------------------------------------------------------
# Symbol table: llvm-objdump -t -> nm-style text -> symtab_gen.py
# ---------------------------------------------------------------------------
function New-Symtab {
    $script:SymtabObject = $null
    if (-not $Objdump) { Write-Warn2 "llvm-objdump missing - skipping kernel symbol table (stack traces will show 'unknown')."; return }
    if (-not $PythonExe) { Write-Warn2 "python missing - skipping kernel symbol table."; return }

    Write-Step "Generating kernel symbol table..."
    $raw = Join-Path $Root "obj\.boot.sym.raw"
    Invoke-Native $Objdump (@("-t", (Join-Path $Root "boot.bin"))) $raw -NoAppend
    if ($script:NativeExit -ne 0 -or -not (Test-Path $raw)) { Write-Warn2 "llvm-objdump failed - skipping symbol table."; return }

    # llvm-objdump -t lines look like:
    #   0000000000001000 g     F .text	0000000000000020 kmain
    # nm -n lines look like:
    #   0000000000001000 T kmain
    $nmLines = New-Object System.Collections.Generic.List[string]
    foreach ($line in Get-Content $raw) {
        if ($line -notmatch '^([0-9a-fA-F]{16})\s+(\S+)\s+(\S+)\s+(\S+)\s*(.*)$') { continue }
        $addr = $Matches[1]; $type = $Matches[3]; $rest = $Matches[5]
        if ($type -notmatch '^[Ff]$') { continue }
        $fields = $rest -split '\s+' | Where-Object { $_ }
        if ($fields.Count -lt 1) { continue }
        $name = $fields[-1]
        $nmType = if ($type -eq 'F') { 'T' } else { 't' }
        $nmLines.Add("$addr $nmType $name")
    }
    # nm -n sorts ascending by address; symtab_gen.py's lookup relies on order.
    $sorted = $nmLines | Sort-Object { [Convert]::ToUInt64(($_ -split ' ')[0], 16) }
    $symFile = Join-Path $Root "kernel.sym"
    Set-Content -Path $symFile -Value $sorted -Encoding ASCII
    Write-Step ("Symbol table: {0} text symbols." -f $sorted.Count)

    $gen = Join-Path $Root "src\kernel_symtab.c"
    $pyArgs = ($PythonArgs + @("symtab_gen.py")) -join " "
    & cmd.exe /d /c "`"$PythonExe`" $pyArgs < kernel.sym > src\kernel_symtab.c 2>&1"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $gen)) {
        Write-Err "symtab_gen.py failed."
        exit 1
    }
    $script:SymtabObject = (Join-Path $Root "obj\kernel_symtab.o")
}

function Build-Kernel {
    param([string[]]$Objects)

    $rsp = Join-Path $Root "obj\.link.rsp"
    $rspLines = foreach ($o in $Objects) { '"' + (Join-Path $Root ($o -replace '/', '\')) + '"' }

    Write-Step "Linking boot.bin (phase 1)..."
    Set-Content -Path $rsp -Value $rspLines -Encoding ASCII
    Invoke-Native $Lld (@($LdFlags) + @("-o", (Join-Path $Root "boot.bin"), "@$rsp"))
    if ($script:NativeExit -ne 0 -or -not (Test-Path (Join-Path $Root "boot.bin"))) {
        Write-Err "Phase-1 link failed. This usually means the linker is not ELF-capable."
        Get-LogTail 40 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }

    New-Symtab
    $symtabObj = $script:SymtabObject

    $finalObjects = @($Objects)
    if ($symtabObj) {
        $flags = Get-FlagsForSource "src/kernel_symtab.c"
        Invoke-Native $Clang (@($flags) + @("-c", "-o", $symtabObj, (Join-Path $Root "src\kernel_symtab.c")))
        if ($script:NativeExit -ne 0) {
            Write-Warn2 "kernel_symtab.c failed to compile - linking without symbols."
        } else {
            $finalObjects += "obj/kernel_symtab.o"
        }
    }

    Write-Step "Linking kernel_final.bin (phase 2)..."
    $rspLines2 = foreach ($o in $finalObjects) { '"' + (Join-Path $Root ($o -replace '/', '\')) + '"' }
    Set-Content -Path $rsp -Value $rspLines2 -Encoding ASCII
    Invoke-Native $Lld (@($LdFlags) + @("-o", (Join-Path $Root "kernel_final.bin"), "@$rsp"))
    if ($script:NativeExit -ne 0 -or -not (Test-Path (Join-Path $Root "kernel_final.bin"))) {
        Write-Err "Final link failed:"
        Get-LogTail 40 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }
    $size = (Get-Item (Join-Path $Root "kernel_final.bin")).Length
    Write-Step ("kernel_final.bin built ({0:N0} bytes)." -f $size)
}

# ---------------------------------------------------------------------------
# ISO
# ---------------------------------------------------------------------------
function Build-Iso {
    if (-not (Test-Path (Join-Path $Root "kernel_final.bin"))) { throw "kernel_final.bin not found - run -Target kernel first." }
    if (-not $Xorriso) { throw "xorriso not found - cannot build os.iso." }

    Write-Step "Staging ISO tree..."
    foreach ($d in @("iso\boot\limine", "iso\EFI\BOOT")) {
        $full = Join-Path $Root $d
        if (-not (Test-Path $full)) { New-Item -ItemType Directory -Path $full -Force | Out-Null }
    }
    Copy-Item (Join-Path $Root "kernel_final.bin") (Join-Path $Root "iso\boot\boot.bin") -Force
    foreach ($f in @("limine.cfg")) {
        Copy-Item (Join-Path $Root $f) (Join-Path $Root "iso\boot\limine\") -Force
    }
    foreach ($f in @("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin")) {
        $src = Join-Path $Root "limine\$f"
        if (-not (Test-Path $src)) { throw "Missing limine asset: limine\$f" }
        Copy-Item $src (Join-Path $Root "iso\boot\limine\") -Force
    }
    foreach ($f in @("BOOTX64.EFI", "BOOTIA32.EFI")) {
        $src = Join-Path $Root "limine\$f"
        if (Test-Path $src) { Copy-Item $src (Join-Path $Root "iso\EFI\BOOT\") -Force }
    }

    Write-Step "Creating os.iso..."
    $isoArgs = @(
        "-as", "mkisofs",
        "-b", "boot/limine/limine-bios-cd.bin",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "--efi-boot", "boot/limine/limine-uefi-cd.bin",
        "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
        "iso", "-o", "os.iso"
    )
    Invoke-Native $Xorriso $isoArgs
    if ($script:NativeExit -ne 0 -or -not (Test-Path (Join-Path $Root "os.iso"))) {
        Write-Err "xorriso failed:"
        Get-LogTail 30 | ForEach-Object { Write-Host "    $_" }
        exit 1
    }

    if ($Limine) {
        Write-Step "Installing Limine BIOS bootloader into os.iso..."
        Invoke-Native $Limine @("bios-install", (Join-Path $Root "os.iso"))
        if ($script:NativeExit -ne 0) { Write-Warn2 "limine bios-install returned $script:NativeExit (the ISO may still boot via its El Torito image)." }
    } else {
        Write-Warn2 "limine.exe not found - skipped bios-install."
    }
    Write-Step ("os.iso built ({0:N0} bytes)." -f (Get-Item (Join-Path $Root "os.iso")).Length)
}

# ---------------------------------------------------------------------------
# Headless boot smoke test
# ---------------------------------------------------------------------------
function Invoke-BootTest {
    param([int]$TimeoutSec = 60)
    if (-not $Qemu) { throw "QEMU not found - cannot run the boot test." }
    if (-not (Test-Path (Join-Path $Root "os.iso"))) { throw "os.iso not found - build it first." }

    # Empty stand-in disks, mirroring the CI boot test.
    foreach ($img in @(@{n = "sata_disk.img"; s = 64MB }, @{ n = "nvme_disk.img"; s = 64MB })) {
        $p = Join-Path $Root $img.n
        if (-not (Test-Path $p)) {
            $fs = [System.IO.File]::Create($p); $fs.SetLength($img.s); $fs.Close()
            Write-Step "Created $($img.n) ($($img.s / 1MB) MB)"
        }
    }

    $serial = Join-Path $Root "serial-win.log"
    if (Test-Path $serial) { Remove-Item $serial -Force }

    $qemuArgs = @(
        "-boot", "d", "-cdrom", (Join-Path $Root "os.iso"),
        "-smp", "4", "-m", "1G", "-M", "q35",
        "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
        "-drive", "file=$(Join-Path $Root 'sata_disk.img'),format=raw,index=0",
        "-drive", "file=$(Join-Path $Root 'nvme_disk.img'),format=raw,index=1,media=disk",
        "-device", "virtio-vga,xres=1920,yres=1080",
        "-display", "none", "-no-reboot",
        "-serial", "file:$serial"
    )
    Write-Step "Booting headlessly for $TimeoutSec s (serial -> serial-win.log)..."
    $proc = Start-Process -FilePath $Qemu -ArgumentList $qemuArgs -PassThru -WindowStyle Hidden
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) { Start-Sleep -Milliseconds 500 }
    if (-not $proc.HasExited) { $proc.Kill(); Start-Sleep -Milliseconds 500 }
    Write-Step "QEMU stopped. Serial log: serial-win.log"
    if (Test-Path $serial) {
        Write-Host ""
        Write-Host "--- serial output (tail) ---"
        Get-Content $serial -Tail 30
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Host "inDevOS Windows build (clang + ld.lld)" -ForegroundColor Magenta
Write-Host "=====================================" -ForegroundColor Magenta
Write-Host ""

if ($Clean -or $Target -eq "clean") {
    Invoke-Clean
    if ($Target -eq "clean") { exit 0 }
}

if (-not (Test-Path (Join-Path $Root "obj"))) { New-Item -ItemType Directory -Path (Join-Path $Root "obj") | Out-Null }
Set-Content -Path $BuildLog -Value "inDevOS build log $(Get-Date -Format o)" -Encoding ASCII

Test-Toolchain

switch ($Target) {
    "all" {
        Build-Objects -Force:($Clean) -KeepGoing:$KeepGoing
        Build-Kernel $script:BuiltObjects
        Build-Iso
        if ($BootTest) { Invoke-BootTest -TimeoutSec $BootTimeoutSec }
    }
    "kernel" {
        Build-Objects -Force:($Clean) -KeepGoing:$KeepGoing
        Build-Kernel $script:BuiltObjects
    }
    "iso" {
        Build-Iso
    }
    "boot" {
        Invoke-BootTest -TimeoutSec $BootTimeoutSec
    }
    "install-iso" {
        Build-Objects -Force:($Clean) -KeepGoing:$KeepGoing
        Build-Kernel $script:BuiltObjects
        & (Join-Path $Root "make_install_iso.ps1")
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}

Write-Host ""
Write-Step "Build completed successfully."
Write-Host "Run it with:  .\run_windows.bat        (or: .\build_windows.ps1 -Target boot)"
