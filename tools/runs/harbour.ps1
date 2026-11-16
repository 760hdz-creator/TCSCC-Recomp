param([string]$Extra="",[int]$Secs=330)
$S="G:\Project Zero\fresh\_run\harbour"  # lad\ = LOCALAPPDATA copy with a profile, s30.ini
$a="240:START,480:DOWN:3,560:A:4,660:A:4,840:A:4,1020:A:4,1400:A:20,1600:A:20,1800:A:20,2000:A:20"
for($t=3000;$t -le 4500;$t+=300){ $a+=",$($t):RY=1:60,$($t+150):RY=-1:40" }
$a+=",4700:B:6,4900:B:6,5000:RY=1:150,5300:RY=-1:150,5600:RY=1:200"
for($t=6500;$t -le 8150;$t+=150){ $a+=",$($t):RX=-0.5:12" }
$env:LOCALAPPDATA="$S\lad"; $env:SCDA_SETTINGS="$S\s30.ini"; $env:SCDA_UNLOCK_ALL="1"
$env:SCDA_START_MINIMIZED="1"; $env:SCDA_WATCHDOG="$Secs"; $env:SCDA_AUTOPRESS=$a
$env:SCDA_AUTOPILOT="6000:-4400,1000,100;-4600,700,60"
foreach($kv in $Extra.Split(';')){ if($kv){ $p=$kv.Split('=',2); Set-Item "env:$($p[0])" $p[1] } }
Copy-Item "G:\Project Zero\fresh\runtime\build\tcscc.exe" "G:\Project Zero\_install\tcscc_fresh.exe" -Force
Set-Location "G:\Project Zero\_install"
$p=Start-Process -FilePath "G:\Project Zero\_install\tcscc_fresh.exe" -ArgumentList '"G:\Project Zero\_install\game\default.img"' -PassThru
$p.WaitForExit(($Secs+30)*1000) | Out-Null; if(!$p.HasExited){$p.Kill()}
