<#
.SYNOPSIS
Per-user installer for the On My Disk P2P node client (embedded.pro build).

.DESCRIPTION
Installs a static onmydisk.exe (plus omd-tunnel.exe) into %LOCALAPPDATA%\OnMyDisk
and registers autostart for the current user. No admin rights required.

With -WithAi it additionally installs the local OnMyChat assistant (Python
FastAPI + Ollama) listening on http://localhost:4000, which is the default
AiServer base URL of the node - no client settings change needed.

Run from PowerShell:

    # Typical laptop / PC without GPU (Node + Agentic AI):
    & ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Agentic

    # PC with NVIDIA GPU (Node + Local GPU AI + Ollama):
    & ([scriptblock]::Create((irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1))) -Gpu

    # Storage-only node:
    irm https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1 | iex

or for install/uninstall options:

    curl.exe -fsSL https://forge.onmydisk.net/repo/Windows/install-onmydisk.ps1 -o install-onmydisk.ps1
    powershell -ExecutionPolicy Bypass -File .\install-onmydisk.ps1                # node only
    powershell -ExecutionPolicy Bypass -File .\install-onmydisk.ps1 -WithAi        # node + OnMyChat
    powershell -ExecutionPolicy Bypass -File .\install-onmydisk.ps1 -Uninstall     # uninstall
#>
param(
    [switch]$Uninstall,
    [switch]$WithAi,
    [switch]$Gpu,     # install Node + OnMyChat in full local GPU mode (CUDA PyTorch + local Ollama)
    [switch]$AiCpu,   # install CPU-only torch (much smaller) instead of the default CUDA build
    [switch]$NoOllama,
    [switch]$Agentic, # configure OnMyChat in P2P agentic mode (implies -WithAi -AiCpu -NoOllama)
    [string]$InferenceNode = "auto",
    [string]$RepoBase = "https://forge.onmydisk.net/repo/Windows"
)
if ($Agentic) { $WithAi = $true; $AiCpu = $true; $NoOllama = $true }
if ($Gpu)     { $WithAi = $true }

$ErrorActionPreference = "Stop"
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$InstallDir = Join-Path $env:LOCALAPPDATA "OnMyDisk"
$BinDir     = Join-Path $InstallDir "bin"
$EtcDir     = Join-Path $InstallDir "etc"
$NodeExe    = Join-Path $BinDir "onmydisk.exe"
$TunnelExe  = Join-Path $BinDir "omd-tunnel.exe"
$Conf       = Join-Path $EtcDir "onmydisk.conf"
$RunKeyPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$RunValue   = "OnMyDiskNode"

function Stop-Node {
    Get-Process onmydisk -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

$StartMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\On My Disk"
$DesktopLnk   = Join-Path ([Environment]::GetFolderPath("Desktop")) "On My Disk Node.lnk"

if ($Uninstall) {
    Stop-Node
    # stop OnMyChat if installed
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*uvicorn api:app*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Remove-ItemProperty -Path $RunKeyPath -Name $RunValue -ErrorAction SilentlyContinue
    Remove-ItemProperty -Path $RunKeyPath -Name "OnMyChatAI" -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName "OnMyDiskNode" -Confirm:$false -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName "OnMyChatAI" -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $DesktopLnk -Force -ErrorAction SilentlyContinue
    Remove-Item $StartMenuDir -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $env:LOCALAPPDATA "OnMyChat") -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $InstallDir) {
        Remove-Item $InstallDir -Recurse -Force
    }
    Write-Host "On My Disk node was uninstalled ($InstallDir removed)."
    exit 0
}

# --- install ---------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $BinDir, $EtcDir | Out-Null

# Stop any running node (including legacy Program Files desktop client that shares the IPC pipe) BEFORE overwriting binaries
Stop-Node
Remove-ItemProperty -Path $RunKeyPath -Name "onmydisk" -ErrorAction SilentlyContinue

$downloads = @(
    @{ Url = "$RepoBase/onmydisk-node-latest-x64.exe"; Out = $NodeExe }
    @{ Url = "$RepoBase/omd-tunnel-x64.exe";           Out = $TunnelExe }
)
if (-not (Test-Path $Conf)) {
    $downloads += @{ Url = "$RepoBase/onmydisk.conf"; Out = $Conf }
}

foreach ($d in $downloads) {
    Write-Host "Downloading $($d.Url) ..."
    Invoke-WebRequest -Uri $d.Url -OutFile $d.Out -UseBasicParsing
}

