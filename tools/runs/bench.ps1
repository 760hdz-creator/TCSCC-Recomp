param([string]$Extra="",[int]$Secs=200,[string]$Tag="bench")
# Renderer CPU benchmark: Deniable Ops Hunter Banya, FrameRate 240 (uncapped), windowed 1280x720, minimized.
# [perf] lines every 30 s; the summary averages those after 90 s (in the level). Log -> _run\bench\<Tag>.log
$S="G:\Project Zero\fresh\_run\harbour"
$B="G:\Project Zero\fresh\_run\bench"; New-Item -ItemType Directory -Force $B | Out-Null
"[Display]`nMode=windowed`nWidth=1280`nHeight=720`nScale=1`nFrameRate=240`nVSync=0`n[Profile]`nName=P1" | Set-Content -Encoding ascii "$B\bench.ini"
$env:LOCALAPPDATA="$S\lad"; $env:SCDA_SETTINGS="$B\bench.ini"; $env:SCDA_START_MINIMIZED="1"; $env:SCDA_ONLINE="0"
$env:SCDA_WATCHDOG="$Secs"; $env:SCDA_AUTOPRESS="@G:\Project Zero\fresh\tools\autopress\deniable_banya.txt"; $env:SCDA_AUTOPRESS_WALL="1"
foreach($kv in $Extra.Split(';')){ if($kv){ $p=$kv.Split('=',2); Set-Item "env:$($p[0])" $p[1] } }
Get-Process tcscc_fresh -ErrorAction SilentlyContinue | % { $_.Kill(); $_.WaitForExit() }; Start-Sleep 2
Copy-Item "G:\Project Zero\fresh\runtime\build\tcscc.exe" "G:\Project Zero\_install\tcscc_fresh.exe" -Force
Set-Location "G:\Project Zero\_install"
$p=Start-Process -FilePath "G:\Project Zero\_install\tcscc_fresh.exe" -ArgumentList '"G:\Project Zero\_install\game\default.img"' -PassThru
$p.WaitForExit(($Secs+30)*1000) | Out-Null; if(!$p.HasExited){$p.Kill()}; $p.WaitForExit()
Copy-Item tcscc.log "$B\$Tag.log" -Force
$fps=@(); $draw=@(); $bind=@(); $lines=Get-Content tcscc.log
$i=0; foreach($l in $lines){
  if($l -match '^\[perf\] ([\d.]+) fps'){ $i++; if($i -ge 3){ $fps+=[double]$matches[1] } }
  if($i -ge 3 -and $l -match 'renderer ms/frame: draws ([\d.]+) \(bind ([\d.]+), vertex upload ([\d.]+)'){ $draw+=$l }
  if($i -ge 3 -and $l -match 'bind ms/frame'){ $bind+=$l }
}
"$Tag fps " + (($fps | Measure-Object -Average).Average) + " samples " + $fps.Count
$draw | Select-Object -Last 2; $bind | Select-Object -Last 2
