$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root
# Each CI run is a new version, so the console takes it as an update rather than a reinstall.
$pkgVersion = "0.2.$(if ($env:GITHUB_RUN_NUMBER) { $env:GITHUB_RUN_NUMBER } else { 0 }).0"
foreach ($d in "gen", "gen\core", "gen\host", "gen\uwp", "gen\desktop", "gen\tests", "gen\inspect", "gen\manifests", "pkg", "out") {
    New-Item -ItemType Directory -Force $d | Out-Null
}
$info = New-Object System.Collections.Generic.List[string]
function Note($s) { Write-Output $s; $info.Add($s) }
function Run($exe, [string[]]$a) {
    Write-Output "> $exe $($a -join ' ')"
    & $exe @a
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with $LASTEXITCODE" }
}

Note "cl: $((Get-Command cl.exe).Source)"
Note "sdk: $env:WindowsSDKVersion"
Note "version: $pkgVersion"

# No /guard:cf: the runtime calls code it writes below 4 GB. core, host and tests compile with the desktop API family
# so that VirtualAlloc2 and AddVectoredExceptionHandler are declared; the imports still come from WindowsApp.lib and
# onecore.lib.
# _USE_STD_VECTOR_ALGORITHMS=0: the MSVC STL otherwise links its AVX2 vector-algorithm bodies (dispatched at run time,
# but every one would need an ISA-gate allow). The UCRT's own dispatched bodies remain and are allowed by name.
$common = @("/nologo", "/std:c++latest", "/permissive-", "/EHsc", "/O2", "/MT", "/W3", "/Zi", "/bigobj", "/utf-8",
    "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS",
    "/D_USE_STD_VECTOR_ALGORITHMS=0", "/I$root")
$cppwinrt = Join-Path $env:WindowsSdkDir "Include\$env:WindowsSDKVersion\cppwinrt"
$app = @("/DWINAPI_FAMILY=WINAPI_FAMILY_PC_APP", "/I$cppwinrt")

