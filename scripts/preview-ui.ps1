[CmdletBinding()]
param(
    [ValidateRange(620, 2400)][int]$Width = 1100,
    [ValidateRange(480, 1800)][int]$Height = 780,
    [ValidateSet(0, 96, 120, 144, 168, 192)][int]$Dpi = 0,
    [ValidateSet('normal', 'empty', 'long')][string]$Scene = 'normal'
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = Join-Path $repoRoot 'build\Release\iPad互联.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    $executable = Join-Path $repoRoot 'dist\iPad互联-0.3.0-preview-portable.exe'
}
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw 'Build with scripts/build.ps1 or use the packaged 0.3.0-preview portable EXE.'
}
$settings = @{
    IPAD_UI_PREVIEW_WIDTH = [string]$Width
    IPAD_UI_PREVIEW_HEIGHT = [string]$Height
    IPAD_UI_PREVIEW_DPI = [string]$Dpi
    IPAD_UI_PREVIEW_SCENE = $Scene
}
$previous = @{}
try {
    foreach ($name in $settings.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $settings[$name], 'Process')
    }
    # An interactive, isolated UI preview: no device connections or settings writes.
    Start-Process -FilePath $executable -ArgumentList '--preview-ui' -WindowStyle Normal
} finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
