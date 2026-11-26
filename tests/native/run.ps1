$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$testRoot = Join-Path $env:TEMP ("fzunzip-native-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $testRoot | Out-Null

try {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    function New-TestArchive($Path, $Entries, $Compression) {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
        try {
            $archive = [System.IO.Compression.ZipArchive]::new(
                $stream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
            try {
                foreach($item in $Entries) {
                    $entry = $archive.CreateEntry($item.Name, $Compression)
                    $entryStream = $entry.Open()
                    try {
                        $bytes = [System.Text.Encoding]::UTF8.GetBytes($item.Content)
                        $entryStream.Write($bytes, 0, $bytes.Length)
                    } finally {
                        $entryStream.Dispose()
                    }
                }
            } finally {
                $archive.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
    }

    $storedEntries = @(
        @{ Name = "plain.txt"; Content = "stored data" },
        @{ Name = "empty.bin"; Content = "" }
    )
    New-TestArchive (Join-Path $testRoot "stored.zip") $storedEntries `
        ([System.IO.Compression.CompressionLevel]::NoCompression)
    $repeated = -join (1..96000 | ForEach-Object { [char](97 + (($_ - 1) % 23)) })
    New-TestArchive (Join-Path $testRoot "deflated.zip") `
        @(@{ Name = "repeated.txt"; Content = $repeated }) `
        ([System.IO.Compression.CompressionLevel]::Optimal)
    New-TestArchive (Join-Path $testRoot "empty.zip") @() `
        ([System.IO.Compression.CompressionLevel]::Optimal)
    New-TestArchive (Join-Path $testRoot "traversal.zip") `
        @(@{ Name = "../escape.txt"; Content = "unsafe" }) `
        ([System.IO.Compression.CompressionLevel]::Optimal)

    $storedPath = Join-Path $testRoot "stored.zip"
    $storedBytes = [System.IO.File]::ReadAllBytes($storedPath)
    $centralOffset = -1
    for($i = 0; $i -le $storedBytes.Length - 46; $i++) {
        if($storedBytes[$i] -eq 0x50 -and $storedBytes[$i + 1] -eq 0x4b -and
           $storedBytes[$i + 2] -eq 0x01 -and $storedBytes[$i + 3] -eq 0x02) {
            $centralOffset = $i
            break
        }
    }
    if($centralOffset -lt 0) { throw "Could not locate ZIP central directory" }
    $storedBytes[$centralOffset + 16] = $storedBytes[$centralOffset + 16] -bxor 0xff
    [System.IO.File]::WriteAllBytes((Join-Path $testRoot "bad_crc.zip"), $storedBytes)

    [System.IO.File]::WriteAllBytes(
        (Join-Path $testRoot "truncated.zip"),
        $storedBytes[0..($storedBytes.Length - 11)])

    $encryptedBytes = [System.IO.File]::ReadAllBytes($storedPath)
    $localFlags = [BitConverter]::ToUInt16($encryptedBytes, 6) -bor 1
    [BitConverter]::GetBytes($localFlags).CopyTo($encryptedBytes, 6)
    $centralSignatureOffset = -1
    for($i = 0; $i -le $encryptedBytes.Length - 46; $i++) {
        if($encryptedBytes[$i] -eq 0x50 -and $encryptedBytes[$i + 1] -eq 0x4b -and
           $encryptedBytes[$i + 2] -eq 0x01 -and $encryptedBytes[$i + 3] -eq 0x02) {
            $centralSignatureOffset = $i
            break
        }
    }
    $centralFlags = [BitConverter]::ToUInt16($encryptedBytes, $centralSignatureOffset + 8) -bor 1
    [BitConverter]::GetBytes($centralFlags).CopyTo($encryptedBytes, $centralSignatureOffset + 8)
    [System.IO.File]::WriteAllBytes((Join-Path $testRoot "encrypted.zip"), $encryptedBytes)

    $emcc = Get-Command emcc.bat -ErrorAction SilentlyContinue
    $node = Get-Command node.exe -ErrorAction SilentlyContinue
    if(-not $emcc -or -not $node) {
        throw "Emscripten (emcc.bat) and Node.js (node.exe) must be on PATH"
    }

    $output = Join-Path $testRoot "test_zip_unzipper.js"
    $preload = $testRoot + "@/ext"
    $compile = 'call "' + $emcc.Source + '" -sALLOW_MEMORY_GROWTH=1 ' +
        '-sSTACK_SIZE=262144 -sEXIT_RUNTIME=1 -I"tests\native\stubs" ' +
        '-I"." --preload-file "' + $preload + '" -o "' + $output +
        '" zip_unzipper.c tests\native\test_zip_unzipper.c'
    & $env:ComSpec /c $compile
    if($LASTEXITCODE -ne 0) { throw "Native test harness compilation failed" }

    Push-Location $testRoot
    try {
        & $node.Source $output /ext
        $testExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    if($testExit -ne 0) { throw "Native extractor tests failed" }
} finally {
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
