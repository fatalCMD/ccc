<#
.SYNOPSIS
	Tallies dialogue emotion type and intensity across Skyrim plugin files.

.DESCRIPTION
	Scene Director's shot-selection design rests on a claim: that every authored
	response carries an emotion and an intensity, and that those values vary
	enough to drive a shot language. In-game sampling could not test that — a
	follower's greeting pool returned Happy(50%) sixteen times out of sixteen.

	This reads the answer straight out of the plugin files instead. Every INFO
	record carries a TRDT subrecord whose first two DWORDs are the emotion type
	and its value, so the true distribution over ~60,000 lines is a file scan
	rather than a play session.

	If the result is overwhelmingly one value, the emotion signal is not usable
	and the cut policy has to be built on duration and speaker changes instead.
	Better to learn that from a scan than from a half-built director.

.PARAMETER Plugins
	Plugin files to scan. Defaults to the vanilla masters.

.EXAMPLE
	.\tools\Analyze-Emotions.ps1
	.\tools\Analyze-Emotions.ps1 -Plugins 'X:\...\Data\3DNPC.esp'
#>
[CmdletBinding()]
param(
	[string[]]$Plugins
)

$ErrorActionPreference = 'Stop'

$dataDir = 'X:\!--- Main\Documents\Nolvus\STOCK GAME\Data'
if (-not $Plugins) {
	$Plugins = @('Skyrim.esm', 'Dawnguard.esm', 'HearthFires.esm', 'Dragonborn.esm') |
		ForEach-Object { Join-Path $dataDir $_ } |
		Where-Object { Test-Path $_ }
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;

public static class EsmScan
{
	public static Dictionary<string, long> Scan(string path)
	{
		var tally = new Dictionary<string, long>();
		byte[] data = File.ReadAllBytes(path);
		Walk(data, 0, data.Length, tally);
		return tally;
	}

	static void Walk(byte[] b, int pos, int end, Dictionary<string, long> tally)
	{
		while (pos + 24 <= end)
		{
			string type = Encoding.ASCII.GetString(b, pos, 4);
			uint size = BitConverter.ToUInt32(b, pos + 4);

			if (type == "GRUP")
			{
				if (size < 24) return;
				long ge = (long)pos + size;
				if (ge > end) return;
				Walk(b, pos + 24, (int)ge, tally);
				pos = (int)ge;
			}
			else
			{
				uint flags = BitConverter.ToUInt32(b, pos + 8);
				int dataStart = pos + 24;
				long recEnd = (long)dataStart + size;
				if (recEnd > end) return;

				if (type == "INFO")
				{
					byte[] rec = null;
					if ((flags & 0x00040000) != 0)
						rec = Inflate(b, dataStart, (int)size);
					else
					{
						rec = new byte[size];
						Array.Copy(b, dataStart, rec, 0, (int)size);
					}
					if (rec != null) ScanSub(rec, tally);
				}
				pos = (int)recEnd;
			}
		}
	}

	static byte[] Inflate(byte[] b, int start, int len)
	{
		try
		{
			int outSize = (int)BitConverter.ToUInt32(b, start);
			if (outSize <= 0 || outSize > 64 * 1024 * 1024) return null;
			// Skip the 4-byte decompressed size and the 2-byte zlib header.
			using (var ms = new MemoryStream(b, start + 6, len - 6))
			using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
			{
				byte[] outBuf = new byte[outSize];
				int read = 0, n;
				while (read < outSize && (n = ds.Read(outBuf, read, outSize - read)) > 0) read += n;
				return outBuf;
			}
		}
		catch { return null; }
	}

	static void ScanSub(byte[] rec, Dictionary<string, long> tally)
	{
		int p = 0;
		int oversize = 0;
		while (p + 6 <= rec.Length)
		{
			string st = Encoding.ASCII.GetString(rec, p, 4);
			int sl = BitConverter.ToUInt16(rec, p + 4);
			p += 6;

			// XXXX carries the real length of the subrecord that follows it.
			if (st == "XXXX" && sl == 4)
			{
				oversize = (int)BitConverter.ToUInt32(rec, p);
				p += sl;
				continue;
			}
			if (oversize > 0) { sl = oversize; oversize = 0; }

			if (p + sl > rec.Length) break;

			if (st == "TRDT" && sl >= 8)
			{
				uint et = BitConverter.ToUInt32(rec, p);
				uint ev = BitConverter.ToUInt32(rec, p + 4);
				string key = Name(et) + "\t" + ev;
				long c;
				tally.TryGetValue(key, out c);
				tally[key] = c + 1;
			}
			p += sl;
		}
	}

	static string Name(uint t)
	{
		switch (t)
		{
			case 0: return "Neutral";
			case 1: return "Anger";
			case 2: return "Disgust";
			case 3: return "Fear";
			case 4: return "Sad";
			case 5: return "Happy";
			case 6: return "Surprise";
			case 7: return "Puzzled";
			default: return "Unknown" + t;
		}
	}
}
'@ -Language CSharp

$total = @{}
foreach ($p in $Plugins) {
	Write-Host "Scanning $(Split-Path $p -Leaf) ..." -ForegroundColor Cyan
	$t = [EsmScan]::Scan($p)
	foreach ($k in $t.Keys) {
		if ($total.ContainsKey($k)) { $total[$k] += $t[$k] } else { $total[$k] = $t[$k] }
	}
}

$grand = ($total.Values | Measure-Object -Sum).Sum
Write-Host ""
Write-Host "Total responses with a TRDT emotion: $grand" -ForegroundColor Green
Write-Host ""

Write-Host "=== by emotion type ===" -ForegroundColor Yellow
$byType = @{}
foreach ($k in $total.Keys) {
	$name = $k.Split("`t")[0]
	if ($byType.ContainsKey($name)) { $byType[$name] += $total[$k] } else { $byType[$name] = $total[$k] }
}
$byType.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object {
	"{0,-10} {1,8:N0}  {2,6:N2}%" -f $_.Key, $_.Value, (100 * $_.Value / $grand)
}

Write-Host ""
Write-Host "=== top 20 type+intensity pairs ===" -ForegroundColor Yellow
$total.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 20 | ForEach-Object {
	$parts = $_.Key.Split("`t")
	"{0,-10} {1,4}%  {2,8:N0}  {3,6:N2}%" -f $parts[0], $parts[1], $_.Value, (100 * $_.Value / $grand)
}
