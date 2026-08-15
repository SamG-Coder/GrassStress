$ErrorActionPreference = 'Stop'
$toolRoot = 'C:\msys64\ucrt64\bin'
$cmake = Join-Path $toolRoot 'cmake.exe'
$ninja = Join-Path $toolRoot 'ninja.exe'
$compiler = Join-Path $toolRoot 'g++.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'MSYS2 UCRT64 CMake is missing.' }
$env:Path = "$toolRoot;$env:Path"
$dxc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter dxc.exe -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $dxc) { throw 'DirectX Shader Compiler is missing. Install Microsoft.DirectX.ShaderCompiler with winget.' }
New-Item -ItemType Directory -Force -Path 'build\shaders' | Out-Null
New-Item -ItemType Directory -Force -Path 'build\bin' | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'
$cl = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe'
if (Test-Path -LiteralPath $vcvars) {
    $bridgeCmd = 'call "' + $vcvars + '" && cl /nologo /O2 /LD /EHsc /std:c++20 /I third_party\streamline\include /DSL_BRIDGE_EXPORTS src\sl_bridge.cpp /Fo:build\sl_bridge.obj /Fe:build\bin\sl_bridge.dll /link d3d12.lib dxgi.lib'
    cmd.exe /c $bridgeCmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'MSVC sl_bridge.dll not built (Windows SDK missing). Native G-buffer path still runs.'
    }
} elseif (Test-Path -LiteralPath $cl) {
    Write-Host 'MSVC cl.exe found without vcvars/Windows SDK. Native G-buffer path still runs.'
} else {
    Write-Host 'MSVC missing; Streamline stays off until sl_bridge.dll is built.'
}
& $dxc -T lib_6_6 -HV 2021 -O3 -Fo 'build\shaders\raytracing.dxil' 'shaders\raytracing.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'DXR shader compilation failed.' }
& $dxc -T vs_6_6 -E VSMain -HV 2021 -O3 -Fo 'build\shaders\grass_overlay_vs.dxil' 'shaders\grass_overlay.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'Grass overlay vertex shader compilation failed.' }
& $dxc -T ps_6_6 -E PSMain -HV 2021 -O3 -Fo 'build\shaders\grass_overlay_ps.dxil' 'shaders\grass_overlay.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'Grass overlay pixel shader compilation failed.' }
& $dxc -T vs_6_6 -E VSMain -HV 2021 -O3 -Fo 'build\shaders\hud_overlay_vs.dxil' 'shaders\hud_overlay.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'HUD vertex shader compilation failed.' }
& $dxc -T ps_6_6 -E PSMain -HV 2021 -O3 -Fo 'build\shaders\hud_overlay_ps.dxil' 'shaders\hud_overlay.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'HUD pixel shader compilation failed.' }
& $dxc -T vs_6_6 -E VSMain -HV 2021 -O3 -Fo 'build\shaders\present_tonemap_vs.dxil' 'shaders\present_tonemap.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'Present vertex shader compilation failed.' }
& $dxc -T ps_6_6 -E PSMain -HV 2021 -O3 -Fo 'build\shaders\present_tonemap_ps.dxil' 'shaders\present_tonemap.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'Present pixel shader compilation failed.' }
& $dxc -T cs_6_6 -E TreeWindCS -HV 2021 -O3 -Fo 'build\shaders\tree_wind.dxil' 'shaders\tree_wind.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'Tree wind compute shader compilation failed.' }
& $dxc -T cs_6_6 -E CSMain -HV 2021 -O3 -Fo 'build\shaders\sm_denoise.dxil' 'shaders\sm_denoise.hlsl'
if ($LASTEXITCODE -ne 0) { throw 'SM denoise compute shader compilation failed.' }
& $cmake -S . -B build -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build build --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& (Join-Path $toolRoot 'ctest.exe') --test-dir build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
$slBin = 'third_party\streamline\bin\x64'
$outSl = 'build\bin\streamline'
New-Item -ItemType Directory -Force -Path $outSl | Out-Null
if (Test-Path $slBin) {
    foreach ($dll in @(
        'sl.interposer.dll','sl.common.dll','sl.dlss.dll','sl.dlss_d.dll','sl.dlss_g.dll',
        'sl.reflex.dll','sl.pcl.dll','nvngx_dlss.dll','nvngx_dlssd.dll','nvngx_dlssg.dll'
    )) {
        $src = Join-Path $slBin $dll
        if (Test-Path $src) { Copy-Item -Force $src (Join-Path $outSl $dll) }
    }
}
foreach ($dll in @(
    'sl.interposer.dll','sl.common.dll','sl.dlss.dll','sl.dlss_d.dll','sl.dlss_g.dll',
    'sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll'
)) {
    $loose = Join-Path 'build\bin' $dll
    if (Test-Path $loose) { Remove-Item -Force $loose }
}
# NGX Super Resolution loads nvngx_dlss.dll from the exe directory. Frame-gen
# DLLs and sl.interposer stay only under build/bin/streamline — that interposer
# faults before wWinMain if it sits beside the MinGW exe.
foreach ($dll in @('nvngx_dlss.dll','nvngx_dlssd.dll')) {
    $src = Join-Path $outSl $dll
    if (Test-Path $src) { Copy-Item -Force $src (Join-Path 'build\bin' $dll) }
}
Write-Host 'Built: build\bin\GrassStress.exe'
