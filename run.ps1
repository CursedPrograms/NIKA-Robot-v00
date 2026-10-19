# Sets up (or reuses) a venv in .\venv, makes sure pygame/requests are
# installed, then launches the NIKA WiFi controller.
$ErrorActionPreference = "Stop"

Set-Location $PSScriptRoot

if (-not (Test-Path "venv")) {
    Write-Host "Creating venv..."
    python -m venv venv
}

& "venv\Scripts\Activate.ps1"
pip install -q -r requirements.txt

python scripts\nika_controller.py @args
