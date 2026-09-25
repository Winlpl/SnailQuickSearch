[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$raw = Get-Content -Raw -Encoding UTF8 "C:\Users\admin\AppData\Local\Temp\hist.json"
$j = $raw | ConvertFrom-Json
$kw = [string][char]0x7D27 + [char]0x7F1A
foreach ($c in $j) {
  if ($c.id -ne 26 -and $c.id -ne 18) { continue }
  Write-Output ("===== conv id=" + $c.id + " title=" + $c.title + " =====")
  $i = 0
  foreach ($m in $c.msgs) {
    $role = $m.r
    if ($role -eq 0) { Write-Output ("[" + $i + "] USER: " + $m.text) }
    elseif ($role -eq 1) {
      $t = $m.text
      if ($t.Length -gt 2600) { $t = $t.Substring(0, 2600) + " ...[TRUNC " + ($t.Length - 2600) + " chars]" }
      Write-Output ("[" + $i + "] ASSISTANT: " + $t)
    }
    elseif ($role -eq 2 -and $m.steps) {
      foreach ($st in $m.steps) {
        $q = $st.query
        if ($q -and $q.Length -gt 300) { $q = $q.Substring(0, 300) + "...[TRUNC]" }
        Write-Output ("[" + $i + "] TOOL name=" + $st.name + " mode=" + $st.mode + " n=" + $st.n + " st=" + $st.st + " query=" + $q)
        if ($st.top) { foreach ($p in $st.top) { Write-Output ("      top: " + $p) } }
        if ($st.err) { Write-Output ("      err: " + $st.err) }
      }
    }
    $i++
  }
}
