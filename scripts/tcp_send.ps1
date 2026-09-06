# Sends line commands to the demo's TCP control server on localhost and prints
# the reply of each command prefixed with "< ". Used by the batch measure
# scripts on both desktop and Android (the latter through "adb forward").
# Commands are given in one string separated by "|".
param(
    [Parameter(Mandatory = $true)][int]$Port,
    [Parameter(Mandatory = $true)][string]$CommandsText,
    [int]$WaitSeconds = 0
)

$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
try {
    $stream = $client.GetStream()
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($stream)
    foreach ($cmd in ($CommandsText -split "\|")) {
        $trimmed = $cmd.Trim()
        if ($trimmed -eq "") {
            continue
        }
        $writer.WriteLine($trimmed)
        $reply = $reader.ReadLine()
        Write-Output ("< " + $reply)
        if ($WaitSeconds -gt 0) {
            Start-Sleep -Seconds $WaitSeconds
        }
    }
}
finally {
    $client.Close()
}
