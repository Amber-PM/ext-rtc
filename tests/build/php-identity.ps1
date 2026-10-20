param([string]$PhpExe = (Get-Command php).Source)
$ErrorActionPreference = 'Stop'
$helper = Join-Path $PSScriptRoot '../../scripts/php-identity.ps1'
if(!(Test-Path -LiteralPath $helper)){ throw 'FAIL: PHP identity implementation missing' }
. $helper
$identity = Get-RtcPhpIdentity -PhpExe $PhpExe
Assert-RtcPhpIdentity -Expected $identity -Actual $identity
foreach($field in @('Api', 'ExtensionBuild', 'Compiler')){
	$changed = $identity.PSObject.Copy()
	$changed.$field = 'different-ABI-field'
	$rejected = $false
	try{ Assert-RtcPhpIdentity -Expected $identity -Actual $changed }catch{
		if($_.Exception.Message -notlike "PHP ABI mismatch: $field*"){ throw }
		$rejected = $true
	}
	if(!$rejected){ throw "FAIL: mismatched $field was accepted" }
}
Write-Output 'PASS: actual PHP identity inspected; API/build/compiler mismatches rejected'
