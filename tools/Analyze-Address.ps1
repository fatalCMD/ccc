<#
.SYNOPSIS
	Measures how often authored dialogue speaks directly to the player.

.DESCRIPTION
	A reaction shot needs a reason to fire. "The line mentions the player" is an
	appealing one, but only if it is *rare* — the emotion scan already taught this
	lesson the hard way: intensity looked like a usable axis until the scan showed
	76-83% of every line sitting at exactly 50, leaving only 100 (~5-8% of lines)
	with the rarity a close-up needs.

	Second-person address almost certainly fails that test. Skyrim's dialogue is
	written *at* the player, so "you" is likely in most lines and a rule built on
	it would cut to the player constantly — the same shape as the cue-is-not-a-turn
	bug, a signal that fires so often it carries nothing.

	So measure before building. This walks the same INFO records as
	Analyze-Emotions.ps1 but reads NAM1, the response text, and reports what share
	of lines each candidate trigger would fire on. Anything landing near the
	emotion scan's 5-8% is a usable accent; anything in the tens of percent is
	wallpaper.

	NOTE ON LOCALIZED PLUGINS. The vanilla masters set the TES4 localized flag
	(0x80), which means NAM1 does not hold text at all — it holds a 4-byte string
	ID into Strings\<plugin>_english.ILSTRINGS. Read naively it scans as binary
	and every pattern reports 0.00%, which looks exactly like a real finding and
	is not one. The strings live inside "Skyrim - Interface.bsa", so they have to
	be unpacked first:

	    TOOLS\BSArch\BSArch.exe unpack "...\Data\Skyrim - Interface.bsa" <dir> -q

	then point -StringsDir at the resulting Strings folder. Unlocalized plugins —
	most mod-authored ESPs — need none of this and are read directly.

.PARAMETER Plugins
	Plugin files to scan. Defaults to the vanilla masters.

.PARAMETER StringsDir
	Folder holding the unpacked *_english.ILSTRINGS files. Required for localized
	plugins; ignored for the rest.

.EXAMPLE
	.\tools\Analyze-Address.ps1 -StringsDir C:\temp\iface\Strings
	.\tools\Analyze-Address.ps1 -Plugins 'X:\...\Data\3DNPC.esp'
