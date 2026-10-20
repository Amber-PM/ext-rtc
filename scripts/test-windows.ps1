#
#  _____ __  _______   ____ _____ ____  
# | ____|\ \/ /_   _| |  _ \_   _/ ___| 
# |  _|   \  /  | |   | |_) || || |     
# | |___ /  \  | |   |  _ < | || |___   
# |_____/_/\_\ |_|   |_| \_\|_| \____|  
#
# Copyright (c) 2026 Amber-PM contributors
# SPDX-License-Identifier: MIT
#
# https://github.com/Amber-PM/ext-rtc
#

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][ValidateSet('8.1.34', '8.4.24')][string]$PhpVersion,
	[Parameter(Mandatory = $true)][string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath = [IO.Path]::GetFullPath($BuildDirectory)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root '.build'))
$prefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if(Test-Path -LiteralPath $buildRoot){
	$buildRootItem = Get-Item -LiteralPath $buildRoot -Force
	if(($buildRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){
		throw 'ext-rtc/.build cannot be a symlink or reparse point'
	}
}
if(!$buildPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)){
	throw 'BuildDirectory must be a child of ext-rtc/.build'
}
$cursor = $buildRoot
foreach($segment in $buildPath.Substring($prefix.Length).Split([IO.Path]::DirectorySeparatorChar)){
	if([string]::IsNullOrWhiteSpace($segment)){ throw 'BuildDirectory contains an empty path component' }
	$cursor = Join-Path $cursor $segment
	if(Test-Path -LiteralPath $cursor){
		$item = Get-Item -LiteralPath $cursor -Force
		if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){
			throw 'BuildDirectory cannot pass through a symlink or reparse point'
		}
	}
}
$ownerMarker = Join-Path $buildPath '.ext-rtc-generated'
if(!(Test-Path -LiteralPath $ownerMarker -PathType Leaf) -or (Get-Content -LiteralPath $ownerMarker -Raw).Trim() -ne 'Amber-PM ext-rtc Windows CI build v1'){
	throw 'Windows tests require an ext-rtc-owned native build directory'
}
$manifestPath = Join-Path $buildPath 'build-complete.json'
if(!(Test-Path -LiteralPath $manifestPath -PathType Leaf)){
	throw 'Windows tests require build-complete.json from a completed native build'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if($manifest.phpVersion -ne $PhpVersion -or !$manifest.module -or !$manifest.backend){
	throw 'Windows build completion manifest does not match the requested PHP target'
}
$lock = Get-Content -LiteralPath (Join-Path $root 'build/dependencies.lock.json') -Raw | ConvertFrom-Json
$target = $lock.windows.targets.$PhpVersion
if($null -eq $target){ throw "PHP $PhpVersion is not a locked Windows test target" }
$module = [IO.Path]::GetFullPath([string]$manifest.module)
$backend = [IO.Path]::GetFullPath([string]$manifest.backend)
$buildPrefix = $buildPath.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
foreach($artifact in @($module, $backend)){
	if(!$artifact.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)){
		throw 'Windows build manifest references an artifact outside its owned .build directory'
	}
}
$phpSource = Join-Path $buildPath 'source/php-src'
$phpRelease = Join-Path $phpSource 'x64/Release_TS'
$builtPhp = Join-Path $phpRelease 'php.exe'
$referencePhp = Join-Path $buildPath 'reference-runtime/php.exe'
$runner = Join-Path $phpSource 'run-tests.php'
foreach($file in @($module, $backend, $builtPhp, $referencePhp, $runner)){
	if(!(Test-Path -LiteralPath $file -PathType Leaf)){ throw "Required Windows build artifact is missing: $file" }
}
$logs = Join-Path $buildPath 'logs'
New-Item -ItemType Directory -Path $logs -Force | Out-Null

. (Join-Path $PSScriptRoot 'php-identity.ps1')
$referenceIdentity = Get-RtcPhpIdentity -PhpExe $referencePhp
$builtIdentity = Get-RtcPhpIdentity -PhpExe $builtPhp
Assert-RtcPhpIdentity -Expected $referenceIdentity -Actual $builtIdentity
if($referenceIdentity.Version -ne $PhpVersion -or
	$referenceIdentity.VersionId -ne $target.versionId -or
	!$referenceIdentity.Zts -or
	$referenceIdentity.IntSize -ne 8 -or
	$referenceIdentity.Debug -or
	$referenceIdentity.Api -ne $target.phpApi -or
	$referenceIdentity.ExtensionBuild -ne $target.extensionBuild -or
	$referenceIdentity.Compiler -ne $target.compiler){
	throw "Reference PHP runtime ABI does not match locked $PhpVersion ZTS x64"
}
$referenceIdentity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logs 'php-abi-tested.json') -Encoding utf8

