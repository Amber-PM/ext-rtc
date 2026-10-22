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
	[Parameter(Mandatory = $true)][ValidateSet('8.4.27', '8.5.12')][string]$PhpVersion,
	[Parameter(Mandatory = $true)][string]$BuildDirectory,
	[switch]$InsidePhpSdk,
	[switch]$PreflightOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath = [IO.Path]::GetFullPath($BuildDirectory)
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root '.build'))
$buildRootPrefix = $buildRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if(Test-Path -LiteralPath $buildRoot){
	$buildRootItem = Get-Item -LiteralPath $buildRoot -Force
	if(($buildRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){
		throw 'ext-rtc/.build cannot be a symlink or reparse point'
	}
}

if(!$buildPath.StartsWith($buildRootPrefix, [StringComparison]::OrdinalIgnoreCase)){
	throw 'BuildDirectory must be a child of ext-rtc/.build'
}

$cursor = $buildRoot
foreach($segment in $buildPath.Substring($buildRootPrefix.Length).Split([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)){
	if([string]::IsNullOrWhiteSpace($segment)){ throw 'BuildDirectory contains an empty path component' }
	$cursor = Join-Path $cursor $segment
	if(Test-Path -LiteralPath $cursor){
		$item = Get-Item -LiteralPath $cursor -Force
		if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){
			throw 'BuildDirectory cannot pass through a symlink or reparse point'
		}
	}
}

$lockPath = Join-Path $root 'build/dependencies.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json -AsHashtable
$target = $lock.windows.targets[$PhpVersion]
if($null -eq $target){ throw "PHP $PhpVersion is not a locked Windows target" }

if($PreflightOnly){
	[pscustomobject]@{
		phpVersion = $PhpVersion
		versionId = $target.versionId
		phpApi = $target.phpApi
		extensionBuild = $target.extensionBuild
		compiler = $target.compiler
		visualStudio = $target.visualStudio
		toolsetMinMinor = $target.toolsetMinMinor
		toolsetMaxMinor = $target.toolsetMaxMinor
		runtimeArchive = $target.runtimeArchive
		runtimeSha256 = $target.runtimeSha256
		phpSourceCommit = $target.sourceCommit
		opensslCommit = $lock.openssl.commit
		opensslConfigureOptions = @($lock.windows.opensslConfigureOptions)
		libdatachannelCommit = $lock.libdatachannel.commit
	} | ConvertTo-Json -Compress
	return
}

if($root -match '\s' -or $buildPath -match '\s'){
	throw 'The pinned PHP SDK batch wrappers do not quote workspace paths containing whitespace; use a workspace path without whitespace for native Windows builds'
}

$ownerMarker = Join-Path $buildPath '.ext-rtc-generated'
$ownerValue = 'Amber-PM ext-rtc Windows CI build v1'
$logs = Join-Path $buildPath 'logs'

function Invoke-Logged{
	param(
		[Parameter(Mandatory = $true)][string]$Program,
		[Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$Label,
		[Parameter(Mandatory = $true)][string]$LogPath
	)
	$output = @(& $Program @Arguments 2>&1)
	$status = $LASTEXITCODE
	$output | ForEach-Object { [string]$_ } | Tee-Object -FilePath $LogPath -Append
	if($status -ne 0){ throw "$Label failed with exit code $status; see $LogPath" }
	return ,$output
}

function Invoke-LoggedFrom{
	param(
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[Parameter(Mandatory = $true)][string]$Program,
		[Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$Label,
		[Parameter(Mandatory = $true)][string]$LogPath
	)
	Push-Location -LiteralPath $WorkingDirectory
	try{
		Invoke-Logged -Program $Program -Arguments $Arguments -Label $Label -LogPath $LogPath
	}finally{
		Pop-Location
	}
}

function Get-GitValue{
	param([Parameter(Mandatory = $true)][string[]]$Arguments)
	$value = & git.exe @Arguments 2>&1
	$status = $LASTEXITCODE
	if($status -ne 0){ throw "git $($Arguments -join ' ') failed with exit code $status" }
	return (($value | ForEach-Object { [string]$_ }) -join [Environment]::NewLine).Trim()
}

function Sync-LockedRepository{
	param(
		[Parameter(Mandatory = $true)][string]$Name,
		[Parameter(Mandatory = $true)][string]$Repository,
		[Parameter(Mandatory = $true)][string]$Commit,
		[Parameter(Mandatory = $true)][string]$Destination
	)
	if(!(Test-Path -LiteralPath $Destination)){
		[void](Invoke-Logged -Program 'git.exe' -Arguments @('clone', $Repository, $Destination) -Label "$Name clone" -LogPath (Join-Path $logs "$Name-clone.log"))
	}
	if(!(Test-Path -LiteralPath (Join-Path $Destination '.git'))){ throw "$Name checkout exists without Git metadata" }
	$origin = Get-GitValue -Arguments @('-C', $Destination, 'remote', 'get-url', 'origin')
	if($origin.TrimEnd('/') -ne $Repository.TrimEnd('/')){ throw "$Name origin does not match the dependency lock" }
	$status = Get-GitValue -Arguments @('-C', $Destination, 'status', '--porcelain', '--untracked-files=all')
	if($status.Length -ne 0){ throw "$Name checkout is dirty; refusing to reuse it" }
	[void](Invoke-Logged -Program 'git.exe' -Arguments @('-C', $Destination, 'checkout', '--detach', $Commit) -Label "$Name locked checkout" -LogPath (Join-Path $logs "$Name-checkout.log"))
	$actual = Get-GitValue -Arguments @('-C', $Destination, 'rev-parse', 'HEAD')
	if($actual -ne $Commit){ throw "$Name commit mismatch: expected $Commit, got $actual" }
}

function Assert-PhpTarget{
	param([Parameter(Mandatory = $true)]$Identity)
	if($Identity.Version -ne $PhpVersion -or
		$Identity.VersionId -ne $target.versionId -or
		!$Identity.Zts -or
		$Identity.IntSize -ne 8 -or
		$Identity.Debug -or
		$Identity.Api -ne $target.phpApi -or
		$Identity.ExtensionBuild -ne $target.extensionBuild -or
		$Identity.Compiler -ne $target.compiler){
		throw "PHP runtime ABI does not match the locked $PhpVersion ZTS x64 target"
	}
}

if(!$InsidePhpSdk){
	if(Test-Path -LiteralPath $buildPath){
		if(!(Test-Path -LiteralPath $ownerMarker -PathType Leaf) -or (Get-Content -LiteralPath $ownerMarker -Raw).Trim() -ne $ownerValue){
			throw 'BuildDirectory already exists and is not an ext-rtc-owned build directory'
		}
		Get-ChildItem -LiteralPath $buildPath -Force | Where-Object { $_.FullName -ne $ownerMarker } | Remove-Item -Recurse -Force
	}else{
		New-Item -ItemType Directory -Path $buildPath -Force | Out-Null
	}
	Set-Content -LiteralPath $ownerMarker -Value $ownerValue -Encoding utf8
	New-Item -ItemType Directory -Path $logs -Force | Out-Null

	foreach($tool in @('git.exe', 'curl.exe', 'pwsh.exe')){
		if(!(Get-Command $tool -ErrorAction SilentlyContinue)){ throw "Missing required host tool $tool" }
	}
	$sdk = $lock.windows.phpSdk
	$sdkPath = Join-Path $buildPath 'tooling/php-sdk'
	if(!(Test-Path -LiteralPath $sdkPath)){
		[void](Invoke-Logged -Program 'git.exe' -Arguments @('clone', '--branch', $sdk.tag, $sdk.repository, $sdkPath) -Label 'Pinned PHP SDK clone' -LogPath (Join-Path $logs 'php-sdk-clone.log'))
	}
	$sdkCommit = Get-GitValue -Arguments @('-C', $sdkPath, 'rev-parse', 'HEAD')
	if($sdkCommit -ne $sdk.commit){ throw "PHP SDK pin mismatch: expected $($sdk.commit), got $sdkCommit" }
	$sdkOrigin = Get-GitValue -Arguments @('-C', $sdkPath, 'remote', 'get-url', 'origin')
	if($sdkOrigin.TrimEnd('/') -ne $sdk.repository.TrimEnd('/')){ throw 'PHP SDK origin does not match the dependency lock' }
	$sdkStatus = Get-GitValue -Arguments @('-C', $sdkPath, 'status', '--porcelain', '--untracked-files=all')
	if($sdkStatus.Length -ne 0){ throw 'Pinned PHP SDK checkout is dirty' }

	$vswhere = Join-Path $sdkPath 'bin/vswhere.exe'
	if(!(Test-Path -LiteralPath $vswhere -PathType Leaf)){ throw 'Pinned PHP SDK does not contain vswhere.exe' }
	$vsInstallOutput = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath 2>&1
	$vswhereStatus = $LASTEXITCODE
	if($vswhereStatus -ne 0){ throw "Pinned SDK vswhere failed with exit code $vswhereStatus" }
	$vsInstall = @($vsInstallOutput | ForEach-Object { [string]$_ } | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -First 1)
	if($vsInstall.Count -ne 1){ throw 'Visual Studio with the x64 C++ tool component was not found on windows-2022' }
	$toolsetRoot = Join-Path $vsInstall[0].Trim() 'VC/Tools/MSVC'
	$toolsets = @(Get-ChildItem -LiteralPath $toolsetRoot -Directory | Where-Object {
		try{
			$v = [version]$_.Name
			$v.Major -eq 14 -and $v.Minor -ge $target.toolsetMinMinor -and $v.Minor -le $target.toolsetMaxMinor -and
				(Test-Path -LiteralPath (Join-Path $_.FullName 'bin/Hostx64/x64/cl.exe'))
		}catch{ $false }
	} | Sort-Object { [version]$_.Name } -Descending)
	if($toolsets.Count -eq 0){
		throw "No installed MSVC toolset matches $($target.visualStudio) minor range $($target.toolsetMinMinor)-$($target.toolsetMaxMinor); no Visual Studio installation will be modified"
	}
	$selectedToolset = $toolsets[0].Name
	Set-Content -LiteralPath (Join-Path $buildPath 'selected-toolset.txt') -Value $selectedToolset -Encoding ascii

	$taskFile = Join-Path $buildPath 'run-under-php-sdk.bat'
	$scriptPath = [IO.Path]::GetFullPath($PSCommandPath)
	$taskContents = '@echo off' + [Environment]::NewLine +
		'pwsh.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $scriptPath +
		'" -PhpVersion ' + $PhpVersion + ' -BuildDirectory "' + $buildPath + '" -InsidePhpSdk' + [Environment]::NewLine +
		'exit /b %ERRORLEVEL%' + [Environment]::NewLine
	Set-Content -LiteralPath $taskFile -Value $taskContents -Encoding ascii
	$starter = Join-Path $sdkPath ("phpsdk-{0}-x64.bat" -f $target.visualStudio)
	if(!(Test-Path -LiteralPath $starter -PathType Leaf)){ throw "Pinned PHP SDK starter missing: $starter" }
	Push-Location -LiteralPath $buildPath
	try{
		$startOutput = & $starter -c $target.visualStudio -a x64 -s $selectedToolset -t 'run-under-php-sdk.bat' 2>&1
		$startStatus = $LASTEXITCODE
		$startOutput | ForEach-Object { [string]$_ } | Tee-Object -FilePath (Join-Path $logs 'php-sdk-start.log')
	}finally{
		Pop-Location
	}
	if($startStatus -ne 0){ throw "PHP SDK task failed with exit code $startStatus; see $logs/php-sdk-start.log" }
	if(!(Test-Path -LiteralPath (Join-Path $buildPath 'build-complete.json'))){ throw 'PHP SDK task exited without producing the build completion manifest' }
	Write-Output "Windows native build complete for PHP ${PhpVersion}: $buildPath"
	return
}

if(!(Test-Path -LiteralPath $ownerMarker -PathType Leaf) -or (Get-Content -LiteralPath $ownerMarker -Raw).Trim() -ne $ownerValue){
	throw 'Inner PHP SDK task must run inside an ext-rtc-owned .build directory'
}
New-Item -ItemType Directory -Path $logs -Force | Out-Null
if($env:VSCMD_ARG_TGT_ARCH -ne 'x64'){ throw 'PHP SDK did not initialize the x64 target architecture' }
$expectedToolset = (Get-Content -LiteralPath (Join-Path $buildPath 'selected-toolset.txt') -Raw).Trim()
$actualToolset = ($env:VCToolsVersion -replace '[\\/]+$', '').Trim()
if($actualToolset -ne $expectedToolset){ throw "MSVC toolset mismatch: expected $expectedToolset, initialized $actualToolset" }
$toolsetVersion = [version]$actualToolset
if($toolsetVersion.Major -ne 14 -or $toolsetVersion.Minor -lt $target.toolsetMinMinor -or $toolsetVersion.Minor -gt $target.toolsetMaxMinor){
	throw "Initialized MSVC toolset $actualToolset is outside the locked PHP $PhpVersion compatibility range"
}
foreach($tool in @('cl.exe', 'nmake.exe', 'cmake.exe', 'git.exe', 'dumpbin.exe', 'perl.exe', 'ml64.exe', 're2c.exe', 'bison.exe')){
	if(!(Get-Command $tool -ErrorAction SilentlyContinue)){ throw "PHP SDK environment is missing required tool $tool" }
}

$sdkPath = Join-Path $buildPath 'tooling/php-sdk'
$phpSource = Join-Path $buildPath 'source/php-src'
$opensslSource = Join-Path $buildPath 'source/openssl'
$backendSource = Join-Path $buildPath 'source/libdatachannel'
$runtimePath = Join-Path $buildPath 'reference-runtime'
$prefix = Join-Path $buildPath 'install'
$null = New-Item -ItemType Directory -Path $prefix -Force
$toolchainSource = Join-Path $buildPath 'toolchain-probe.cpp'
$toolchainExe = Join-Path $buildPath 'toolchain-probe.exe'
Set-Content -LiteralPath $toolchainSource -Value '#if _MSVC_LANG < 202002L
#error C++20 is required
#endif
static_assert(sizeof(void*) == 8, "x64 is required");
int main(){return 0;}' -Encoding ascii
& cl.exe /nologo /EHsc /std:c++20 /MD "/Fe$toolchainExe" $toolchainSource 2>&1 | Tee-Object -FilePath (Join-Path $logs 'toolchain-probe.log')
if($LASTEXITCODE -ne 0){ throw 'Selected MSVC toolchain cannot compile the required x64 C++20 /MD probe' }
& $toolchainExe
if($LASTEXITCODE -ne 0){ throw 'Selected MSVC toolchain probe failed at runtime' }
@{
	target = 'x64'
	phpVersion = $PhpVersion
	visualStudioFamily = $target.visualStudio
	visualStudioHost = $env:VisualStudioVersion
	toolset = $actualToolset
	compiler = $target.compiler
	compilerExecutable = (Get-Command cl.exe).Source
	cppStandard = 'C++20'
	runtime = 'MultiThreadedDLL (/MD)'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $buildPath 'toolchain.json') -Encoding utf8

. (Join-Path $PSScriptRoot 'php-identity.ps1')
$runtimeArchive = Join-Path $buildPath $target.runtimeArchive
$runtimeUrl = '{0}/{1}' -f $lock.windows.phpRuntimeBaseUrl.TrimEnd('/'), $target.runtimeArchive
if(!(Test-Path -LiteralPath $runtimeArchive -PathType Leaf)){
	[void](Invoke-Logged -Program 'curl.exe' -Arguments @('--fail', '--location', '--retry', '3', '--output', $runtimeArchive, $runtimeUrl) -Label 'Official PHP runtime download' -LogPath (Join-Path $logs 'php-runtime-download.log'))
}
$runtimeSha = (Get-FileHash -LiteralPath $runtimeArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if($runtimeSha -ne $target.runtimeSha256){ throw "Official PHP runtime SHA-256 mismatch: expected $($target.runtimeSha256), got $runtimeSha" }
if(Test-Path -LiteralPath $runtimePath){ throw 'Reference runtime extraction unexpectedly exists inside a fresh build directory' }
Expand-Archive -LiteralPath $runtimeArchive -DestinationPath $runtimePath
$referencePhp = Join-Path $runtimePath 'php.exe'
if(!(Test-Path -LiteralPath $referencePhp -PathType Leaf)){ throw 'Official PHP runtime archive did not contain php.exe' }
$referenceIdentity = Get-RtcPhpIdentity -PhpExe $referencePhp
Assert-PhpTarget -Identity $referenceIdentity
$referenceIdentity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $buildPath 'php-abi.json') -Encoding utf8

Sync-LockedRepository -Name 'php-src' -Repository $lock.windows.phpSourceRepository -Commit $target.sourceCommit -Destination $phpSource
$sourceTagCommit = Get-GitValue -Arguments @('-C', $phpSource, 'rev-parse', "$($target.sourceTag)^{commit}")
if($sourceTagCommit -ne $target.sourceCommit){ throw "PHP source tag $($target.sourceTag) no longer points to the locked commit" }
Sync-LockedRepository -Name 'openssl' -Repository $lock.openssl.repository -Commit $lock.openssl.commit -Destination $opensslSource
Sync-LockedRepository -Name 'libdatachannel' -Repository $lock.libdatachannel.repository -Commit $lock.libdatachannel.commit -Destination $backendSource
$backendTagCommit = Get-GitValue -Arguments @('-C', $backendSource, 'rev-parse', "$($lock.libdatachannel.tag)^{commit}")
if($backendTagCommit -ne $lock.libdatachannel.commit){ throw "libdatachannel tag $($lock.libdatachannel.tag) no longer points to the locked commit" }
[void](Invoke-Logged -Program 'git.exe' -Arguments @('-C', $backendSource, 'submodule', 'update', '--init', '--recursive', '--depth', '1') -Label 'libdatachannel submodules' -LogPath (Join-Path $logs 'submodules-update.log'))
foreach($entry in $lock.libdatachannel.submodules.GetEnumerator()){
	$actual = Get-GitValue -Arguments @('-C', (Join-Path $backendSource $entry.Key), 'rev-parse', 'HEAD')
	if($actual -ne $entry.Value){ throw "libdatachannel submodule pin mismatch at $($entry.Key): expected $($entry.Value), got $actual" }
}
$submoduleStatus = Get-GitValue -Arguments @('-C', $backendSource, 'submodule', 'status', '--recursive')
$submoduleStatus | Set-Content -LiteralPath (Join-Path $logs 'submodules.txt') -Encoding utf8
[void](Invoke-Logged -Program 'git.exe' -Arguments @('-C', $backendSource, 'submodule', 'foreach', '--recursive', 'git diff --quiet && git diff --cached --quiet') -Label 'submodule cleanliness check' -LogPath (Join-Path $logs 'submodules-clean.log'))

$patchedBackendSource = Join-Path $buildPath 'source/libdatachannel-patched'
[void](Invoke-Logged -Program 'cmake.exe' -Arguments @("-DROOT=$root", "-DSOURCE=$backendSource", "-DDESTINATION=$patchedBackendSource", '-P', (Join-Path $root 'build/patches/prepare-backend.cmake')) -Label 'Verified private backend patch preparation' -LogPath (Join-Path $logs 'backend-patches.log'))

New-Item -ItemType Directory -Path (Join-Path $buildPath 'source') -Force | Out-Null
$opensslInstall = Join-Path $prefix 'openssl'
$opensslArgs = @(
	'Configure',
	'VC-WIN64A',
	'shared',
	'no-tests',
	"--prefix=$($opensslInstall.Replace('\', '/'))",
	"--openssldir=$($opensslInstall.Replace('\', '/'))/ssl",
	'--libdir=lib'
)
$opensslConfigureOptions = @($lock.windows.opensslConfigureOptions)
if($opensslConfigureOptions -notcontains 'no-asm'){
	throw 'Windows OpenSSL configuration must disable optional assembly because NASM is not provisioned by the runner'
}
$opensslArgs += $opensslConfigureOptions
[void](Invoke-LoggedFrom -WorkingDirectory $opensslSource -Program 'perl.exe' -Arguments $opensslArgs -Label 'OpenSSL configure' -LogPath (Join-Path $logs 'openssl-configure.log'))
[void](Invoke-LoggedFrom -WorkingDirectory $opensslSource -Program 'nmake.exe' -Arguments @() -Label 'OpenSSL build' -LogPath (Join-Path $logs 'openssl-build.log'))
[void](Invoke-LoggedFrom -WorkingDirectory $opensslSource -Program 'nmake.exe' -Arguments @('install_sw') -Label 'OpenSSL install_sw' -LogPath (Join-Path $logs 'openssl-install.log'))
$opensslExe = Join-Path $opensslInstall 'bin/openssl.exe'
if(!(Test-Path -LiteralPath $opensslExe -PathType Leaf)){ throw 'Pinned OpenSSL install did not produce openssl.exe' }
$opensslVersionOutput = & $opensslExe version 2>&1
$opensslVersionStatus = $LASTEXITCODE
$opensslVersionOutput | Set-Content -LiteralPath (Join-Path $logs 'openssl-version.txt') -Encoding utf8
if($opensslVersionStatus -ne 0 -or ($opensslVersionOutput -join ' ') -notmatch ('^OpenSSL ' + [regex]::Escape($lock.openssl.version) + '(\s|$)')){
	throw "Private OpenSSL runtime does not report version $($lock.openssl.version)"
}
$opensslHeader = Get-Content -LiteralPath (Join-Path $opensslInstall 'include/openssl/opensslv.h') -Raw
if($opensslHeader -notmatch ('(?m)^#\s*define OPENSSL_VERSION_STR "' + [regex]::Escape($lock.openssl.version) + '"')){
	throw 'Private OpenSSL headers do not match the locked version'
}
$opensslCryptoImport = Join-Path $opensslInstall 'lib/libcrypto.lib'
$opensslSslImport = Join-Path $opensslInstall 'lib/libssl.lib'
$opensslCryptoDll = Join-Path $opensslInstall 'bin/libcrypto-3-x64.dll'
$opensslSslDll = Join-Path $opensslInstall 'bin/libssl-3-x64.dll'
foreach($file in @($opensslCryptoImport, $opensslSslImport, $opensslCryptoDll, $opensslSslDll)){
	if(!(Test-Path -LiteralPath $file -PathType Leaf)){ throw "Required OpenSSL import library/runtime missing: $file" }
}

$backendInstall = Join-Path $prefix 'libdatachannel'
$backendBuild = Join-Path $buildPath 'backend'
$backendFlags = @()
foreach($entry in $lock.cmake.GetEnumerator()){ $backendFlags += "-D$($entry.Key)=$($entry.Value)" }
$cmakeArgs = @(
	'-S', $patchedBackendSource,
	'-B', $backendBuild,
	'-G', 'NMake Makefiles',
	"-DCMAKE_INSTALL_PREFIX=$backendInstall",
	'-DCMAKE_BUILD_TYPE=Release',
	'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
	'-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
	'-DCMAKE_C_FLAGS_RELEASE=/MD /O2 /DNDEBUG',
	'-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /DNDEBUG',
	'-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
	'-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
	"-DOPENSSL_ROOT_DIR=$opensslInstall",
	"-DOPENSSL_INCLUDE_DIR=$opensslInstall/include",
	"-DOPENSSL_SSL_LIBRARY=$opensslSslImport",
	"-DOPENSSL_CRYPTO_LIBRARY=$opensslCryptoImport",
	'-DOPENSSL_USE_STATIC_LIBS=OFF'
) + $backendFlags
[void](Invoke-Logged -Program 'cmake.exe' -Arguments $cmakeArgs -Label 'libdatachannel configure' -LogPath (Join-Path $logs 'backend-configure.log'))
$cmakeCache = Get-Content -LiteralPath (Join-Path $backendBuild 'CMakeCache.txt') -Raw
foreach($entry in @{
	'CMAKE_MSVC_RUNTIME_LIBRARY' = 'MultiThreadedDLL'
	'CMAKE_POLICY_DEFAULT_CMP0091' = 'NEW'
	'CMAKE_C_FLAGS_RELEASE' = '/MD /O2 /DNDEBUG'
	'CMAKE_CXX_FLAGS_RELEASE' = '/MD /O2 /DNDEBUG'
	'CMAKE_C_FLAGS' = '-DICE_MAX_CANDIDATES_COUNT=64'
}.GetEnumerator()){
	$cacheEntry = [regex]::Match($cmakeCache, '(?m)^' + [regex]::Escape($entry.Key) + ':[^=]*=([^\r\n]*)\r?$')
	if(!$cacheEntry.Success -or $cacheEntry.Groups[1].Value -ne $entry.Value){
		throw "libdatachannel CMake did not preserve the required MSVC runtime setting $($entry.Key)=$($entry.Value)"
	}
}
foreach($library in @($opensslSslImport, $opensslCryptoImport)){
	$normalized = [regex]::Escape($library.Replace('\', '/'))
	if($cmakeCache.Replace('\', '/') -notmatch "(?m)^[^=\r\n]+=[^\r\n]*$normalized\s*$"){
		throw "libdatachannel CMake did not select the locked OpenSSL import library: $library"
	}
}
$backendLog = Join-Path $logs 'backend-build.log'
$backendOutput = & cmake.exe --build $backendBuild --parallel 2 --verbose 2>&1
$backendStatus = $LASTEXITCODE
$backendOutput | ForEach-Object { [string]$_ } | Tee-Object -FilePath $backendLog
if($backendStatus -ne 0){ throw "libdatachannel build failed with exit code $backendStatus; see $backendLog" }
[void](Invoke-Logged -Program 'cmake.exe' -Arguments @('--install', $backendBuild) -Label 'libdatachannel install' -LogPath (Join-Path $logs 'backend-install.log'))
$backendDll = Join-Path $backendInstall 'bin/datachannel.dll'
$backendImport = Join-Path $backendInstall 'lib/datachannel.lib'
if(!(Test-Path -LiteralPath $backendDll -PathType Leaf) -or !(Test-Path -LiteralPath $backendImport -PathType Leaf)){
	throw 'Pinned libdatachannel did not install the expected datachannel.dll/datachannel.lib pair'
}
$backendCache = Get-Content -LiteralPath (Join-Path $backendBuild 'CMakeCache.txt') -Raw
foreach($flag in $lock.cmake.GetEnumerator()){
	if($backendCache -notmatch ("(?m)^" + [regex]::Escape($flag.Key) + ':[^=]*=' + [regex]::Escape([string]$flag.Value) + '\s*$')){
		throw "CMake did not preserve locked libdatachannel option $($flag.Key)=$($flag.Value)"
	}
}

$probeExe = Join-Path $backendInstall 'bin/ext-rtc-backend-probe.exe'
[void](Invoke-Logged -Program 'cl.exe' -Arguments @('/nologo', '/std:c++20', '/EHsc', '/MD', '/DRTC_ENABLE_MEDIA=0', '/DRTC_ENABLE_WEBSOCKET=0', "/I$backendInstall/include", (Join-Path $root 'build/patches/backend-probe.cpp'), "/Fo$backendBuild/backend-probe.obj", "/Fe$probeExe", '/link', $backendImport) -Label 'Backend lifetime overload compile and link probe' -LogPath (Join-Path $logs 'backend-probe-build.log'))
$probePath = $env:PATH
try{
 $env:PATH = (Join-Path $opensslInstall 'bin') + [IO.Path]::PathSeparator + $probePath
 [void](Invoke-Logged -Program $probeExe -Arguments @($backendDll) -Label 'Backend runtime path and identity probe' -LogPath (Join-Path $logs 'backend-probe-run.log'))
}finally{ $env:PATH = $probePath }

$extension = Join-Path $phpSource 'ext/rtc'
$srcRoot = Join-Path $root 'src'
Get-ChildItem -LiteralPath $srcRoot -Recurse -File | Where-Object { $_.Extension -in @('.cpp', '.h') } | ForEach-Object {
	$rel = $_.FullName.Substring($srcRoot.Length).TrimStart([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
	$dest = Join-Path $extension (Join-Path 'src' $rel)
	$destDir = Split-Path -Path $dest -Parent
	if(!(Test-Path -LiteralPath $destDir)){
		New-Item -ItemType Directory -Path $destDir -Force | Out-Null
	}
	Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
}
foreach($name in @('php_rtc.h', 'config.w32')){
	Copy-Item -LiteralPath (Join-Path $root $name) -Destination $extension -Force
}
Push-Location -LiteralPath $phpSource
try{
	[void](Invoke-Logged -Program '.\buildconf.bat' -Arguments @('--force') -Label 'PHP buildconf' -LogPath (Join-Path $logs 'php-buildconf.log'))
	$configArgs = @(
		'--disable-all',
		'--enable-cli',
		'--enable-zts',
		'--disable-cgi',
		'--disable-phpdbg',
		'--without-pear',
		'--with-verbosity=1',
		'--enable-rtc=shared',
		"--with-libdatachannel=$backendInstall"
	)
	[void](Invoke-Logged -Program '.\configure.bat' -Arguments $configArgs -Label 'PHP configure' -LogPath (Join-Path $logs 'php-configure.log'))
	[void](Invoke-Logged -Program 'nmake.exe' -Arguments @() -Label 'PHP and rtc extension build' -LogPath (Join-Path $logs 'php-build.log'))
}finally{
	Pop-Location
}
$phpBuildText = Get-Content -LiteralPath (Join-Path $logs 'php-build.log') -Raw
if($phpBuildText -notmatch '(?i)(^|\s)/MD(\s|$)'){
	throw 'PHP Release_TS verbose build did not show the required /MD CRT'
}

$phpRelease = Join-Path $phpSource 'x64/Release_TS'
$builtPhp = Join-Path $phpRelease 'php.exe'
if(!(Test-Path -LiteralPath $builtPhp -PathType Leaf)){ throw 'PHP build did not produce x64/Release_TS/php.exe' }
$modules = @(Get-ChildItem -LiteralPath $phpRelease -Filter 'php_rtc.dll' -File -Recurse)
if($modules.Count -ne 1){ throw "Expected exactly one native php_rtc.dll under the PHP release directory, found $($modules.Count)" }
$module = $modules[0].FullName
$builtIdentity = Get-RtcPhpIdentity -PhpExe $builtPhp
Assert-PhpTarget -Identity $builtIdentity
Assert-RtcPhpIdentity -Expected $referenceIdentity -Actual $builtIdentity

$backendRuntimeDlls = @(Get-ChildItem -LiteralPath (Join-Path $backendInstall 'bin') -Filter '*.dll' -File)
if($backendRuntimeDlls.Count -eq 0){ throw 'libdatachannel runtime DLL set is empty' }
$runtimeDlls = @($backendRuntimeDlls) + @(
	Get-Item -LiteralPath $opensslCryptoDll
	Get-Item -LiteralPath $opensslSslDll
)
foreach($directory in @($phpRelease, $runtimePath)){
	foreach($dll in $runtimeDlls){
		Copy-Item -LiteralPath $dll.FullName -Destination $directory -Force
	}
}
$originalPath = $env:PATH
try{
	$env:PATH = $phpRelease + [IO.Path]::PathSeparator + $runtimePath + [IO.Path]::PathSeparator + $originalPath
	foreach($runtime in @($builtPhp, $referencePhp)){
		$loadLog = Join-Path $logs ("module-info-{0}.txt" -f (Split-Path -Leaf (Split-Path -Parent $runtime)))
		$loadOutput = & $runtime -n -d "extension=$module" --ri rtc 2>&1
		$loadStatus = $LASTEXITCODE
		$loadOutput | Set-Content -LiteralPath $loadLog -Encoding utf8
		if($loadStatus -ne 0 -or ($loadOutput -join ' ') -match '(?i)PHP Startup:|Unable to load dynamic library|warning:'){
			throw "php_rtc.dll failed to load cleanly in $runtime; inspect $loadLog"
		}
		if(($loadOutput -join ' ') -notmatch 'rtc support => enabled'){
			throw "php_rtc.dll did not report enabled rtc support in $runtime"
		}
	}
}finally{
	$env:PATH = $originalPath
}

$moduleDependencies = & dumpbin.exe /dependents $module 2>&1
if($LASTEXITCODE -ne 0){ throw 'dumpbin could not inspect php_rtc.dll dependencies' }
$backendDependencies = & dumpbin.exe /dependents $backendDll 2>&1
if($LASTEXITCODE -ne 0){ throw 'dumpbin could not inspect datachannel.dll dependencies' }
if(($moduleDependencies -join ' ') -notmatch '(?i)datachannel\.dll'){ throw 'php_rtc.dll does not import the pinned datachannel.dll' }
if(($backendDependencies -join ' ') -notmatch '(?i)libssl-3-x64\.dll' -or ($backendDependencies -join ' ') -notmatch '(?i)libcrypto-3-x64\.dll'){
	throw 'datachannel.dll does not import both locked OpenSSL runtime DLLs'
}
$moduleDependencies | Set-Content -LiteralPath (Join-Path $logs 'module-dependencies.txt') -Encoding utf8
$backendDependencies | Set-Content -LiteralPath (Join-Path $logs 'backend-dependencies.txt') -Encoding utf8
$allDlls = @($module, $backendDll, $opensslCryptoDll, $opensslSslDll)
Get-FileHash -LiteralPath $allDlls -Algorithm SHA256 | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $logs 'sha256.json') -Encoding utf8
$submoduleStatus | Set-Content -LiteralPath (Join-Path $logs 'submodules.txt') -Encoding utf8
@{
	phpVersion = $PhpVersion
	target = $referenceIdentity
	built = $builtIdentity
	module = $module
	backend = $backendDll
	opensslVersion = $lock.openssl.version
	opensslCommit = $lock.openssl.commit
	opensslConfigureOptions = $opensslConfigureOptions
	libdatachannelCommit = $lock.libdatachannel.commit
	toolset = $actualToolset
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $buildPath 'build-complete.json') -Encoding utf8

$notices = Join-Path $buildPath 'notices'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'build/patches') -Destination $notices -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'third-party/licenses') -Destination $notices -Recurse -Force
foreach($name in @('LICENSE', 'build/dependencies.lock.json', 'third-party/NOTICES.md')){
	Copy-Item -LiteralPath (Join-Path $root $name) -Destination $notices -Force
}
Write-Output "Native php_rtc.dll built and loaded for PHP $PhpVersion"
Write-Output "PHP runtime: $referencePhp"
Write-Output "Extension: $module"