#>
[CmdletBinding()]
param(
	[string[]]$Plugins,
	[string]$StringsDir
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
using System.Text.RegularExpressions;

public static class AddressScan
{
	// Each class is counted independently; one line can satisfy several. The point
	// is the firing rate of each candidate rule on its own, not a partition.
	static readonly string[] Names = {
		"total", "title", "honorific", "secondPerson", "question",
		"opensOnYou", "imperative", "titleOrHonorific", "rareCombo"
	};

	static readonly Regex Title      = new Regex(@"\b(dragonborn|dovahkiin)\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
	static readonly Regex Honorific  = new Regex(@"\b(thane|harbinger|arch-?mage|listener|nightingale|champion|dragonslayer)\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
	static readonly Regex Second     = new Regex(@"\b(you|your|yours|yourself|you're|you'll|you've|you'd)\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
	static readonly Regex OpensOnYou = new Regex(@"^\s*[""']?(you|your)\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
	static readonly Regex Imperative = new Regex(@"\b(go|bring|find|take|come|follow|help|listen|look|tell|give|stop|wait)\b\s", RegexOptions.IgnoreCase | RegexOptions.Compiled);

	// Populated from *_english.ILSTRINGS when the plugin is localized. Null means
	// NAM1 is literal text and should be read as-is.
	static Dictionary<uint, string> Strings;
	public static bool Localized;

	public static Dictionary<string, long> Scan(string path, string ilstrings)
	{
		var tally = new Dictionary<string, long>();
		foreach (string n in Names) tally[n] = 0;
		byte[] data = File.ReadAllBytes(path);

		// TES4 is always the first record, and bit 0x80 of its flags is "localized".
		Localized = data.Length > 12 && (BitConverter.ToUInt32(data, 8) & 0x80) != 0;
		Strings = null;
		if (Localized && ilstrings != null && File.Exists(ilstrings))
			Strings = LoadStrings(ilstrings);

		Walk(data, 0, data.Length, tally);
		return tally;
	}

	// ILSTRINGS/DLSTRINGS layout: uint32 count, uint32 dataSize, then count pairs
	// of (uint32 id, uint32 offset), then the data block. Offsets are relative to
	// the start of that block, and in the *L* variants each entry is a uint32
	// byte-length followed by the string. Plain .STRINGS omits the length prefix.
	static Dictionary<uint, string> LoadStrings(string path)
	{
		var map = new Dictionary<uint, string>();
		byte[] b = File.ReadAllBytes(path);
		if (b.Length < 8) return map;

		uint count = BitConverter.ToUInt32(b, 0);
		uint dataSize = BitConverter.ToUInt32(b, 4);
		int dirStart = 8;
		int dataStart = (int)(b.Length - dataSize);
		Encoding cp = Encoding.GetEncoding(1252);

		bool lengthPrefixed = path.EndsWith("lstrings", StringComparison.OrdinalIgnoreCase);

		for (uint i = 0; i < count; i++)
		{
			int e = dirStart + (int)i * 8;
			if (e + 8 > b.Length) break;
			uint id = BitConverter.ToUInt32(b, e);
			uint off = BitConverter.ToUInt32(b, e + 4);
			int at = dataStart + (int)off;
			if (at < 0 || at >= b.Length) continue;

			int len;
			if (lengthPrefixed)
			{
				if (at + 4 > b.Length) continue;
				len = (int)BitConverter.ToUInt32(b, at);
				at += 4;
			}
			else
			{
				len = 0;
				while (at + len < b.Length && b[at + len] != 0) len++;
			}
			if (len <= 0 || at + len > b.Length) continue;
			while (len > 0 && b[at + len - 1] == 0) len--;
			if (len <= 0) continue;

			map[id] = cp.GetString(b, at, len);
		}
		return map;
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
		// Windows-1252, not UTF-8. Skyrim's strings are codepage text and the
		// apostrophes in "you're" arrive as 0x92 in plenty of authored lines.
		Encoding cp = Encoding.GetEncoding(1252);

		int p = 0;
		int oversize = 0;
		while (p + 6 <= rec.Length)
		{
			string st = Encoding.ASCII.GetString(rec, p, 4);
			int sl = BitConverter.ToUInt16(rec, p + 4);
			p += 6;

			if (st == "XXXX" && sl == 4)
			{
				oversize = (int)BitConverter.ToUInt32(rec, p);
				p += sl;
				continue;
			}
			if (oversize > 0) { sl = oversize; oversize = 0; }

			if (p + sl > rec.Length) break;

			// NAM1 is the spoken text of one response. An INFO holds several, which
			// is why this counts responses rather than records — the same unit the
			// emotion scan reported and the same unit a cue arrives on.
			if (st == "NAM1")
			{
				if (Strings != null)
				{
					// Localized: NAM1 is a string ID, not a sentence.
					if (sl >= 4)
					{
						uint id = BitConverter.ToUInt32(rec, p);
						string looked;
						if (Strings.TryGetValue(id, out looked) && looked.Length > 0)
							Tally(looked, tally);
					}
				}
				else if (sl > 1)
				{
					int len = sl;
					while (len > 0 && rec[p + len - 1] == 0) len--;
					if (len > 0) Tally(cp.GetString(rec, p, len), tally);
				}
			}
			p += sl;
		}
	}

	static void Bump(Dictionary<string, long> t, string k) { t[k] = t[k] + 1; }

	static void Tally(string text, Dictionary<string, long> t)
	{
		Bump(t, "total");

		bool title     = Title.IsMatch(text);
		bool honorific = Honorific.IsMatch(text);
		bool second    = Second.IsMatch(text);
		bool question  = text.TrimEnd().EndsWith("?");
		bool opens     = OpensOnYou.IsMatch(text);
		bool imper     = Imperative.IsMatch(text);

		if (title)     Bump(t, "title");
		if (honorific) Bump(t, "honorific");
		if (second)    Bump(t, "secondPerson");
		if (question)  Bump(t, "question");
		if (opens)     Bump(t, "opensOnYou");
		if (imper)     Bump(t, "imperative");

		if (title || honorific) Bump(t, "titleOrHonorific");

		// The candidate rule: named outright, or asked something directly. Both are
		// moments a listener would actually look up.
		if (title || honorific || (question && second)) Bump(t, "rareCombo");
	}
}
'@ -Language CSharp

$total = @{}
$skipped = @()
foreach ($p in $Plugins) {
	$leaf = Split-Path $p -Leaf
	$il = $null
	if ($StringsDir) {
		$il = Join-Path $StringsDir ("{0}_english.ilstrings" -f [IO.Path]::GetFileNameWithoutExtension($p))
	}

	$t = [AddressScan]::Scan($p, $il)

	# A localized plugin with no strings file scans as binary and reports zeroes.
	# That is indistinguishable from a real "this never happens" result, so say so
	# out loud and drop the plugin rather than folding silent nulls into the total.
	if ([AddressScan]::Localized -and -not ($il -and (Test-Path $il))) {
		Write-Host "Skipping $leaf - localized, no strings file" -ForegroundColor DarkYellow
		$skipped += $leaf
		continue
	}

	Write-Host ("Scanned {0} ({1}, {2:N0} responses)" -f $leaf,
		$(if ([AddressScan]::Localized) { 'localized' } else { 'literal text' }), $t['total']) -ForegroundColor Cyan
	foreach ($k in $t.Keys) {
		if ($total.ContainsKey($k)) { $total[$k] += $t[$k] } else { $total[$k] = $t[$k] }
	}
}

if (-not $total.Count -or -not $total['total']) {
	Write-Host ""
	Write-Host "No readable dialogue text. Unpack the strings first - see the notes at the top." -ForegroundColor Red
	return
}

$grand = $total['total']
Write-Host ""
Write-Host "Responses with text: $('{0:N0}' -f $grand)" -ForegroundColor Green
Write-Host ""
Write-Host "=== how often each candidate trigger would fire ===" -ForegroundColor Yellow

$order = @(
	@{ k = 'title';            d = 'names Dragonborn / Dovahkiin' },
	@{ k = 'honorific';        d = 'Thane, Harbinger, Arch-Mage, Listener...' },
	@{ k = 'titleOrHonorific'; d = 'either of the above' },
	@{ k = 'rareCombo';        d = 'named, or asked a direct question' },
	@{ k = 'question';         d = 'ends in a question mark' },
	@{ k = 'opensOnYou';       d = 'opens on "you"/"your"' },
	@{ k = 'imperative';       d = 'contains a command verb' },
	@{ k = 'secondPerson';     d = 'contains you/your/yours' }
)

foreach ($row in $order) {
	$v = $total[$row.k]
	$pct = if ($grand -gt 0) { 100 * $v / $grand } else { 0 }
	$verdict = if ($pct -lt 2) { 'too rare' } elseif ($pct -le 12) { 'USABLE' } elseif ($pct -le 30) { 'frequent' } else { 'wallpaper' }
	"{0,-18} {1,8:N0}  {2,6:N2}%  {3,-9}  {4}" -f $row.k, $v, $pct, $verdict, $row.d
}

Write-Host ""
Write-Host "Compare against the emotion scan: intensity 100 lands on ~5-8% of lines," -ForegroundColor DarkGray
Write-Host "which is the rarity that makes a close-up read as emphasis rather than habit." -ForegroundColor DarkGray