# Migrate existing 3.0 node configs from legacy gateway port 9000 -> direct gateway port 9010
foreach ($cfgPath in @($Conf, "$Conf.user")) {
    if (Test-Path $cfgPath) {
        $rawCfg = [System.IO.File]::ReadAllText($cfgPath)
        $newCfg = $rawCfg `
            -replace '(?m)^host=onmydisk\.net\r?$', 'host=direct.onmydisk.net' `
            -replace '(?m)^restUrl=https://onmydisk\.net/rest\r?$', 'restUrl=https://direct.onmydisk.net/rest' `
            -replace '(?m)^wsPort=9988\r?$', 'wsPort=9989' `
            -replace '(?m)^port=9000\r?$', 'port=9010'
        if ($newCfg -ne $rawCfg) {
            [System.IO.File]::WriteAllText($cfgPath, $newCfg, (New-Object System.Text.UTF8Encoding($false)))
        }
    }
}

# strip Mark-of-the-Web so the statically signed-free exe is not flagged by SmartScreen on first run
Unblock-File $NodeExe -ErrorAction SilentlyContinue
Unblock-File $TunnelExe -ErrorAction SilentlyContinue

# Create silent background launchers (no console window, no PowerShell needed)
$StartVbs = Join-Path $InstallDir "start-node.vbs"
$OpenVbs  = Join-Path $InstallDir "open-node.vbs"
$StopVbs  = Join-Path $InstallDir "stop-node.vbs"

$startVbsCode = @"
Function IsHttpTimedOut()
    On Error Resume Next
    Dim req
    Set req = CreateObject("MSXML2.ServerXMLHTTP.6.0")
    If Err.Number <> 0 Then
        Err.Clear
        IsHttpTimedOut = False
        Exit Function
    End If
    req.setTimeouts 3000, 3000, 3000, 4000
    req.Open "GET", "http://127.0.0.1:8081/", False
    req.Send
    If Err.Number = -2147012894 Then
        IsHttpTimedOut = True
    Else
        IsHttpTimedOut = False
    End If
    Err.Clear
End Function

Set sh = CreateObject("WScript.Shell")
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name = 'onmydisk.exe'")
If procs.Count = 0 Then
    sh.CurrentDirectory = "$BinDir"
    sh.Run Chr(34) & "$NodeExe" & Chr(34), 0, False
ElseIf IsHttpTimedOut() Then
    WScript.Sleep 3000
    If IsHttpTimedOut() Then
        sh.Run "taskkill.exe /F /IM onmydisk.exe", 0, True
        WScript.Sleep 1000
        sh.CurrentDirectory = "$BinDir"
        sh.Run Chr(34) & "$NodeExe" & Chr(34), 0, False
    End If
End If
"@
Set-Content -Path $StartVbs -Value $startVbsCode -Encoding ASCII

$openVbsCode = @"
Set sh = CreateObject("WScript.Shell")
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT ProcessId FROM Win32_Process WHERE Name = 'onmydisk.exe'")
If procs.Count = 0 Then
    sh.CurrentDirectory = "$BinDir"
    sh.Run Chr(34) & "$NodeExe" & Chr(34), 0, False
    WScript.Sleep 1800
End If
sh.Run "http://localhost:8081", 1, False
"@
Set-Content -Path $OpenVbs -Value $openVbsCode -Encoding ASCII

$stopVbsCode = @"
Set sh = CreateObject("WScript.Shell")
sh.Run "taskkill.exe /F /IM onmydisk.exe", 0, True
"@
Set-Content -Path $StopVbs -Value $stopVbsCode -Encoding ASCII

