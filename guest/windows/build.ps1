param([string]$OutDir = "$PSScriptRoot\out")

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$version = '10.0.26100.2454'
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$linker = (Get-Command link.exe -ErrorAction Stop).Source
$scene = Join-Path $PSScriptRoot 'gpu_scene.cpp'
$transport = Join-Path $PSScriptRoot 'gpu_transport.c'
$include = Join-Path $PSScriptRoot '..\..\src'
$out = (New-Item -ItemType Directory -Path $OutDir -Force).FullName

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with $LASTEXITCODE"
    }
}

# Bootstraps headers and kernel import libraries from Microsoft's pinned package.
# The ordinary Windows SDK and MSVC are selected by the x64 developer prompt.
$pkgs = Join-Path $out 'packages'
Invoke-Checked 'nuget.exe' @('install', 'Microsoft.Windows.WDK.x64', '-Version', $version,
    '-DependencyVersion', 'Lowest', '-OutputDirectory', $pkgs, '-NonInteractive', '-DirectDownload',
    '-Source', 'https://api.nuget.org/v3/index.json')
$wdk = Join-Path $pkgs "Microsoft.Windows.WDK.x64.$version"
$header = Get-ChildItem -Path $wdk -Recurse -Filter 'ntddk.h' |
    Where-Object { $_.Directory.Name -eq 'km' } | Select-Object -First 1
$lib = Get-ChildItem -Path $wdk -Recurse -Filter 'ntoskrnl.lib' |
    Where-Object { $_.Directory.Name -eq 'x64' } | Select-Object -First 1
if (!$header -or !$lib) { throw 'Pinned WDK kernel headers/libraries missing' }
$shared = Join-Path $header.Directory.Parent.FullName 'shared'
$obj = Join-Path $out 'gpu_transport.obj'
$driver = Join-Path $out 'ReversePrimeGpu.sys'
Invoke-Checked $compiler @('/nologo','/c','/TC','/std:c17','/W4','/O2','/GS','/kernel',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DNTDDI_VERSION=0x0A000000', '/D_WIN32_WINNT=0x0A00',
    "/I$($header.Directory.FullName)", "/I$shared", "/I$include", "/Fo$obj", $transport)
Invoke-Checked $linker @('/nologo','/driver','/subsystem:native','/machine:x64','/entry:GsDriverEntry',
    '/nodefaultlib','/incremental:no','/dynamicbase','/nxcompat','/integritycheck',
    "/out:$driver", "/libpath:$($lib.Directory.FullName)", $obj,
    'ntoskrnl.lib','hal.lib','wdmsec.lib','BufferOverflowFastFailK.lib')
Copy-Item (Join-Path $PSScriptRoot 'ReversePrimeGpu.inf') $out -Force
Invoke-Checked $compiler @('/nologo','/std:c++17','/EHsc','/W4','/O2','/MT',
    "/Fo$(Join-Path $out 'gpu_scene.obj')","/Fe$(Join-Path $out 'prime_gpu_scene.exe')",
    $scene,'dxgi.lib','d3d11.lib','d3dcompiler.lib')
Invoke-Checked (Join-Path $out 'prime_gpu_scene.exe') @('--reference')
