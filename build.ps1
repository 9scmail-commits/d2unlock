param(
    [string]$Zig = 'zig',
    [string]$ExecutableName = 'D2Unlock.exe',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist'),
    [string]$TestWork = (Join-Path $PSScriptRoot '.build'),
    [string]$Dota = '',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$compiler = (Get-Command $Zig -ErrorAction Stop).Source
$artifactDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$launcherPath = Join-Path $artifactDirectory $ExecutableName
$testDirectory = [IO.Path]::GetFullPath($TestWork)
$payloadDirectory = Join-Path $testDirectory 'payload'
New-Item -ItemType Directory -Force -Path $artifactDirectory,$testDirectory,$payloadDirectory | Out-Null
function Invoke-Compile([string]$Name, [string[]]$CompilerArguments) {
    $logPath = Join-Path $testDirectory ($Name + '.build.log')
    & $compiler c++ -std=c++20 -O2 -g0 -s -Wall -Wextra @CompilerArguments *> $logPath
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $logPath -Tail 40; throw "Compilation failed: $Name" }
    Write-Output "Built: $Name"
}
Push-Location $PSScriptRoot
try {
    Invoke-Compile 'CatalogBuilder' @('-municode', '-static', '-o', "$payloadDirectory/CatalogBuilder.exe", 'native/catalog.cpp', '-ladvapi32')
    Invoke-Compile 'InventoryClient' @('-shared', '-o', "$payloadDirectory/InventoryClient.dll", 'native/client.cpp', '-ladvapi32', '-lws2_32', '-ld3d11', '-ld2d1', '-ldwrite', '-lole32', '-luuid')
    Invoke-Compile 'InventoryBridge' @('-municode', '-static', '-o', "$payloadDirectory/InventoryBridge.exe", 'native/launcher.cpp', '-ladvapi32')
    $resource = "101 RCDATA `"$($payloadDirectory.Replace('\','/'))/InventoryClient.dll`"`n102 RCDATA `"$($payloadDirectory.Replace('\','/'))/InventoryBridge.exe`"`n103 RCDATA `"$($payloadDirectory.Replace('\','/'))/CatalogBuilder.exe`""
    $icon = Join-Path $PSScriptRoot 'native/d2unlock.ico'
    if (Test-Path -LiteralPath $icon) { $resource = "1 ICON `"$($icon.Replace('\','/'))`"`n" + $resource }
    [IO.File]::WriteAllText("$payloadDirectory/payload.rc", $resource)
    & $compiler rc /fo "$payloadDirectory/payload.res" "$payloadDirectory/payload.rc" *> "$testDirectory/resources.build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$testDirectory/resources.build.log"; throw 'Resource compilation failed' }
    Invoke-Compile 'Dotaunlock' @('-municode', '-Wl,/subsystem:windows', '-static', '-o', "$launcherPath", 'native/gui.cpp', "$payloadDirectory/payload.res", '-lgdi32', '-luser32', '-lshell32', '-ldwmapi', '-lgdiplus', '-ladvapi32', '-lwinhttp', '-lshlwapi')
    if ($Test) {
        if ($Dota) { & "$payloadDirectory/CatalogBuilder.exe" $Dota $payloadDirectory }
        else { & "$payloadDirectory/CatalogBuilder.exe" $payloadDirectory }
        if ($LASTEXITCODE -ne 0) { throw 'Catalogue generation failed' }
        $p = Start-Process -FilePath "$launcherPath" -ArgumentList @('--smoke-test', ('"' + $testDirectory + '/launcher.bmp"')) -WindowStyle Hidden -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw 'GUI smoke test failed' }
        $p = Start-Process -FilePath "$launcherPath" -ArgumentList @('--smoke-test', ('"' + $testDirectory + '/hero-panel.bmp"'), ('"' + $payloadDirectory + '"')) -WindowStyle Hidden -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw 'Hero panel smoke test failed' }
        foreach ($page in @(@(1,'settings-panel'),@(7,'welcome-panel'))) {
            $p = Start-Process -FilePath "$launcherPath" -ArgumentList @('--smoke-test', ('"' + $testDirectory + '/' + $page[1] + '.bmp"'), ('"' + $payloadDirectory + '"'), $page[0]) -WindowStyle Hidden -Wait -PassThru
            if ($p.ExitCode -ne 0) { throw "Panel smoke test failed: $($page[1])" }
        }
        $install = Join-Path $testDirectory ('install-' + [Guid]::NewGuid().ToString('N'))
        $p = Start-Process -FilePath "$launcherPath" -ArgumentList @('--install-only', ('"' + $install + '"')) -WindowStyle Hidden -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw 'Self-contained installation failed' }
        foreach ($name in @('InventoryClient.dll','InventoryBridge.exe','CatalogBuilder.exe')) {
            $extracted = Get-ChildItem -LiteralPath $install -Recurse -Filter $name
            if ($extracted.Count -ne 1 -or (Get-FileHash $extracted.FullName).Hash -ne (Get-FileHash "$payloadDirectory/$name").Hash) { throw "Embedded payload mismatch: $name" }
        }
        Write-Output 'PASS: launcher pages render and the embedded components extract intact'
    }
} finally { Pop-Location }
