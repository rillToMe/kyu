$root = 'E:\Project\02_Software_Engineering\low-level\kyuzen-os'
$qemu = 'E:\Tools\msys2\ucrt64\bin\qemu-system-x86_64.exe'
$rootf = $root.Replace('\','/')
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Remove-Item "$root\p6-serial.log" -ErrorAction SilentlyContinue
$qargs = @('-cpu','max','-m','1G','-boot','d','-smp','4',
  '-drive','file=disk.img,format=raw,index=0,media=disk',
  '-drive','file=boot_image.iso,media=cdrom,index=2',
  '-nic','user,model=e1000','-display','none',
  '-serial',"file:$root\p6-serial.log",
  '-qmp','tcp:127.0.0.1:4457,server,nowait')
$p = Start-Process -FilePath $qemu -ArgumentList $qargs -WorkingDirectory $root -PassThru -RedirectStandardError "$root\p6-qemu.err" -RedirectStandardOutput "$root\p6-qemu.out"
$client=$null; for($i=0;$i -lt 40;$i++){try{$client=[System.Net.Sockets.TcpClient]::new('127.0.0.1',4457);break}catch{Start-Sleep -Milliseconds 250}}
if(-not $client){Write-Output 'QMP FAILED';exit 1}
$stream=$client.GetStream(); $writer=[System.IO.StreamWriter]::new($stream); $writer.AutoFlush=$true
$writer.WriteLine('{"execute":"qmp_capabilities"}'); Start-Sleep -Milliseconds 300
function Send-Mouse([int]$dx,[int]$dy){$writer.WriteLine('{"execute":"input-send-event","arguments":{"events":[{"type":"rel","data":{"axis":"x","value":'+$dx+'}},{"type":"rel","data":{"axis":"y","value":'+$dy+'}}]}}')}
function Btn([bool]$down){$d=if($down){'true'}else{'false'};$writer.WriteLine('{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":'+$d+',"button":"left"}}]}}')}
function Click{Btn $true;Start-Sleep -Milliseconds 70;Btn $false;Start-Sleep -Milliseconds 70}
function Shot([string]$n){Start-Sleep -Milliseconds 350;$writer.WriteLine('{"execute":"screendump","arguments":{"filename":"'+$rootf+'/'+$n+'"}}');Start-Sleep -Milliseconds 350}

Write-Output 'booting...'; Start-Sleep -Seconds 23
Shot 'p6-init.ppm'
Write-Output 'click widget_demo close button (440,92)'
Send-Mouse -72 -292
Start-Sleep -Milliseconds 250
Click
Start-Sleep -Milliseconds 700
Shot 'p6-closed.ppm'
Write-Output 'done'
try{$writer.WriteLine('{"execute":"quit"}')}catch{}
Start-Sleep -Seconds 1
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Write-Output 'finished'
