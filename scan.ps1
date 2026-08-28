# Загрузка builds\rdpkey.exe в VirusTotal, вердикт движка Microsoft.
# Windows PowerShell 5.1 + curl.exe. Ключ читается из virustotal.api.key.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $root 'builds\rdpkey.exe'
$key  = (Get-Content (Join-Path $root 'virustotal.api.key') -Raw).Trim()

if (-not (Test-Path $exe)) { Write-Error "Нет $exe — сначала собери build.cmd"; exit 1 }

Write-Host "Загрузка $exe в VirusTotal..."
$up = curl.exe -s -X POST 'https://www.virustotal.com/api/v3/files' `
    -H "x-apikey: $key" -F "file=@$exe" | ConvertFrom-Json
$id = $up.data.id
if (-not $id) { Write-Error "VT не вернул id: $($up | ConvertTo-Json -Depth 6)"; exit 1 }
Write-Host "analysis id: $id"

$analysis = $null
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 15
    $analysis = curl.exe -s "https://www.virustotal.com/api/v3/analyses/$id" `
        -H "x-apikey: $key" | ConvertFrom-Json
    $status = $analysis.data.attributes.status
    Write-Host ("[{0}] status={1}" -f $i, $status)
    if ($status -eq 'completed') { break }
}

$attr  = $analysis.data.attributes
$stats = $attr.stats
$ms    = $attr.results.Microsoft

Write-Host ""
Write-Host "===== stats ====="
$stats | Format-List
Write-Host "===== Microsoft ====="
if ($ms) {
    Write-Host ("category = {0}" -f $ms.category)
    Write-Host ("result   = {0}" -f $ms.result)
} else {
    Write-Host "движок Microsoft отсутствует в отчёте"
}

Write-Host ""
if ($ms -and $ms.category -eq 'undetected' -and [int]$stats.malicious -eq 0) {
    Write-Host "ПРИЁМКА: Microsoft undetected, malicious=0" -ForegroundColor Green
} elseif ($ms -and $ms.category -eq 'undetected') {
    Write-Host ("Microsoft undetected, но malicious={0} на других движках" -f $stats.malicious) -ForegroundColor Yellow
} else {
    Write-Host "ПРОВАЛ: Microsoft флагует — нужны итерации" -ForegroundColor Red
}
