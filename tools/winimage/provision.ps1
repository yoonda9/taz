# Toolchain for the Windows build template, mirroring CI's windows-2025 job.
# tools/winbuild.py runs it over SSH (elevated) with the versions pinned in
# .github/workflows/ci.yml, and reads the final `FACTS {json}` line.
param(
    [Parameter(Mandatory)] [string]$MiseVersion,
    [Parameter(Mandatory)] [string]$CppcheckVersion,
    [Parameter(Mandatory)] [string]$CppcheckSha256
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Step([string]$message) { Write-Output ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $message) }

function Add-Path([string]$dir, [string]$scope) {
    $current = [Environment]::GetEnvironmentVariable('Path', $scope)
    if (($current -split ';') -notcontains $dir) {
        [Environment]::SetEnvironmentVariable('Path', ($current.TrimEnd(';') + ';' + $dir), $scope)
    }
}

function Update-SessionPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
        [Environment]::GetEnvironmentVariable('Path', 'User')
}

# winget's exit code is unreliable for installers that ask for a reboot, so
# each install is checked by what it leaves behind instead.
function Install-Winget([string]$id, [string[]]$extra) {
    $arguments = @('install', '--id', $id, '-e', '--source', 'winget', '--accept-source-agreements',
        '--accept-package-agreements', '--disable-interactivity') + $extra
    & winget @arguments | Select-String -Pattern 'Successfully|already installed|Installer failed|error' |
        ForEach-Object { '    ' + $_.Line.Trim() }
    Write-Output ('    winget exit code: {0}' -f $LASTEXITCODE)
}

function Get-Download([string]$uri, [string]$name) {
    $path = Join-Path $env:TEMP $name
    Invoke-WebRequest -OutFile $path -Uri $uri
    return $path
}

Step 'Defender exclusions'
foreach ($path in "$env:USERPROFILE\.conan2", "$env:LOCALAPPDATA\mise", "$env:LOCALAPPDATA\uv",
    "$env:APPDATA\uv", 'C:\Program Files (x86)\Microsoft Visual Studio', 'C:\Program Files\Microsoft Visual Studio') {
    Add-MpPreference -ExclusionPath $path
}

Step 'Git for Windows'
Install-Winget 'Git.Git' @('--scope', 'machine', '--silent')
Update-SessionPath
& git config --system core.longpaths true
if ($LASTEXITCODE -ne 0) { throw 'git is not installed' }

Step "mise $MiseVersion (checked against the release's SHASUMS256.txt)"
$base = "https://github.com/jdx/mise/releases/download/v$MiseVersion"
$zipName = "mise-v$MiseVersion-windows-x64.zip"
$zip = Get-Download "$base/$zipName" $zipName
$sums = Get-Content (Get-Download "$base/SHASUMS256.txt" 'mise-SHASUMS256.txt')
$want = ($sums | Where-Object { $_ -match "\s\.?/?$([regex]::Escape($zipName))$" } | Select-Object -First 1) -split '\s+' |
    Select-Object -First 1
if (-not $want) { throw "no checksum listed for $zipName" }
if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $want) { throw 'mise checksum mismatch' }
New-Item -ItemType Directory -Force -Path 'C:\tools' | Out-Null
Expand-Archive -Force -Path $zip -DestinationPath 'C:\tools'
$mise = Get-ChildItem -Path 'C:\tools' -Recurse -Filter mise.exe | Select-Object -First 1
Add-Path $mise.DirectoryName 'Machine'
New-Item -ItemType Directory -Force -Path "$env:LOCALAPPDATA\mise\shims" | Out-Null
Add-Path "$env:LOCALAPPDATA\mise\shims" 'User'

Step "cppcheck $CppcheckVersion (same installer and checksum as CI)"
$msiName = "cppcheck-$CppcheckVersion-x64-Setup.msi"
$msi = Get-Download "https://github.com/cppcheck-opensource/cppcheck/releases/download/$CppcheckVersion/$msiName" $msiName
if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $CppcheckSha256) { throw 'cppcheck installer checksum mismatch' }
$install = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart" -Wait -PassThru
if ($install.ExitCode -ne 0) { throw "cppcheck install failed with exit code $($install.ExitCode)" }
Add-Path 'C:\Program Files\Cppcheck' 'Machine'

Step 'Visual Studio Build Tools: C++ workload + AddressSanitizer (about 20 minutes)'
Install-Winget 'Microsoft.VisualStudio.BuildTools' @('--override',
    '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.ASAN')

Step 'verify'
# Native tools write notices to stderr (mise: "new version available"), which
# PowerShell 5.1 turns into terminating errors under 'Stop'. The checks below
# throw on their own.
$ErrorActionPreference = 'Continue'
Update-SessionPath
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json |
    ConvertFrom-Json | Select-Object -First 1
if (-not $vs) { throw 'Visual Studio Build Tools with the C++ workload are not installed' }
$msvcRoot = Join-Path $vs.installationPath 'VC\Tools\MSVC'
$asan = Get-ChildItem -Recurse -Filter 'clang_rt.asan_dynamic-x86_64.dll' $msvcRoot -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $asan) { throw 'the MSVC AddressSanitizer runtime is missing' }
$dlv = (& cscript.exe //nologo "$env:SystemRoot\System32\slmgr.vbs" /dlv) -join "`n"
$evalMinutes = if ($dlv -match 'Timebased activation expiration:\s*(\d+)\s*minute') { [int]$Matches[1] } else { 0 }
$os = Get-CimInstance Win32_OperatingSystem
$facts = [ordered]@{
    hostname = (hostname)
    os = "$($os.Caption) (build $($os.BuildNumber))"
    eval_minutes = $evalMinutes
    vs = "$($vs.displayName) $($vs.catalog.productDisplayVersion)"
    vs_path = $vs.installationPath
    msvc = ((Get-ChildItem $msvcRoot -Name) -join ', ')
    sdk = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs\Windows\v10.0' -ErrorAction SilentlyContinue).ProductVersion
    git = ((& git --version) -replace '^git version ', '')
    mise = ((& $mise.FullName --version 2>$null | Select-Object -Last 1) -split ' ')[0]
    cppcheck = ((& 'C:\Program Files\Cppcheck\cppcheck.exe' --version) -replace '^Cppcheck ', '')
}
Write-Output ('FACTS ' + ($facts | ConvertTo-Json -Compress))
Step 'done'
