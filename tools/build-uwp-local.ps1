# Local build of the UWP (Xbox) package, so a code change reaches the Xbox in minutes instead of a ~25 minute CI run.
# First run: checks out submodules, bootstraps vcpkg and builds all dependencies (an hour or more). Later runs only
# recompile what changed. The package is signed with a local self-signed certificate (CN=CemuUWP, like CI) made with
# openssl, so nothing is added to the Windows certificate store.
# Output: build-uwp-local\AppPackages\...\CemuBin_<version>_x64.msix
param([string]$Version = "0.1.900.0")
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Set-Location $repo
$log = "$repo\build-uwp-local.log"
function Log($text) { $text | Out-File $log -Append -Encoding utf8; $text }
# native tools write progress to stderr, which Windows PowerShell turns into errors under 'Stop': judge by exit code only
function Step($name, [scriptblock]$block) { Log "== $name $(Get-Date -Format T)"; $ErrorActionPreference = 'Continue'; & $block 2>&1 | ForEach-Object { "$_" } | Out-File $log -Append -Encoding utf8; if ($LASTEXITCODE) { throw "$name failed ($LASTEXITCODE), see $log" } }

$vs = "C:\Program Files\Microsoft Visual Studio\18\Community"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$env:VCPKG_ROOT = "$repo\dependencies\vcpkg"

if (-not (Test-Path "$repo\dependencies\vcpkg\bootstrap-vcpkg.bat")) { Step 'submodules' { git submodule update --init --recursive } }
if (-not (Test-Path "$repo\dependencies\vcpkg\vcpkg.exe")) { Step 'bootstrap vcpkg' { cmd /c "$repo\dependencies\vcpkg\bootstrap-vcpkg.bat" -disableMetrics } }

# signing certificate, made once
$pfx = "$repo\build-uwp-local\CemuUWP_local.pfx"
New-Item -ItemType Directory -Force "$repo\build-uwp-local" | Out-Null
if (-not (Test-Path $pfx)) {
	$openssl = "C:\Program Files\Git\usr\bin\openssl.exe"
	Step 'certificate' {
		& $openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=CemuUWP" -keyout "$repo\build-uwp-local\key.pem" -out "$repo\build-uwp-local\cert.pem" -addext "extendedKeyUsage=codeSigning" -addext "basicConstraints=CA:FALSE"
		& $openssl pkcs12 -export -inkey "$repo\build-uwp-local\key.pem" -in "$repo\build-uwp-local\cert.pem" -out $pfx -passout pass:cemu
	}
}

# package version (CI stamps the run number; local builds use a high number so they update over CI builds)
$manifest = "$repo\src\gui\host\uwp\Package.appxmanifest"
$orig = Get-Content $manifest -Raw
try {
	(($orig -replace 'Version="0\.1\.\d+\.0"', "Version=`"$Version`"")) | Set-Content $manifest -NoNewline -Encoding utf8
	if (-not (Test-Path "$repo\build-uwp-local\CemuBin.sln") -and -not (Test-Path "$repo\build-uwp-local\Cemu.sln")) {
		Step 'cmake configure' { & $cmake -S $repo -B "$repo\build-uwp-local" -G "Visual Studio 18 2026" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore "-DCMAKE_SYSTEM_VERSION=10.0" }
	}
	Step 'build and package' { & $cmake --build "$repo\build-uwp-local" --config Release --target CemuBin -- -m -p:GenerateAppxPackageOnBuild=true -p:AppxBundle=Never -p:UapAppxPackageBuildMode=SideloadOnly -p:AppxPackageSigningEnabled=true "-p:PackageCertificateKeyFile=$pfx" -p:PackageCertificatePassword=cemu "-p:AppxPackageDir=$repo\build-uwp-local\AppPackages\\" }
}
finally { git -C $repo checkout -- src/gui/host/uwp/Package.appxmanifest } # never leave the stamped version for a commit
Get-ChildItem "$repo\build-uwp-local\AppPackages" -Recurse -Filter *.msix | Sort-Object LastWriteTime | Select-Object -Last 1 | ForEach-Object { Log "package: $($_.FullName)" }
