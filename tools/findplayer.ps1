# 自動找出玩家座標的 heap 副本並設定寫入監看（逆向輔助用）。
# 原理：stack 上會同時有 (X, 腳底Y, Z, 1) 與 (X, 腳底Y+100, Z, 1)，以此認出玩家；
# 再用 X 精確搜尋 heap 上 Y = 腳底+100 的副本，對它的 Y 設 watch。
param(
    [float[]]$Range = @(9000, 15000, 500, 3000, 9000, 13000),  # xlo xhi ylo yhi zlo zhi
    [switch]$NoWatch
)

$ErrorActionPreference = 'Stop'
$m = Join-Path $PSScriptRoot 'memscan\bin\memscan.exe'
$watchFile = Join-Path $PSScriptRoot '..\..\Mods\Nioh2DropFilter\Nioh2DropFilter.watch.txt'

function Read-Vec([string]$addr) {
    $vals = & $m read $addr 4 | ForEach-Object { [double](($_.Trim() -split '\s+')[3]) }
    return $vals
}

& $m findvec $Range[0] $Range[1] $Range[2] $Range[3] $Range[4] $Range[5] | Out-Null
$stack = & $m list 5000 | Select-Object -Skip 1 | ForEach-Object {
    $p = $_.Trim() -split '\s+'
    $a = [Convert]::ToUInt64($p[1], 16)
    if ($a -lt 0x10000000000) {
        $v = Read-Vec $p[1]
        [pscustomobject]@{ Addr = $p[1]; X = $v[0]; Y = $v[1]; Z = $v[2] }
    }
}

$player = $null
foreach ($s in $stack) {
    if ($stack | Where-Object { $_.X -eq $s.X -and $_.Z -eq $s.Z -and [math]::Abs($_.Y - $s.Y - 100) -lt 0.01 }) {
        $player = $s
        break
    }
}
if (-not $player) { Write-Output '找不到玩家座標（stack 上沒有 Y 與 Y+100 成對的 vec4）'; exit 1 }
Write-Output ("玩家腳底座標：({0}, {1}, {2})" -f $player.X, $player.Y, $player.Z)

& $m find $player.X 0.0005 | Out-Null
$heap = & $m list 100 | Select-Object -Skip 1 | ForEach-Object {
    $p = $_.Trim() -split '\s+'
    if ([Convert]::ToUInt64($p[1], 16) -ge 0x10000000000) {
        $v = Read-Vec $p[1]
        [pscustomobject]@{ Addr = $p[1]; X = $v[0]; Y = $v[1]; Z = $v[2]; W = $v[3] }
    }
}
$heap | Format-Table -AutoSize | Out-String | Write-Output

$target = $heap | Where-Object { [math]::Abs($_.Y - $player.Y - 100) -lt 0.01 } | Select-Object -First 1
if (-not $target) { Write-Output '找不到 Y=腳底+100 的 heap 副本'; exit 1 }
$yAddr = '{0:X}' -f ([Convert]::ToUInt64($target.Addr, 16) + 4)
Write-Output "監看目標（Y=腳底+100 副本的 Y）：$yAddr"

if (-not $NoWatch) {
    [IO.File]::WriteAllText($watchFile, "w $yAddr 4")
    Write-Output '已寫入 watch.txt'
}
