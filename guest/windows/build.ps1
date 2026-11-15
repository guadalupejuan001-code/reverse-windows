param(
    [string]$OutDir = "$PSScriptRoot\out",
    [switch]$TestSignWddm
)

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
$sdkUm = Join-Path (Join-Path $env:WindowsSdkDir 'Include') (Join-Path $env:WindowsSDKVersion.TrimEnd('\') 'um')
$wdkUm = Join-Path $header.Directory.Parent.FullName 'um'
$graphicsInclude = @("/I$sdkUm", "/I$wdkUm", "/I$shared", "/I$($header.Directory.FullName)")

$infTool = Get-ChildItem -Path $wdk -Recurse -Filter 'Inf2Cat.exe' |
    Where-Object { $_.Directory.Name -in @('x86', 'x64') } | Select-Object -First 1
if (!$infTool) {
    $infTool = Get-ChildItem -Path (Join-Path $env:WindowsSdkDir 'bin') -Recurse -Filter 'Inf2Cat.exe' |
        Where-Object { $_.Directory.Name -in @('x86', 'x64') } | Select-Object -First 1
}
if (!$infTool) { throw 'Inf2Cat.exe is required to generate the package catalogs' }

function New-DriverPackage([string]$Name, [string[]]$Files, [string]$Catalog) {
    $folder = (New-Item -Path (Join-Path $out $Name) -ItemType Directory -Force).FullName
    foreach ($file in $Files) { Copy-Item $file $folder -Force }
    Invoke-Checked $infTool.FullName @("/driver:$folder", '/os:10_X64', '/uselocaltime')
    if (!(Test-Path (Join-Path $folder $Catalog))) { throw "Missing catalog $Catalog" }
}

$obj = Join-Path $out 'gpu_transport.obj'
$driver = Join-Path $out 'ReversePrimeGpu.sys'
Invoke-Checked $compiler @('/nologo','/c','/TC','/std:c17','/W4','/O2','/GS','/kernel',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DNTDDI_VERSION=0x0A000000', '/D_WIN32_WINNT=0x0A00',
    "/I$($header.Directory.FullName)", "/I$shared", "/I$include", "/Fo$obj", $transport)
Invoke-Checked $linker @('/nologo','/driver','/subsystem:native','/machine:x64','/entry:GsDriverEntry',
    '/nodefaultlib','/incremental:no','/dynamicbase','/nxcompat','/integritycheck',
    "/out:$driver", "/libpath:$($lib.Directory.FullName)", $obj,
    'ntoskrnl.lib','hal.lib','wdmsec.lib','BufferOverflowFastFailK.lib')
Invoke-Checked $compiler (@('/nologo','/std:c++17','/EHsc','/W4','/O2','/MT',
    "/Fo$(Join-Path $out 'gpu_scene.obj')","/Fe$(Join-Path $out 'prime_gpu_scene.exe')",
    $scene,'dxgi.lib','d3d11.lib','d3dcompiler.lib','gdi32.lib') + $graphicsInclude)
Invoke-Checked (Join-Path $out 'prime_gpu_scene.exe') @('--reference')

# The display package is separate from the working command transport.
$miniport = Join-Path $PSScriptRoot 'prime_render_kmd.c'
$miniportObj = Join-Path $out 'prime_render_kmd.obj'
$miniportSys = Join-Path $out 'ReversePrimeRender.sys'
Invoke-Checked $compiler @('/nologo','/c','/TC','/std:c17','/W4','/O2','/GS','/kernel',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/DNTDDI_VERSION=0x0A000000', '/D_WIN32_WINNT=0x0A00',
    '/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_WDDM1_3',
    "/I$($header.Directory.FullName)", "/I$shared", "/Fo$miniportObj", $miniport)
Invoke-Checked $linker @('/nologo','/driver','/subsystem:native','/machine:x64','/entry:GsDriverEntry',
    '/nodefaultlib','/incremental:no','/dynamicbase','/nxcompat','/integritycheck',
    "/out:$miniportSys", "/libpath:$($lib.Directory.FullName)", $miniportObj,
    'ntoskrnl.lib','hal.lib','displib.lib','BufferOverflowFastFailK.lib')

$umd = Join-Path $PSScriptRoot 'prime_render_umd.cpp'
$umdDll = Join-Path $out 'ReversePrimeRenderUmd.dll'
Invoke-Checked $compiler (@('/nologo','/std:c++17','/EHsc','/W4','/O2','/MT','/LD',
    "/Fo$(Join-Path $out 'prime_render_umd.obj')","/Fe$umdDll",
    "/Fd$(Join-Path $out 'prime_render_umd.pdb')") + $graphicsInclude + @($umd))
$umdTest = Join-Path $PSScriptRoot 'prime_render_umd_test.cpp'
$umdTestExe = Join-Path $out 'prime_render_umd_test.exe'
Invoke-Checked $compiler (@('/nologo','/std:c++17','/EHsc','/W4','/O2','/MT',
    "/Fo$(Join-Path $out 'prime_render_umd_test.obj')","/Fe$umdTestExe", $umdTest, 'd3dcompiler.lib') + $graphicsInclude)
Invoke-Checked $umdTestExe @($umdDll)
$probe = Join-Path $PSScriptRoot 'prime_wddm_probe.cpp'
Invoke-Checked $compiler (@('/nologo','/std:c++17','/EHsc','/W4','/O2','/MT',
    "/Fo$(Join-Path $out 'prime_wddm_probe.obj')","/Fe$(Join-Path $out 'prime_wddm_probe.exe')",
    $probe, 'dxgi.lib', 'gdi32.lib') + $graphicsInclude)
New-DriverPackage 'wdm-package' @($driver, (Join-Path $PSScriptRoot 'ReversePrimeGpu.inf')) 'ReversePrimeGpu.cat'

$wddmInf = Join-Path $PSScriptRoot 'ReversePrimeRender.inf'
if (!$TestSignWddm) {
    New-DriverPackage 'wddm-package' @($miniportSys, $umdDll, $wddmInf) 'ReversePrimeRender.cat'
    return
}

$sdkBin = Join-Path (Join-Path $env:WindowsSdkDir 'bin') $env:WindowsSDKVersion.TrimEnd('\')
$signTool = Join-Path $sdkBin 'x64\signtool.exe'
if (!(Test-Path $signTool)) { throw 'The Windows SDK x64 SignTool is required for the WDDM test package' }
$certificateParams = @{
    Type = 'CodeSigningCert'
    Subject = 'CN=REVERSE PrimeVGC Development Test'
    CertStoreLocation = 'Cert:\CurrentUser\My'
    KeyAlgorithm = 'RSA'
    KeyLength = 2048
    HashAlgorithm = 'SHA256'
    KeyExportPolicy = 'NonExportable'
    NotAfter = (Get-Date).AddYears(1)
}
$cert = New-SelfSignedCertificate @certificateParams
$thumb = $cert.Thumbprint
$root = "Cert:\CurrentUser\Root\$thumb"
try {
    $sign = @('sign', '/fd', 'SHA256', '/s', 'My', '/sha1', $thumb)
    Invoke-Checked $signTool ($sign + @($miniportSys, $umdDll))
    # Generate the catalog after embedding signatures so it describes the
    # exact signed binaries that are actually shipped.
    New-DriverPackage 'wddm-package' @($miniportSys, $umdDll, $wddmInf) 'ReversePrimeRender.cat'
    $folder = Join-Path $out 'wddm-package'
    $catalog = Join-Path $folder 'ReversePrimeRender.cat'
    Invoke-Checked $signTool ($sign + @($catalog))
    $certPath = Join-Path $folder 'PrimeVGC-Development-Test.cer'
    Export-Certificate -Cert $cert -FilePath $certPath | Out-Null
    Import-Certificate -FilePath $certPath -CertStoreLocation 'Cert:\CurrentUser\Root' | Out-Null
    Invoke-Checked $signTool @('verify', '/pa', $miniportSys)
    Invoke-Checked $signTool @('verify', '/pa', $catalog)
    foreach ($name in @('ReversePrimeRender.sys', 'ReversePrimeRenderUmd.dll', 'ReversePrimeRender.inf')) {
        Invoke-Checked $signTool @('verify', '/pa', '/c', $catalog, (Join-Path $folder $name))
    }
    Write-Output 'PRIME_C08_WDDM_TEST_SIGNATURE_PASS catalog=1 sys=1 umd=1'
} finally {
    if (Test-Path $root) { Remove-Item $root }
    Remove-Item "Cert:\CurrentUser\My\$thumb"
}
