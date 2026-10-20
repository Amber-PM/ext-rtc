function Get-RtcPhpIdentity{
	param([Parameter(Mandatory = $true)][string]$PhpExe)
	$json = & $PhpExe -n -r 'echo json_encode([PHP_VERSION,PHP_VERSION_ID,PHP_ZTS,PHP_INT_SIZE,PHP_DEBUG]);'
	if($LASTEXITCODE -ne 0){ throw 'Cannot inspect PHP runtime' }
	$abi = $json | ConvertFrom-Json
	$details = (& $PhpExe -n -i) -join [Environment]::NewLine
	if($LASTEXITCODE -ne 0){ throw 'Cannot inspect PHP build identity' }
	$fields = @{}
	foreach($field in @('PHP API', 'PHP Extension Build', 'Compiler')){
		$match = [regex]::Match($details, '(?m)^' + [regex]::Escape($field) + '\s*=>\s*([^\r\n]+)')
		if(!$match.Success){ throw "Missing PHP identity field: $field" }
		$fields[$field] = $match.Groups[1].Value.Trim()
	}
	[pscustomobject]@{
		Version = [string]$abi[0]
		VersionId = [int]$abi[1]
		Zts = [bool]$abi[2]
		IntSize = [int]$abi[3]
		Debug = [bool]$abi[4]
		Api = $fields['PHP API']
		ExtensionBuild = $fields['PHP Extension Build']
		Compiler = $fields['Compiler']
	}
}

function Assert-RtcPhpIdentity{
	param(
		[Parameter(Mandatory = $true)]$Expected,
		[Parameter(Mandatory = $true)]$Actual
	)
	foreach($field in @('Version', 'VersionId', 'Zts', 'IntSize', 'Debug', 'Api', 'ExtensionBuild', 'Compiler')){
		if($Expected.$field -cne $Actual.$field){ throw "PHP ABI mismatch: $field" }
	}
}
