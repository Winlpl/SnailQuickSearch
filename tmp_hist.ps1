[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$raw = Get-Content -Raw -Encoding UTF8 "C:\Users\admin\AppData\Local\Temp\hist.json"
$j = $raw | ConvertFrom-Json
$kw = [string][char]0x7D27 + [char]0x7F1A
Write-Output ("total conversations: " + $j.Count)
foreach ($c in $j) {
  $hit = $false
  foreach ($m in $c.msgs) {
    if ($m.text -and $m.text.Contains($kw)) { $hit = $true }
    if ($m.steps) { foreach ($st in $m.steps) { if ($st.query -and $st.query.Contains($kw)) { $hit = $true }; if ($st.top) { foreach ($p in $st.top) { if ($p.Contains($kw)) { $hit = $true } } } } }
  }
  $when = [DateTimeOffset]::FromUnixTimeSeconds([long]$c.t).LocalDateTime.ToString("yyyy-MM-dd HH:mm")
  Write-Output ("conv id=" + $c.id + " t=" + $when + " hit=" + $hit + " title=" + $c.title)
}
