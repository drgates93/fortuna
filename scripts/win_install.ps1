$PathToAdd = (Get-Location).Path
$ProfilePath = $PROFILE

$green = "`e[32m"
$yellow = "`e[33m"
$reset = "`e[0m"

Write-Host "${yellow}Adding directory:${reset} $PathToAdd"
Write-Host "${yellow}PowerShell profile:${reset} $ProfilePath"

# Ensure profile exists
if (!(Test-Path -Path $ProfilePath)) {
    New-Item -ItemType File -Path $ProfilePath -Force | Out-Null
}

$ProfileContent = Get-Content $ProfilePath
$LineToAdd = '$env:PATH = "' + $PathToAdd + ';$env:PATH"'

if ($ProfileContent -contains $LineToAdd) {
    Write-Host "${green}Already in PATH.${reset}"
} else {
    Add-Content $ProfilePath $LineToAdd
    Write-Host "${green}Added to PATH. Restart PowerShell or run:${reset}"
    Write-Host "$LineToAdd"
}