function Compile($outDir, [string[]]$extra, [string[]]$sources) {
    Run cl.exe ($common + $extra + @("/c", "/Fd$outDir\", "/Fo$outDir\") + $sources)
}
# /std:c11 and the conformant preprocessor: XbSymbolDatabase builds as C11 and its OOVPA tables expand variadic macros
# that the traditional MSVC preprocessor mangles (run 40276849600).
$cflags = @("/nologo", "/O2", "/MT", "/W0", "/Zi", "/bigobj", "/std:c11", "/Zc:preprocessor", "/utf-8",
    "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS")
$zydisFlags = @("/DZYDIS_STATIC_BUILD", "/DZYCORE_STATIC_BUILD", "/DZYDIS_DISABLE_ENCODER",
    "/Iexternal\zydis\include", "/Iexternal\zydis\src", "/Iexternal\zydis\dependencies\zycore\include")
# Zydis and Zycore each have a String.c: one output directory per library, or the second overwrites the first.
New-Item -ItemType Directory -Force gen\zydis, gen\zycore, gen\xbsdb | Out-Null
Run cl.exe ($cflags + $zydisFlags + @("/c", "/Fdgen\zydis\", "/Fogen\zydis\") +
    (Get-ChildItem external\zydis\src\*.c | ForEach-Object { $_.FullName }))
Run cl.exe ($cflags + $zydisFlags + @("/c", "/Fdgen\zycore\", "/Fogen\zycore\") +
    (Get-ChildItem external\zydis\dependencies\zycore\src\*.c | ForEach-Object { $_.FullName }))
Run cl.exe ($cflags + @("/Iexternal\XbSymbolDatabase\include", "/Iexternal\XbSymbolDatabase\src", "/Iexternal\XbSymbolDatabase\src\OOVPADatabase", "/Iexternal\XbSymbolDatabase\src\lib", "/c", "/Fdgen\xbsdb\", "/Fogen\xbsdb\") +
    (Get-ChildItem external\XbSymbolDatabase\src\lib\*.c, external\XbSymbolDatabase\src\OOVPADatabase\*.c | ForEach-Object { $_.FullName }))
# The fork's kernel with the direct front end: host\kernel.cpp defines xbox\xbox_frontend.h and the symbols of the files
# in lifted-only.txt, which are left out. App partition with app_partition.h force-included, as the fork's own UWP
# build does; getenv stays declared (_CRT_USE_WINAPI_FAMILY_DESKTOP_APP), as route 2 builds it for the console. A
# library, so the link takes only the objects the adapter reaches (src\kernel's push-buffer executor waits for U5).
New-Item -ItemType Directory -Force gen\fork | Out-Null
$fork = "external\xboxrecomp"
$liftedOnly = Get-Content "$fork\src\kernel\lifted-only.txt"
$forkSources = @(Get-ChildItem "$fork\src\kernel\*.c" | Where-Object { $liftedOnly -notcontains $_.Name } | ForEach-Object { $_.FullName }) +
    @(Get-ChildItem "$fork\src\usb\*.c", "$fork\src\input\*.c" | ForEach-Object { $_.FullName }) +
    @((Get-Item "$fork\src\platform\app_partition.c").FullName)
Run cl.exe ($cflags + @("/DUNICODE", "/D_UNICODE", "/DWINAPI_FAMILY=WINAPI_FAMILY_PC_APP", "/D_CRT_USE_WINAPI_FAMILY_DESKTOP_APP",
    "/DXBOXRECOMP_UWP=1", "/DXBOXRECOMP_FRONTEND_DIRECT=1", "/FI$root\$fork\src\platform\app_partition.h",
    "/I$fork\src\kernel", "/I$fork\src", "/I$fork\include", "/c", "/Fdgen\fork\", "/Fogen\fork\") + $forkSources)
Run lib.exe (@("/nologo", "/OUT:gen\fork\xboxrecomp_kernel.lib") + (Get-ChildItem gen\fork\*.obj | ForEach-Object { $_.FullName }))
# The M0 shader chain's libraries, built by their own CMake: glslang without the SPIR-V optimizer or HLSL input, and
# SPIRV-Cross's core, GLSL and HLSL backends. The static CRT matches /MT; the CL variable keeps the STL's AVX2
# vector-algorithm bodies out of them as it does for xbrt's own code.
$env:CL = "/D_USE_STD_VECTOR_ALGORITHMS=0"
$cmakeCommon = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded", "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl")
Run cmake.exe ($cmakeCommon + @("-S", "external\glslang", "-B", "gen\glslang", "-DENABLE_OPT=OFF", "-DENABLE_HLSL=OFF",
    "-DENABLE_GLSLANG_BINARIES=OFF", "-DGLSLANG_TESTS=OFF", "-DGLSLANG_ENABLE_INSTALL=OFF", "-DBUILD_EXTERNAL=OFF",
    "-DBUILD_SHARED_LIBS=OFF", "-DENABLE_PCH=OFF"))
Run cmake.exe @("--build", "gen\glslang", "--target", "glslang", "glslang-default-resource-limits")
Run cmake.exe ($cmakeCommon + @("-S", "external\SPIRV-Cross", "-B", "gen\spirv-cross", "-DSPIRV_CROSS_CLI=OFF",
    "-DSPIRV_CROSS_ENABLE_TESTS=OFF", "-DSPIRV_CROSS_ENABLE_MSL=OFF", "-DSPIRV_CROSS_ENABLE_CPP=OFF",
    "-DSPIRV_CROSS_ENABLE_REFLECT=OFF", "-DSPIRV_CROSS_ENABLE_C_API=OFF", "-DSPIRV_CROSS_ENABLE_UTIL=OFF",
    "-DSPIRV_CROSS_SHARED=OFF", "-DSPIRV_CROSS_STATIC=ON", "-DSPIRV_CROSS_SKIP_INSTALL=ON"))
Run cmake.exe @("--build", "gen\spirv-cross")
Remove-Item Env:CL
$chainLibs = @(Get-ChildItem gen\glslang -Recurse -Include glslang.lib, glslang-default-resource-limits.lib |
    ForEach-Object { $_.FullName }) + @(Get-ChildItem gen\spirv-cross -Recurse -Include spirv-cross-core.lib,
    spirv-cross-glsl.lib, spirv-cross-hlsl.lib | ForEach-Object { $_.FullName }) + @("d3dcompiler.lib")
Note "shader chain libraries: $(($chainLibs | ForEach-Object { Split-Path $_ -Leaf }) -join ' ')"

$common += @("/DZYDIS_STATIC_BUILD", "/DZYCORE_STATIC_BUILD", "/Iexternal\zydis\include", "/Iexternal\zydis\dependencies\zycore\include", "/Iexternal\XbSymbolDatabase\include",
    "/I$fork\include", "/I$fork\src")
Compile "gen\core" @() (Get-ChildItem core\*.cpp | ForEach-Object { $_.FullName })
Compile "gen\host" @() (Get-ChildItem host\*.cpp, host\cpu\*.cpp | ForEach-Object { $_.FullName })
New-Item -ItemType Directory -Force gen\m0 | Out-Null
Compile "gen\m0" @("/Iexternal\glslang", "/Igen\glslang\include", "/Iexternal\SPIRV-Cross") (Get-ChildItem host\m0\*.cpp | ForEach-Object { $_.FullName })
Compile "gen\uwp" $app (Get-ChildItem shell\uwp\*.cpp | ForEach-Object { $_.FullName })
Compile "gen\desktop" @() ((Get-ChildItem shell\desktop\*.cpp, tests\host\*.cpp) | ForEach-Object { $_.FullName })
Compile "gen\tests" @() (Get-ChildItem tests\core\*.cpp | ForEach-Object { $_.FullName })
Compile "gen\inspect" @() @("tools\xbrt-inspect\main.cpp")

$coreObjs = Get-ChildItem gen\core\*.obj, gen\zydis\*.obj, gen\zycore\*.obj, gen\xbsdb\*.obj | ForEach-Object { $_.FullName }
$hostObjs = @(Get-ChildItem gen\host\*.obj, gen\m0\*.obj | ForEach-Object { $_.FullName }) + @("gen\fork\xboxrecomp_kernel.lib") + $chainLibs
$uwpObjs = Get-ChildItem gen\uwp\*.obj | ForEach-Object { $_.FullName }

Run link.exe (@("/nologo", "/OUT:gen\xbrt.exe", "/SUBSYSTEM:WINDOWS", "/APPCONTAINER", "/DEBUG", "/OPT:REF", "/OPT:ICF", "/PDB:gen\xbrt.pdb") +
    $coreObjs + $hostObjs + $uwpObjs + @("WindowsApp.lib", "onecore.lib", "d3d12.lib", "d3d11.lib", "dxgi.lib"))
Note "xbrt.exe size: $((Get-Item gen\xbrt.exe).Length)"
Note "xbrt.exe imports: $((& dumpbin /imports gen\xbrt.exe | Where-Object { $_ -match '^\s+\S+\.dll$' } | ForEach-Object { $_.Trim() } | Sort-Object -Unique) -join ' ')"
Run link.exe (@("/nologo", "/OUT:gen\xbrt_desktop.exe", "/SUBSYSTEM:CONSOLE", "/DEBUG", "/PDB:gen\xbrt_desktop.pdb") +
    (Get-ChildItem gen\desktop\*.obj | ForEach-Object { $_.FullName }) +
    $coreObjs + $hostObjs + @("onecore.lib", "d3d12.lib", "d3d11.lib", "dxgi.lib"))
Run link.exe (@("/nologo", "/OUT:gen\xbrt_tests.exe", "/SUBSYSTEM:CONSOLE", "/DEBUG", "/PDB:gen\xbrt_tests.pdb") +
    (Get-ChildItem gen\tests\*.obj | ForEach-Object { $_.FullName }) + $coreObjs)
Run link.exe (@("/nologo", "/OUT:gen\xbrt-inspect.exe", "/SUBSYSTEM:CONSOLE", "/DEBUG", "/PDB:gen\xbrt-inspect.pdb", "gen\inspect\main.obj") + $coreObjs)

Run gen\xbrt_tests.exe @()

# The One S (Jaguar) has no AVX2, BMI2 or FMA; the PDB beside the exe names the function of any offender. The allowed
# functions are the static CRT's and STL's own dispatchers: each runs its AVX2 or FMA3 body only when the CPU
# reports that feature (the probe's list plus the UCRT strnlen helpers route 2's gate allowed, all behind
# __isa_available or __use_fma3_lib, checked against the disassembly on the Mac; Jaguar reports neither). The UCRT's
# wmemchr and wmemcmp enter their AVX2 loop only when _Avx2WmemEnabled is set (dumpbin on windows-2022, run 40271103751).
$allow = @(
    '--allow', '^(strnlen|wcslen|wcsnlen|wmemchr|wmemcmp)$',
    '--allow', '^common_strnlen(_simd)?<',
    '--allow', '^\?(compare_equals|compute_byte_mask)@\?\$__crt_simd_(pack_)?traits@',
    '--allow', '^log10$',
    # The UCRT math the shader chain links (glslang folds constants with them). sin, cos, tan, log and exp test
    # __use_fma3_lib at entry and jump to their FMA3 tail only when it is set (llvm-objdump of the 10.0.26100 libucrt.lib
    # objects); the *_fma bodies and __remainder_piby2_fma3 are reached only from such a test, which the --guards
    # listing below prints from this build's own disassembly. Jaguar reports no FMA3, so __use_fma3_lib stays 0.
    '--allow', '^(sin|cos|tan|log|exp)$',
    '--allow', '^[a-z0-9]+_fma$',
    '--allow', '^__remainder_piby2_fma3(_bdl)?$'
)
& python scripts\isa-gate.py --tool dumpbin --guards '^([a-z0-9]+_fma|__remainder_piby2_fma3(_bdl)?)$' gen\xbrt.exe |
    Tee-Object -Variable guards | Write-Output
[IO.File]::WriteAllText("$root\out\gate-guards.txt", ($guards -join "`n") + "`n")
# Windows PowerShell's Tee-Object and Set-Content write UTF-16 and CRLF; the Mac reads these files, so they are
# written as UTF-8 with LF through .NET.
# xbrt.exe goes to the console and must have no offender outside the allowlist. xbrt_desktop.exe runs only on the
# x64 runner and links std::filesystem, whose separately compiled STL bodies carry AVX2 behind their own dispatch; it
# is reported, not gated.
$gate = New-Object System.Collections.Generic.List[string]
foreach ($exe in "gen\xbrt.exe", "gen\xbrt_desktop.exe") {
    [string[]]$gateArgs = @("scripts\isa-gate.py", "--tool", "dumpbin", "--verbose") + $allow
    if ($exe -ne "gen\xbrt.exe") { $gateArgs += "--report-only" }
    $gateArgs += $exe
    $lines = & python $gateArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $lines | Where-Object { $_ -notmatch '^\s+(BMI2|FMA3|FMA4|AVX2|AVX512) ' } | Write-Output
    $gate.AddRange([string[]]$lines)
    if ($code -ne 0) { [IO.File]::WriteAllText("$root\out\gate.txt", ($gate -join "`n") + "`n"); throw "$exe has instructions the One S cannot run (see out\gate.txt)" }
}
[IO.File]::WriteAllText("$root\out\gate.txt", ($gate -join "`n") + "`n")
Note "isa gate: pass (out\gate.txt)"

Run python.exe @("-I", "package\make-manifests.py", $pkgVersion, "gen\manifests")
Copy-Item gen\xbrt.pdb out\xbrt.pdb -Force
Copy-Item gen\xbrt_desktop.exe out\xbrt_desktop.exe -Force
Copy-Item gen\xbrt-inspect.exe out\xbrt-inspect.exe -Force

# Throwaway certificate whose subject is the manifest Publisher. Dev Mode accepts any signature; the key dies with the runner.
$cert = New-SelfSignedCertificate -Type Custom -Subject "CN=LopezJon" -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 2048 -CertStoreLocation Cert:\CurrentUser\My `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") -NotAfter (Get-Date).AddYears(10)
$password = [guid]::NewGuid().ToString()
Export-PfxCertificate -Cert $cert -FilePath sign.pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
$failed = @()
foreach ($manifest in Get-ChildItem gen\manifests\*.xml) {
    $name = ([xml](Get-Content $manifest.FullName)).Package.Identity.Name
    $payload = "pkg\$name"
    Remove-Item -Recurse -Force $payload -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $payload | Out-Null
    Copy-Item gen\xbrt.exe "$payload\xbrt.exe"
    # The M0 probe's corpus (CI fetches it from the pinned m0-corpus release; tools/m0/corpus.lock).
    if (Test-Path gen\m0-corpus.txt) { Copy-Item gen\m0-corpus.txt "$payload\m0-corpus.txt" }
    Run python.exe @("-I", "package\make-assets.py", "$payload\Assets")
    Copy-Item $manifest.FullName "$payload\AppxManifest.xml"
    $appx = "out\${name}_${pkgVersion}_x64.appx"
    Run makeappx.exe @("pack", "/o", "/d", $payload, "/p", $appx)
    & signtool.exe sign /fd SHA256 /f sign.pfx /p $password $appx
    if ($LASTEXITCODE -ne 0) { $failed += $appx }
    Note "appx: $appx"
}
Remove-Item sign.pfx
Remove-Item $cert.PSPath
if ($failed) { throw "signtool failed on $($failed -join ', ')" }

Get-ChildItem out\*.appx, out\*.exe, out\*.pdb | ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" } |
    Tee-Object -Variable sums | Write-Output
[IO.File]::WriteAllText("$root\out\SHA256SUMS.txt", ($sums -join "`n") + "`n")
[IO.File]::WriteAllText("$root\out\build-info.txt", ($info -join "`n") + "`n")