& (Join-Path $root 'tests/build/php-identity.ps1') -PhpExe $referencePhp 2>&1 | Tee-Object -FilePath (Join-Path $logs 'php-identity-regression.log')
if($LASTEXITCODE -ne 0){ throw 'PHP ABI mismatch regression failed' }
& $referencePhp -n (Join-Path $root 'tests/contract/check.php') 2>&1 | Tee-Object -FilePath (Join-Path $logs 'contract-check.log')
if($LASTEXITCODE -ne 0){ throw 'API contract check failed under the target Windows PHP runtime' }

$originalPath = $env:PATH
$originalInteraction = $env:NO_INTERACTION
$originalReport = $env:REPORT_EXIT_STATUS
$originalTestPhp = $env:TEST_PHP_EXECUTABLE
$originalOfferer = $env:RTC_TEST_OFFERER
try{
	$env:PATH = $phpRelease + [IO.Path]::PathSeparator + (Join-Path $buildPath 'reference-runtime') + [IO.Path]::PathSeparator + $originalPath
	$nativeDirectory = Join-Path $buildPath 'native-tests'
	$backendPrefix = Split-Path (Split-Path $backend -Parent) -Parent
	$nativeToolset = if($PhpVersion -eq '8.1.34'){ 'v142' }else{ 'v143' }
	& cmake -S (Join-Path $root 'tests/native') -B $nativeDirectory -G 'Visual Studio 17 2022' -A x64 -T $nativeToolset -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "-DCMAKE_PREFIX_PATH=$backendPrefix"
	if($LASTEXITCODE -ne 0){ throw 'Native transport harness configuration failed' }
	& cmake --build $nativeDirectory --config Release --parallel 2
	if($LASTEXITCODE -ne 0){ throw 'Native transport harness build failed' }
	$env:RTC_TEST_OFFERER = Join-Path $nativeDirectory 'Release/rtc-offerer.exe'
	& ctest --test-dir $nativeDirectory -C Release --output-on-failure
	if($LASTEXITCODE -ne 0){ throw 'Native transport lifecycle failed' }
	foreach($runtime in @($builtPhp, $referencePhp)){
		$output = & $runtime -n -d "extension=$module" --ri rtc 2>&1
		$status = $LASTEXITCODE
		$logName = if($runtime -eq $builtPhp){ 'module-info-built-php.txt' }else{ 'module-info-reference-php.txt' }
		$output | Set-Content -LiteralPath (Join-Path $logs $logName) -Encoding utf8
		if($status -ne 0 -or ($output -join ' ') -notmatch 'rtc support => enabled' -or ($output -join ' ') -match '(?i)PHP Startup:|Unable to load dynamic library|warning:'){
			throw "Native php_rtc.dll failed to load cleanly in $runtime; inspect $(Join-Path $logs $logName)"
		}
	}

	$expected = @(Get-ChildItem -LiteralPath (Join-Path $root 'tests') -Filter '*.phpt' -File).Count
	if($expected -eq 0){ throw 'Windows PHPT suite is empty' }
	$phptLog = Join-Path $logs 'phpt.log'
	$env:NO_INTERACTION = '1'
	$env:REPORT_EXIT_STATUS = '1'
	$env:TEST_PHP_EXECUTABLE = $referencePhp
	$arguments = @('-n', $runner, '-n', '-d', "extension=$module", (Join-Path $root 'tests'))
	& $referencePhp @arguments 2>&1 | Tee-Object -FilePath $phptLog
	$phptStatus = $LASTEXITCODE
	if($phptStatus -ne 0){ throw "Windows PHPT runner failed with exit code $phptStatus; see $phptLog" }
	& $referencePhp -n (Join-Path $PSScriptRoot 'assert-phpt-summary.php') $expected $phptLog
	if($LASTEXITCODE -ne 0){ throw 'Windows PHPT summary contains failed, skipped, warned, or unselected tests' }
	& $referencePhp -n (Join-Path $PSScriptRoot 'smoke.php') $module 2>&1 | Tee-Object -FilePath (Join-Path $logs 'lifecycle-smoke.log')
	if($LASTEXITCODE -ne 0){ throw 'Windows native module lifecycle smoke failed' }
}finally{
	$env:PATH = $originalPath
	if($null -eq $originalInteraction){ Remove-Item Env:NO_INTERACTION -ErrorAction SilentlyContinue }else{ $env:NO_INTERACTION = $originalInteraction }
	if($null -eq $originalReport){ Remove-Item Env:REPORT_EXIT_STATUS -ErrorAction SilentlyContinue }else{ $env:REPORT_EXIT_STATUS = $originalReport }
	if($null -eq $originalTestPhp){ Remove-Item Env:TEST_PHP_EXECUTABLE -ErrorAction SilentlyContinue }else{ $env:TEST_PHP_EXECUTABLE = $originalTestPhp }
	if($null -eq $originalOfferer){ Remove-Item Env:RTC_TEST_OFFERER -ErrorAction SilentlyContinue }else{ $env:RTC_TEST_OFFERER = $originalOfferer }
}

Write-Output "PASS: Windows PHP $PhpVersion module load, $expected PHPT cases, and 100 lifecycle processes"
