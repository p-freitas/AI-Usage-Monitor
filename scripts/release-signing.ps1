[CmdletBinding(DefaultParameterSetName = 'Sign')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Initialize')][switch]$Initialize,
    [Parameter(Mandatory, ParameterSetName = 'Sign')][string]$Installer,
    [Parameter(Mandatory, ParameterSetName = 'Sign')][string]$Checksums
)
$ErrorActionPreference = 'Stop'
$keyName = 'AI Usage Monitor Release Signing'
$provider = [Security.Cryptography.CngProvider]::MicrosoftSoftwareKeyStorageProvider
$header = Join-Path $PSScriptRoot '..\src\update_public_key.h'

if ($Initialize -and -not [Security.Cryptography.CngKey]::Exists($keyName, $provider)) {
    $params = [Security.Cryptography.CngKeyCreationParameters]::new()
    $params.Provider = $provider
    $params.KeyUsage = [Security.Cryptography.CngKeyUsages]::Signing
    $params.ExportPolicy = [Security.Cryptography.CngExportPolicies]::None
    [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, $keyName, $params).Dispose()
}

if (-not [Security.Cryptography.CngKey]::Exists($keyName, $provider)) {
    throw 'The release key is absent from this Windows account. Run this script with -Initialize on the signing PC.'
}
$key = [Security.Cryptography.CngKey]::Open($keyName, $provider)
try {
    $publicKey = [Convert]::ToBase64String($key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob))
    $embedded = [IO.File]::ReadAllText($header)
    if ($Initialize) {
        if (-not $embedded.Contains('"' + $publicKey + '"')) {
            if ($embedded -notmatch '(?m)^    "",$') {
                throw 'An existing public key is configured. Follow the key rotation procedure before changing it.'
            }
            [IO.File]::WriteAllText($header, ($embedded -replace '(?m)^    "",$', ('    "' + $publicKey + '",')))
        }
        Write-Host 'The public key is in src/update_public_key.h. The private key stays in this Windows account.'
        return
    }

    if (-not $embedded.Contains('"' + $publicKey + '"')) {
        throw 'This PC does not hold a trusted release key for this checkout.'
    }
    $exe = Get-Item -LiteralPath $Installer
    if ($exe.Name -notmatch '^AIUsageMonitor-Setup-(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)[.]exe$') {
        throw 'Expected the versioned AI Usage Monitor installer.'
    }
    $version = $exe.BaseName.Substring('AIUsageMonitor-Setup-'.Length)
    if ([string]$exe.VersionInfo.ProductVersion -notmatch "^$([regex]::Escape($version))\s*$") {
        throw 'The installer version does not match its file name.'
    }
    $file = Get-Item -LiteralPath $Checksums
    if ($file.Name -cne 'SHA256SUMS.txt') { throw 'Expected SHA256SUMS.txt.' }
    $data = [IO.File]::ReadAllBytes($file.FullName)
    $hash = (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = "$hash  $($exe.Name)"
    if ([Text.Encoding]::ASCII.GetString($data).TrimEnd("`r", "`n") -cne $expected) {
        throw 'The checksum list does not match the supplied installer exactly.'
    }
    $signer = [Security.Cryptography.ECDsaCng]::new($key)
    try {
        $signature = $signer.SignData($data, [Security.Cryptography.HashAlgorithmName]::SHA256)
        if (-not $signer.VerifyData($data, $signature, [Security.Cryptography.HashAlgorithmName]::SHA256)) {
            throw 'Signature verification failed.'
        }
        $output = $file.FullName + '.sig'
        [IO.File]::WriteAllText($output, [Convert]::ToBase64String($signature) + "`n")
        Write-Host "Signed $($exe.Name). Attach $output to the draft before publishing."
    } finally { $signer.Dispose() }
} finally { $key.Dispose() }