# 1) Autostart via HKCU Run (using explicit wscript.exe so Windows 11 never prompts for .vbs handler)
$WscriptExe = Join-Path $env:SystemRoot "System32\wscript.exe"
Set-ItemProperty -Path $RunKeyPath -Name $RunValue -Value ("`"$WscriptExe`" //B //Nologo `"$StartVbs`"")

# 2) Autostart & watchdog via Windows Task Scheduler (logon + 5-min healthcheck + wake from sleep/Modern Standby)
try {
    $taskAction    = New-ScheduledTaskAction -Execute $WscriptExe -Argument "//B //Nologo `"$StartVbs`"" -WorkingDirectory $BinDir
    $logonTrigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $watchTrigger  = New-ScheduledTaskTrigger -Once -At ((Get-Date).AddMinutes(1)) `
                         -RepetitionInterval (New-TimeSpan -Minutes 5) `
                         -RepetitionDuration (New-TimeSpan -Days 3650)
    $triggers      = @($logonTrigger, $watchTrigger)
    try {
        $cimTriggerClass = Get-CimClass -Namespace Root/Microsoft/Windows/TaskScheduler -ClassName MSFT_TaskEventTrigger -ErrorAction Stop
        $wakeTrigger = New-CimInstance -CimClass $cimTriggerClass -ClientOnly
        $wakeTrigger.Enabled = $true
        $wakeTrigger.Delay = "PT8S"
        $wakeTrigger.Subscription = "<QueryList><Query Id='0' Path='System'><Select Path='System'>*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]] or *[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=507]]</Select></Query></QueryList>"
        $triggers += $wakeTrigger
    } catch {}
    $taskSettings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                         -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName "OnMyDiskNode" -Action $taskAction -Trigger $triggers `
        -Settings $taskSettings -Description "On My Disk P2P Node background daemon and watchdog" -Force | Out-Null
} catch {
    Write-Warning "Could not register Scheduled Task OnMyDiskNode (HKCU Run fallback active): $_"
}

# 3) Create Desktop & Start Menu shortcuts so the user can start/open the node with a normal click
try {
    New-Item -ItemType Directory -Force -Path $StartMenuDir | Out-Null
    $wsh = New-Object -ComObject WScript.Shell

    foreach ($lnkPath in @($DesktopLnk, (Join-Path $StartMenuDir "On My Disk Node.lnk"))) {
        $sc = $wsh.CreateShortcut($lnkPath)
        $sc.TargetPath = $WscriptExe
        $sc.Arguments = "//B //Nologo `"$OpenVbs`""
        $sc.WorkingDirectory = $BinDir
        $sc.IconLocation = "$NodeExe,0"
        $sc.Description = "Start On My Disk Node and open control panel (http://localhost:8081)"
        $sc.Save()
    }

    $stopSc = $wsh.CreateShortcut((Join-Path $StartMenuDir "Stop On My Disk Node.lnk"))
    $stopSc.TargetPath = $WscriptExe
    $stopSc.Arguments = "//B //Nologo `"$StopVbs`""
    $stopSc.WorkingDirectory = $BinDir
    $stopSc.IconLocation = "$NodeExe,0"
    $stopSc.Description = "Stop On My Disk Node background service"
    $stopSc.Save()
} catch {
    Write-Warning "Could not create shortcuts: $_"
}

# allow inbound HTTP on :8081 for the claim page; silent no-op without admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($isAdmin) {
    netsh advfirewall firewall delete rule name="On My Disk node" *> $null
    netsh advfirewall firewall add rule name="On My Disk node" dir=in action=allow `
        program=$NodeExe enable=yes profile=any | Out-Null
    # Clean up stale 2021 ProgramData Start Menu shortcut if legacy exe is no longer used
    $legacyStartDir = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\On My Disk"
    if (Test-Path (Join-Path $legacyStartDir "On My Disk.lnk")) {
        Remove-Item $legacyStartDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Starting the node ..."
try { Start-ScheduledTask -TaskName "OnMyDiskNode" -ErrorAction Stop } catch { & $WscriptExe //B //Nologo "$StartVbs" }

Start-Sleep -Seconds 2
Write-Host ""
Write-Host "On My Disk node installed at $InstallDir"
Write-Host "Shortcuts created on Desktop and Start Menu -> On My Disk -> On My Disk Node."
Write-Host "Open http://localhost:8081 in your browser and claim this device to your account."
Write-Host "TCP forwarding from outside: omd-tunnel.exe --listen <port> <linkID> <targetPort>"

# --- OnMyChat (local AI assistant) ------------------------------------------
function Install-Ai {
    $AiDir  = Join-Path $env:LOCALAPPDATA "OnMyChat"
    $TmpDir = Join-Path $env:TEMP "onmychat-install"
    $PyVersion = "3.12.10"
    $PyDir = Join-Path $InstallDir "python"
    $Py = Join-Path $PyDir "python.exe"
    New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

    function Download([string]$Url, [string]$Out) {
        if (Test-Path $Out) { Remove-Item $Out -Force }
        Invoke-WebRequest -Uri $Url -OutFile $Out -UseBasicParsing
    }

    # Run a native tool with PS 5.1 safety: pip/python write warnings to stderr,
    # and stderr + $ErrorActionPreference=Stop is fatal. Start-Process isolates it
    # and tees output into %TEMP%\onmychat-install for later inspection.
    function Run-Step([string]$FilePath, [string]$Arguments) {
        $log = Join-Path $TmpDir ("step-" + [IO.Path]::GetFileNameWithoutExtension($FilePath) + ".log")
        $pr = Start-Process -FilePath $FilePath -ArgumentList $Arguments `
                -WindowStyle Hidden -Wait -PassThru -ErrorAction Stop `
                -RedirectStandardOutput $log -RedirectStandardError ($log + ".err")
        return $pr.ExitCode
    }

    # 1. per-user portable Python (embeddable distribution, no admin needed)
    if (-not (Test-Path $Py)) {
        Write-Host "Installing portable Python $PyVersion ..."
        New-Item -ItemType Directory -Force -Path $PyDir | Out-Null
        Download "https://www.python.org/ftp/python/$PyVersion/python-$PyVersion-embed-amd64.zip" (Join-Path $TmpDir "python-embed.zip")
        Expand-Archive (Join-Path $TmpDir "python-embed.zip") -DestinationPath $PyDir -Force
        # enable site-packages in the embeddable distribution
        $pth = Get-ChildItem $PyDir -Filter "*._pth" | Select-Object -First 1
        if ($pth) {
            (Get-Content $pth.FullName) -replace "#import site","import site" | Set-Content $pth.FullName
        }
        Download "https://bootstrap.pypa.io/get-pip.py" (Join-Path $TmpDir "get-pip.py")
        Run-Step $Py ('"' + (Join-Path $TmpDir "get-pip.py") + '" --quiet') | Out-Null
        Unblock-File $Py -ErrorAction SilentlyContinue
    } else {
        Write-Host "Portable Python already present at $PyDir"
    }

    # Stop any running OnMyChat instance before updating files or restarting
    Get-CimInstance Win32_Process -Filter "Name='python.exe' OR Name='pythonw.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -like "*uvicorn api:app*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

    # 2. onmychat application (safe to re-extract on upgrade: user dirs & config.ini are excluded from zip)
    New-Item -ItemType Directory -Force -Path $AiDir | Out-Null
    Write-Host "Downloading OnMyChat app ..."
    Download "$RepoBase/onmychat-latest.zip" (Join-Path $TmpDir "onmychat.zip")
    Expand-Archive (Join-Path $TmpDir "onmychat.zip") -DestinationPath $AiDir -Force
    # user dirs
    foreach ($sub in "user_data","memory_index","history","avatars","data") {
        New-Item -ItemType Directory -Force -Path (Join-Path $AiDir $sub) | Out-Null
    }
    # config from example on first install only (preserve existing config.ini on upgrade)
    $CfgPath = Join-Path $AiDir "config.ini"
    if (-not (Test-Path $CfgPath)) {
        $cfg = Get-Content (Join-Path $AiDir "config.example.ini") -Raw
        $cfg = $cfg -replace "APP_ROOT_DIR\s*=\s*/opt/onmychat", ("APP_ROOT_DIR = " + ($AiDir -replace "\\","/"))
        if ($Agentic) {
            $cfg = $cfg -replace "(?m)^#?AI_PROVIDER\s*=.*$", "AI_PROVIDER = agentic"
            $cfg = $cfg -replace "(?m)^#?INFERENCE_NODE\s*=.*$", ("INFERENCE_NODE = " + $InferenceNode)
            $cfg = $cfg -replace "(?m)^#?INFERENCE_BIND_OLLAMA\s*=.*$", "INFERENCE_BIND_OLLAMA = 21434"
            $cfg = $cfg -replace "(?m)^#?INFERENCE_BIND_COMFY\s*=.*$", "INFERENCE_BIND_COMFY = 28188"
        }
        [IO.File]::WriteAllText($CfgPath, $cfg, (New-Object System.Text.UTF8Encoding $false))
    }

    # 3. python dependencies (this step is long - big wheels like torch/chromadb)
    Write-Host "Installing python dependencies (this may take a long time) ..."
    if ($AiCpu) {
        $rc = Run-Step $Py "-m pip install --quiet torch --index-url https://download.pytorch.org/whl/cpu"
        if ($rc -ne 0) { throw "pip install (cpu torch) failed with exit code $rc" }
    }
    $rc = Run-Step $Py ('-m pip install --quiet -r "' + (Join-Path $AiDir "requirements.txt") + '"')
    if ($rc -ne 0) { throw "pip install failed with exit code $rc - see %TEMP%\onmychat-install pip logs" }
    # crawler support (non-fatal if it fails)
    try { Run-Step $Py "-m playwright install chromium" | Out-Null } catch { }

    # 3b. pre-loaded knowledge base (chromadb) & embedding model cache
    $UnifiedIndexDir = Join-Path $AiDir "memory_index\unified_index"
    $ChromaDbFile = Join-Path $UnifiedIndexDir "chroma.sqlite3"
    if (-not (Test-Path $ChromaDbFile)) {
        Write-Host "Downloading pre-indexed OMD knowledge base ..."
        New-Item -ItemType Directory -Force -Path $UnifiedIndexDir | Out-Null
        $ChromaTar = Join-Path $TmpDir "chroma_omd.tar.gz"
        try {
            Download "https://forge.onmydisk.net/repo/OnMyChat/chroma_omd.tar.gz" $ChromaTar
            Run-Step "tar.exe" ('-xzf "' + $ChromaTar + '" -C "' + $UnifiedIndexDir + '"') | Out-Null
            if (Test-Path $ChromaTar) { Remove-Item $ChromaTar -Force }
        } catch {
            Write-Warning "Failed to download or unpack ChromaDB knowledge base: $_"
        }
    }

    $HfHubDir = Join-Path $env:USERPROFILE ".cache\huggingface\hub"
    $HfModelDir = Join-Path $HfHubDir "models--sentence-transformers--paraphrase-multilingual-MiniLM-L12-v2"
    if (-not (Test-Path $HfModelDir)) {
        Write-Host "Downloading pre-cached embedding model ..."
        New-Item -ItemType Directory -Force -Path $HfHubDir | Out-Null
        $ModelTar = Join-Path $TmpDir "sentence_model.tar.gz"
        try {
            Download "https://forge.onmydisk.net/repo/OnMyChat/sentence_model.tar.gz" $ModelTar
            Run-Step "tar.exe" ('-xzf "' + $ModelTar + '" -C "' + $HfHubDir + '"') | Out-Null
            if (Test-Path $ModelTar) { Remove-Item $ModelTar -Force }
        } catch {
            Write-Warning "Failed to download or unpack embedding model: $_"
        }
    }

    # 4. launcher (hidden window via wscript + .cmd wrapper) + HKCU Run / ScheduledTask autostart
    $CmdFile = Join-Path $AiDir "start-onmychat.cmd"
    $cmd  = "@echo off`r`n"
    $cmd += 'cd /d "%~dp0"' + "`r`n"
    $cmd += '"' + $Py + '" -X utf8 -m uvicorn api:app --host 127.0.0.1 --port 4000 >> "%~dp0onmychat.log" 2>&1' + "`r`n"
    Set-Content $CmdFile $cmd -Encoding ASCII

    $Starter = Join-Path $AiDir "start-onmychat.vbs"
    $vbs  = 'Set sh = CreateObject("Wscript.Shell")' + "`r`n"
    $vbs += 'sh.CurrentDirectory = "' + $AiDir + '"' + "`r`n"
    $vbs += 'sh.Run """' + $CmdFile + '""", 0, False' + "`r`n"
    Set-Content $Starter $vbs -Encoding ASCII
    $WscriptExe = Join-Path $env:SystemRoot "System32\wscript.exe"
    Set-ItemProperty -Path $RunKeyPath -Name "OnMyChatAI" -Value ("`"$WscriptExe`" //B //Nologo `"$Starter`"")
    try {
        $aiAction   = New-ScheduledTaskAction -Execute $WscriptExe -Argument "//B //Nologo `"$Starter`"" -WorkingDirectory $AiDir
        $aiTrigger  = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
        $aiSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                          -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
        Register-ScheduledTask -TaskName "OnMyChatAI" -Action $aiAction -Trigger $aiTrigger `
            -Settings $aiSettings -Description "OnMyChat local AI backend" -Force | Out-Null
    } catch {}

    # 5. Ollama for Windows (per-user install, it does not need elevation)
    if (-not $NoOllama) {
        $OllamaExe = Join-Path $env:LOCALAPPDATA "Programs\Ollama\ollama.exe"
        if (-not (Test-Path $OllamaExe)) {
            Write-Host "Installing Ollama ..."
            Download "https://ollama.com/download/OllamaSetup.exe" (Join-Path $TmpDir "OllamaSetup.exe")
            Start-Process -FilePath (Join-Path $TmpDir "OllamaSetup.exe") -ArgumentList "/VERYSILENT" -Wait
        } else {
            Write-Host "Ollama already present"
        }
    }

    Write-Host "Starting OnMyChat ..."
    try { Start-ScheduledTask -TaskName "OnMyChatAI" -ErrorAction Stop } catch { & $WscriptExe //B //Nologo "$Starter" }

    Write-Host ""
    Write-Host "OnMyChat installed at $AiDir (http://localhost:4000)"
    Write-Host "Ollama model to pull for local inference: ollama pull gemma4 (see config.ini DEFAULT_MODEL)"
}

if ($WithAi) { Install-Ai }
