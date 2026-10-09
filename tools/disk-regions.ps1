<#
.SYNOPSIS
  Hash a disk's regions from Windows, read-only: the optional Windows-side
  check that Jam OS left the drive's untouched parts untouched.

.DESCRIPTION
  Prints the region list of docs/M12.7-PLAN.md ("The region list") in the
  same lines as tools/disk-regions.py (see that file's header for the list
  and the line format), so the outputs can be compared with a diff.

  By default only the regions Windows does not change while it runs are
  hashed: gpt-head, gpt-tail, and the Microsoft reserved and Windows
  recovery partitions. -All hashes every region (the ESP and C: change
  while Windows runs, so their hashes are only comparable after a full
  shutdown).

  READ-ONLY: the disk is opened with FileAccess.Read (share mode
  ReadWrite, which is what Windows needs to let a read open succeed on a
  disk in use) and this script contains no write call. It reads
  \\.\PhysicalDriveN, so it needs an administrator PowerShell.

  The owner runs it, on the PC, later. Nobody else runs it against a disk:
  the tests only use -Image on a file.

.EXAMPLE
  # An administrator PowerShell; the disk number is Get-Disk's Number:
  Get-Disk | Format-Table Number, FriendlyName, Size
  .\disk-regions.ps1 -Disk 0 | Tee-Object regions-windows-1.txt

.EXAMPLE
  .\disk-regions.ps1 -Image .\disk.img -BlockSize 512 -All
