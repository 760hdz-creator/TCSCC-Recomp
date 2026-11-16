$S="G:\Project Zero\fresh\_run\harbour"  # lad\ = LOCALAPPDATA copy with a profile, s30.ini
Copy-Item "G:\Project Zero\fresh\runtime\build\tcscc.exe" "G:\Project Zero\_install\tcscc_fresh.exe" -Force
New-Item -ItemType Directory -Force "$S\sweep" | Out-Null
Set-Location "G:\Project Zero\_install"
for($k=0;$k -lt 11;$k++){
  $a="240:START,480:DOWN:3,560:A:4"; $t=720
  for($i=0;$i -lt $k;$i++){ $a+=",$($t):DOWN:3"; $t+=60 }
  $a+=",$($t+100):A:4,$($t+280):A:4,$($t+460):A:4"
  $env:LOCALAPPDATA="$S\lad"; $env:SCDA_SETTINGS="$S\s30.ini"; $env:SCDA_UNLOCK_ALL="1"; $env:SCDA_START_MINIMIZED="1"
  $env:SCDA_WATCHDOG="160"; $env:SCDA_AUTOPRESS=$a; $env:SCDA_DUMP_FRAME="4300"
  $p=Start-Process -FilePath "G:\Project Zero\_install\tcscc_fresh.exe" -ArgumentList '"G:\Project Zero\_install\game\default.img"' -PassThru
  $p.WaitForExit(190000) | Out-Null; if(!$p.HasExited){$p.Kill()}
  Copy-Item tcscc.log "$S\sweep\ch$k.log" -Force
  if(Test-Path scda_frame_4300.bmp){ Move-Item scda_frame_4300.bmp "$S\sweep\ch$k.bmp" -Force }
  $maps=(Select-String -Path tcscc.log -Pattern "\d\d_[A-Za-z_-]+\.umd" -AllMatches | % { $_.Matches.Value } | Select-Object -Unique) -join ' '
  $fail=(Select-String -Path tcscc.log -Pattern "compile failed|translate failed").Count
  $fatal=(Select-String -Path tcscc.log -Pattern "fatal|exception|crash" ).Count
  "ch$k maps [$maps] shaderfail $fail fatal $fatal" | Tee-Object -Append "$S\sweep\summary.txt"
}
