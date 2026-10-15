# Packages the NativeActivity library into a signed APK: aapt2 links the
# manifest, the stripped library is stored uncompressed, zipalign and
# apksigner finish it. Runs in PowerShell 7 on Windows, Linux and macOS.
param(
    [Parameter(Mandatory = $true)][string]$Aapt2,
    [Parameter(Mandatory = $true)][string]$Zipalign,
    [Parameter(Mandatory = $true)][string]$Apksigner,
    [Parameter(Mandatory = $true)][string]$FrameworkJar,
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$NativeLibrary,
    [string]$Strip = '',
    [Parameter(Mandatory = $true)][string]$Abi,
    [Parameter(Mandatory = $true)][string]$Keystore,
    [Parameter(Mandatory = $true)][string]$KeystorePassword,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][int]$MinSdk,
    [Parameter(Mandatory = $true)][int]$TargetSdk,
    [Parameter(Mandatory = $true)][string]$VersionName,
    [Parameter(Mandatory = $true)][int]$VersionCode
)

$ErrorActionPreference = 'Stop'
foreach ($file in @($Aapt2, $Zipalign, $Apksigner, $FrameworkJar, $Manifest, $NativeLibrary, $Keystore)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Android packaging input does not exist: $file"
    }
}

$directory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$unsigned = Join-Path $directory 'unsigned.apk'
$aligned = Join-Path $directory 'aligned.apk'
$library = Join-Path $directory (Split-Path -Leaf $NativeLibrary)
Remove-Item -LiteralPath $unsigned, $aligned, $library, $Output -Force -ErrorAction SilentlyContinue

# Debug information stays in the build; the device gets the stripped copy.
Copy-Item -LiteralPath $NativeLibrary -Destination $library
if ($Strip) {
    & $Strip --strip-unneeded $library
    if ($LASTEXITCODE -ne 0) { throw "strip failed with exit code $LASTEXITCODE" }
}

& $Aapt2 link --manifest $Manifest -I $FrameworkJar --min-sdk-version $MinSdk `
    --target-sdk-version $TargetSdk --version-code $VersionCode --version-name $VersionName `
    -o $unsigned
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed with exit code $LASTEXITCODE" }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($unsigned, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    # Uncompressed and page-aligned below, the library is mapped straight
    # from the APK.
    $entry = $archive.CreateEntry("lib/$Abi/$(Split-Path -Leaf $library)",
        [System.IO.Compression.CompressionLevel]::NoCompression)
    $source = [System.IO.File]::OpenRead($library)
    try {
        $stream = $entry.Open()
        try { $source.CopyTo($stream) } finally { $stream.Dispose() }
    } finally { $source.Dispose() }
} finally { $archive.Dispose() }

& $Zipalign -f -P 16 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed with exit code $LASTEXITCODE" }
# apksigner loads its bundled Conscrypt library; allow that native access
# explicitly on current JDKs.
$previousJavaOptions = [Environment]::GetEnvironmentVariable('JDK_JAVA_OPTIONS', 'Process')
try {
    $env:JDK_JAVA_OPTIONS = "$previousJavaOptions --enable-native-access=ALL-UNNAMED".Trim()
    & $Apksigner sign --ks $Keystore --ks-pass "pass:$KeystorePassword" --out $Output $aligned
    if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed with exit code $LASTEXITCODE" }
    & $Apksigner verify $Output
    if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed with exit code $LASTEXITCODE" }
} finally {
    [Environment]::SetEnvironmentVariable('JDK_JAVA_OPTIONS', $previousJavaOptions, 'Process')
}
Remove-Item -LiteralPath $unsigned, $aligned, $library -Force
Write-Host "Created $Output"