#>
param(
    [Parameter(ParameterSetName = 'Disk', Mandatory = $true)] [int]$Disk,
    [Parameter(ParameterSetName = 'Image', Mandatory = $true)] [string]$Image,
    [int]$BlockSize = 0,
    [switch]$All
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
public static class JamCrc32 {
    static uint[] t;
    public static uint Of(byte[] b, int off, int len) {
        if (t == null) {
            uint[] tt = new uint[256];
            for (uint i = 0; i < 256; i++) {
                uint c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                tt[i] = c;
            }
            t = tt;
        }
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < len; i++) crc = t[(crc ^ b[off + i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
}
'@

$MiB = [int64]1048576
$GiB = [int64]1073741824
$guidMsr = 'e3c9e316-0b5c-4db8-817d-f92df00215ae'
$guidRecovery = 'de94bba4-06d1-4d40-a16a-bfd50179d6ac'

# Open: the only place a disk or file is opened, with FileAccess.Read.
if ($PSCmdlet.ParameterSetName -eq 'Disk') {
    $path = '\\.\PhysicalDrive' + $Disk
    $gd = Get-Disk -Number $Disk
    $size = [int64]$gd.Size
    if ($BlockSize -eq 0) { $BlockSize = [int]$gd.LogicalSectorSize }
} else {
    if ($Image.StartsWith('\\.\') -or -not (Test-Path -LiteralPath $Image -PathType Leaf)) {
        throw "-Image takes a regular file, not a device: $Image"
    }
    $path = (Resolve-Path -LiteralPath $Image).ProviderPath
    $size = (Get-Item -LiteralPath $path).Length
    if ($BlockSize -eq 0) { $BlockSize = 512 }
}
$fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite, 1)
$bs = $BlockSize
$blocks = [int64]($size / $bs)

function Read-Blocks([int64]$lba, [int64]$count) {
    $buf = New-Object byte[] ($count * $bs)
    [void]$fs.Seek($lba * $bs, [System.IO.SeekOrigin]::Begin)
    $got = 0
    while ($got -lt $buf.Length) {
        $n = $fs.Read($buf, $got, $buf.Length - $got)
        if ($n -le 0) { throw "short read at lba $lba" }
        $got += $n
    }
    return , $buf
}

function Get-Sha([int64]$lba, [int64]$count) {
    $h = [System.Security.Cryptography.SHA256]::Create()
    $chunk = [int64](4 * $MiB / $bs)
    $done = [int64]0
    while ($done -lt $count) {
        $n = [Math]::Min($chunk, $count - $done)
        $b = Read-Blocks ($lba + $done) $n
        [void]$h.TransformBlock($b, 0, $b.Length, $null, 0)
        $done += $n
    }
    [void]$h.TransformFinalBlock((New-Object byte[] 0), 0, 0)
    return ([BitConverter]::ToString($h.Hash) -replace '-', '').ToLower()
}

function Get-GptHeader([int64]$lba, [int64]$my, [int64]$alt) {
    $raw = Read-Blocks $lba 1
    if ([Text.Encoding]::ASCII.GetString($raw, 0, 8) -ne 'EFI PART') { return $null }
    $rev = [BitConverter]::ToUInt32($raw, 8)
    $hsize = [BitConverter]::ToUInt32($raw, 12)
    $crc = [BitConverter]::ToUInt32($raw, 16)
    $rsvd = [BitConverter]::ToUInt32($raw, 20)
    if ($rev -ne 0x00010000 -or $hsize -lt 92 -or $hsize -gt $bs -or $rsvd -ne 0) { return $null }
    $hb = New-Object byte[] $hsize
    [Array]::Copy($raw, $hb, $hsize)
    $hb[16] = 0; $hb[17] = 0; $hb[18] = 0; $hb[19] = 0
    if ([JamCrc32]::Of($hb, 0, [int]$hsize) -ne $crc) { return $null }
    $myLba = [BitConverter]::ToInt64($raw, 24)
    $altLba = [BitConverter]::ToInt64($raw, 32)
    $first = [BitConverter]::ToInt64($raw, 40)
    $last = [BitConverter]::ToInt64($raw, 48)
    $entLba = [BitConverter]::ToInt64($raw, 72)
    $n = [BitConverter]::ToUInt32($raw, 80)
    $esize = [BitConverter]::ToUInt32($raw, 84)
    $ecrc = [BitConverter]::ToUInt32($raw, 88)
    if ($myLba -ne $my -or $altLba -ne $alt -or $first -gt $last -or $last -ge $blocks) { return $null }
    if ($esize -lt 128 -or ($esize -band ($esize - 1)) -ne 0 -or $n -lt 128 -or $n -gt 1024 -or
        ([int64]$n * $esize) -gt 32768) { return $null }
    $entBlocks = [int64](([int64]$n * $esize + $bs - 1) / $bs)
    if ($entLba + $entBlocks -gt $blocks) { return $null }
    $arr = Read-Blocks $entLba $entBlocks
    if ([JamCrc32]::Of($arr, 0, [int]($n * $esize)) -ne $ecrc) { return $null }
    return @{ first = $first; last = $last; n = [int]$n; esize = [int]$esize; arr = $arr }
}

$notes = New-Object System.Collections.ArrayList
$regs = New-Object System.Collections.ArrayList   # @{name; lba; count; stable}

$prim = Get-GptHeader 1 1 ($blocks - 1)
$back = Get-GptHeader ($blocks - 1) ($blocks - 1) 1
[void]$notes.Add(('GPT primary header {0}, backup {1}' -f $(if ($prim) { 'good' } else { 'bad' }), $(if ($back) { 'good' } else { 'bad' })))
$h = if ($prim) { $prim } else { $back }
$parts = New-Object System.Collections.ArrayList   # @{guid; start; count}
if ($h) {
    $used = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $h.n; $i++) {
        $o = $i * $h.esize
        $allZero = $true
        for ($k = 0; $k -lt 16; $k++) { if ($h.arr[$o + $k] -ne 0) { $allZero = $false; break } }
        if ($allZero) { continue }
        $tb = New-Object byte[] 16
        [Array]::Copy($h.arr, $o, $tb, 0, 16)
        $guid = (New-Object Guid (, $tb)).ToString()
        $first = [BitConverter]::ToInt64($h.arr, $o + 32)
        $last = [BitConverter]::ToInt64($h.arr, $o + 40)
        if (-not ($h.first -le $first -and $first -le $last -and $last -le $h.last)) {
            [void]$notes.Add(('entry {0} ({1}) outside the usable range: left out' -f ($i + 1), $guid)); continue
        }
        $overlap = $false
        foreach ($u in $used) { if ($first -le $u[1] -and $u[0] -le $last) { $overlap = $true } }
        if ($overlap) {
            [void]$notes.Add(('entry {0} ({1}) overlaps an earlier one: left out' -f ($i + 1), $guid)); continue
        }
        [void]$used.Add(@($first, $last))
        [void]$parts.Add(@{ guid = $guid; start = $first; count = $last - $first + 1 })
    }
} else {
    [void]$notes.Add('no good GPT: this script lists only GPT disks; the head and tail are all it hashes')
}

if ($blocks -ge 34) { [void]$regs.Add(@{ name = 'gpt-head'; lba = 0; count = 34; stable = $true }) }
$big = [int64]($GiB / $bs)
$s64 = [int64](64 * $MiB / $bs)
$s1 = [int64]($MiB / $bs)
for ($idx = 0; $idx -lt [Math]::Min(16, $parts.Count); $idx++) {
    $p = $parts[$idx]
    $name = 'part' + ($idx + 1)
    $stable = ($p.guid -eq $guidMsr) -or ($p.guid -eq $guidRecovery)
    if ($p.count -lt $big) {
        [void]$regs.Add(@{ name = $name; lba = $p.start; count = $p.count; stable = $stable })
    } else {
        [void]$regs.Add(@{ name = "$name-head"; lba = $p.start; count = $s64; stable = $false })
        [void]$regs.Add(@{ name = "$name-tail"; lba = $p.start + $p.count - $s64; count = $s64; stable = $false })
        for ($k = 0; $k -lt 32; $k++) {
            [void]$regs.Add(@{ name = ('{0}-s{1:00}' -f $name, $k); lba = $p.start + $k * [int64]($p.count / 32)
                               count = $s1; stable = $false })
        }
    }
}
if ($parts.Count -gt 16) { [void]$notes.Add(('{0} more partition(s) not listed' -f ($parts.Count - 16))) }
if ($blocks -ge 34) { [void]$regs.Add(@{ name = 'gpt-tail'; lba = $blocks - 33; count = 33; stable = $true }) }

Write-Output ('# disk-regions: {0} block_size {1} blocks {2}' -f $path, $bs, $blocks)
foreach ($n in $notes) { Write-Output ('# ' + $n) }
$shown = 0
foreach ($r in $regs) {
    if (-not $All -and -not $r.stable) { continue }
    Write-Output ('{0} lba {1} count {2} sha256 {3}' -f $r.name, $r.lba, $r.count, (Get-Sha $r.lba $r.count))
    $shown++
}
Write-Output ('# disk-regions: {0} regions' -f $shown)
$fs.Close()
