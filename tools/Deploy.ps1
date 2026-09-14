<#
.SYNOPSIS
	Builds Scene Director and deploys it as a Mod Organizer 2 mod folder.

.DESCRIPTION
	Deploys to the MO2 mods directory as a separate mod folder rather than into
	STOCK GAME\Data, so the build stays under MO2's management and is removed by
	deleting one folder.

	This script deliberately does NOT touch modlist.txt or any MO2 profile.
	Enabling the mod is a manual step — a script that silently reorders a curated
	3600-mod load order is not a convenience.

.PARAMETER Target
	MO2 mods directory. Defaults to the Nolvus instance beside this repo.

.PARAMETER SkipBuild
	Deploy whatever is already in build/skyrim/Release without rebuilding.

.EXAMPLE
	.\tools\Deploy.ps1
	.\tools\Deploy.ps1 -SkipBuild
#>
[CmdletBinding()]
param(
	[string]$Target = 'X:\!--- Main\Documents\Nolvus\MODS\mods',
	[switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$modName = 'Scene Director'
$cmake = 'X:\VisualStudio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

# Keep the deployed version honest by reading it from the one place it is declared.
$version = '0.0.0'
$cmakeLists = Join-Path $root 'CMakeLists.txt'
$match = Select-String -Path $cmakeLists -Pattern 'VERSION\s+(\d+\.\d+\.\d+)' | Select-Object -First 1
if ($match) { $version = $match.Matches[0].Groups[1].Value }

if (-not $SkipBuild) {
	if (-not (Test-Path $cmake)) { throw "cmake not found at $cmake" }
	$env:VCPKG_ROOT = 'X:\vcpkg'
	Write-Host "Building Scene Director $version ..." -ForegroundColor Cyan
	& $cmake --build (Join-Path $root 'build/skyrim') --config Release --parallel 8
	if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

$dll = Join-Path $root 'build/skyrim/Release/SceneDirector.dll'
$ini = Join-Path $root 'config/SD.ini'
foreach ($f in @($dll, $ini)) {
	if (-not (Test-Path $f)) { throw "Missing build output: $f" }
}

if (-not (Test-Path $Target)) { throw "MO2 mods directory not found: $Target" }

$modRoot = Join-Path $Target $modName
$plugins = Join-Path $modRoot 'SKSE\Plugins'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null

# The game holds the DLL open for its whole run, so a deploy during a test
# session fails with a bare IOException that reads like a build problem. Name it.
$running = Get-Process -Name 'SkyrimSE' -ErrorAction SilentlyContinue
if ($running) {
	Write-Host ""
	Write-Host "Skyrim is still running (PID $($running.Id))." -ForegroundColor Yellow
	Write-Host "The build succeeded; only the copy is blocked. Close the game and re-run:" -ForegroundColor Yellow
	Write-Host "  .\tools\Deploy.ps1 -SkipBuild"
	throw "Deploy aborted: SkyrimSE.exe is holding SceneDirector.dll open."
}

Copy-Item $dll -Destination $plugins -Force

# The ini is MERGED, not copied over.
#
# `Copy-Item -Force` here silently destroyed the deployed settings on every
# deploy. That is worse than it sounds during development: the whole point of
# this loop is to change something, play, tune the ini, and deploy again — so
# the clobber landed precisely on the values that had just been worked out, and
# looked like the mod resetting itself rather than the deploy doing it.
#
# The shipped file is still the source of structure: new keys, new comments and
# reordering all arrive. Only the VALUES of keys that already exist on disk are
# carried over, which is the one thing the developer's copy knows that the
# repository's does not.
$liveIni = Join-Path $plugins 'SD.ini'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if (-not (Test-Path $liveIni)) {
	Copy-Item $ini -Destination $plugins -Force
	Write-Host "  SD.ini installed" -ForegroundColor DarkGray
} else {
	# Existing values, by section and key.
	#
	# SECTION IS TRACKED, and the note that used to sit here saying it need not be
	# was what made this delete settings. Keys being unique across the file is
	# enough to LOOK one up; it is not enough to put a live-only key BACK, because
	# an ini key means nothing without the section it sits under.
	#
	# The old merge emitted only lines that appear in the shipped file, so any key
	# the live file carried and the shipped file did not was silently dropped. That
	# is not a hypothetical: the per-shot dials — i<Name>Weight, i<Name>Fov,
	# i<Name>Zoom — are written by the in-game menu and are deliberately NOT listed
	# in the shipped ini, which documents them in a comment instead. So they were
	# the one group of settings that existed purely as user data, and every deploy
	# wiped exactly them and left everything else untouched. Measured against a real
	# tuning session: 93 live keys in, 14 deleted, all of them per-shot.
	# bLogFaceAnim was lost to the same bug earlier and got its own note in the ini.
	# THE FLAT $live TABLE THAT USED TO SIT BESIDE THIS IS GONE. It was the last
	# thing in the script that could answer "what is this key worth" without being
	# told which section was being asked about, and leaving it here as an unused
	# convenience is how it would come back.
	$liveBySection = @{}
	$section = ''
	foreach ($line in [System.IO.File]::ReadAllLines($liveIni)) {
		if ($line -match '^\s*\[(.+?)\]\s*$') {
			$section = $Matches[1]
			if (-not $liveBySection.ContainsKey($section)) { $liveBySection[$section] = @{} }
			continue
		}
		if ($line -match '^\s*([A-Za-z_]\w*)\s*=\s*(.*?)\s*$') {
			if (-not $liveBySection.ContainsKey($section)) { $liveBySection[$section] = @{} }
			$liveBySection[$section][$Matches[1]] = $Matches[2]
		}
	}

	# Keys the shipped file accounts for, so the rest can be identified as user
	# data and carried over rather than dropped.
	#
	# BY SECTION, for the same reason $liveBySection is - and this half was still
	# flat long after the other half had been fixed, which is how the bug below
	# survived.
	$shippedBySection = @{}
	$section = ''
	foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
		if ($line -match '^\s*\[(.+?)\]\s*$') {
			$section = $Matches[1]
			if (-not $shippedBySection.ContainsKey($section)) { $shippedBySection[$section] = @{} }
			continue
		}
		if ($line -match '^\s*([A-Za-z_]\w*)\s*=\s*(.*?)\s*$') {
			if (-not $shippedBySection.ContainsKey($section)) { $shippedBySection[$section] = @{} }
			$shippedBySection[$section][$Matches[1]] = $true
		}
	}

	# RETIRED KEYS. Settings the mod used to have and no longer reads.
	#
	# The merge cannot tell these apart from user data on its own, and that is not
	# a flaw in it: both are "present in the live file, absent from the shipped
	# one", which is exactly the test that protects the per-shot dials the in-game
	# menu writes and this file deliberately does not list. So a removed setting
	# looked identical to a menu-written one and was carried forward for ever,
	# accumulating dead lines that read as live settings to anyone opening the ini.
	#
	# ADD A KEY HERE WHEN YOU REMOVE ONE FROM THE MOD. Nothing else needs doing:
	# the removal itself is what makes the plugin ignore it, and this is only about
	# not leaving a corpse in the player's config that looks like a dial.
	$retired = @(
		# 1.2.7 - move, amount and duration became per-shot settings, and the room
		# swing went with the randomiser it drove.
		'iDollyAmount', 'iDollyWindow', 'iRoomSwing',
		# Earlier - the global lens shift, replaced by per-setup FOV.
		'iLensBias',
		# The one-light key: a single lamp on every angle in the game with its
		# position hardcoded. Not migrated - there is no honest way to carry three
		# numbers over to lamps that did not exist.
		'iStrength', 'iRadius',
		# The warmth curve, replaced by an RGB picker. NOTE: iWarmth was briefly a
		# LIVE key in [Lighting] and was removed from this list for exactly that
		# reason - it is back because it is dead again. A key listed here while it
		# is live is one the deploy would offer to delete.
		'iWarmth',
		# The gaffer's-kit version of the lighting: ten rigs at three lamps at
		# seven values, plus a table of what every authored emotion did to them.
		# All of it worked and none of it was usable. Only these two ever reached
		# a shipped ini - the [LightRigs] and [LightEmotion] keys were written by
		# the menu into SD_user.ini, which no deploy touches.
		'iMasterScale', 'bEmotionShifts',
		# Never read by anything; removed rather than wired up.
		'bLogSessions', 'bLogLines',
		# Found by auditing a real SD_user.ini against the source: every one of
		# these was still sitting in a live config, and every one of them stopped
		# being read at some point without being listed here.
		#
		# iChoiceOffsetX/Y are the ones worth remembering. They positioned the
		# topic list, they appear NOWHERE in the source or the shipped ini - not
		# even as a retirement note - and the config carried 630 / -630, which is
		# somebody who moved the list a long way on purpose and has had it quietly
		# doing nothing ever since. A removed feature that leaves no trace is
		# indistinguishable from a broken one.
		'iChoiceOffsetX', 'iChoiceOffsetY',
		'bHideInterface', 'bHideHudWholesale', 'bHideChoicesOnGreeting',
		'bOpenOnSpeaker', 'iEstablishTime',
		# A binding for an action that no longer exists. Only Next angle and Who to
		# look at survive.
		'iKeyRelease',
		# 1.4 - a line ENDING stopped being a reason to change angle. The shot a
		# line was framed on is held through the pause after it, always, so there
		# is nothing left for this to mean.
		'bCutOnLineEnd',
		# 1.4 - replaced by bFadeAfterPlayerLine, and deliberately NOT migrated:
		# this key stored the inverse of what its label said, so carrying a value
		# over would be inverting somebody's setting on their behalf and being
		# wrong for anyone who had already worked the old meaning out.
		'bHideChoicesWhilePlayerSpeaks',
		# 1.4 - expressions, gaze and the player head draw flag went back to the
		# game. All three worked; all three wrote channels that belong to the
		# engine and to whatever face mods are installed, and this is a camera mod.
		'bExpressions', 'iExpressionStrength', 'bGaze', 'bHoldPlayerFace',
		'iListenerGaze', 'iSpeakerGaze'
	)
	$retiredSet = @{}
	foreach ($r in $retired) { $retiredSet[$r] = $true }

	$kept = New-Object System.Collections.Generic.List[string]
	$added = New-Object System.Collections.Generic.List[string]
	$carried = New-Object System.Collections.Generic.List[string]
	$dropped = New-Object System.Collections.Generic.List[string]
	$out = New-Object System.Collections.Generic.List[string]

	# Append every live-only key belonging to a section, as that section ends.
	$flush = {
		param($s)
		if (-not $s -or -not $liveBySection.ContainsKey($s)) { return }
		$live = $liveBySection[$s]
		$shippedHere = if ($shippedBySection.ContainsKey($s)) { $shippedBySection[$s] } else { @{} }
		$orphans = $live.Keys | Where-Object { -not $shippedHere.ContainsKey($_) } | Sort-Object
		foreach ($o in $orphans) {
			if ($retiredSet.ContainsKey($o)) { $dropped.Add("$o=$($live[$o])") | Out-Null }
		}
		$orphans = $orphans | Where-Object { -not $retiredSet.ContainsKey($_) }
		if ($orphans.Count -eq 0) { return }
		$out.Add('') | Out-Null
		$out.Add('; Written by the in-game menu. Not in the shipped ini, preserved on deploy.') | Out-Null
		foreach ($o in $orphans) {
			$out.Add("$o=$($live[$o])") | Out-Null
			$carried.Add("$o=$($live[$o])") | Out-Null
		}
	}

	$section = ''
	foreach ($line in [System.IO.File]::ReadAllLines($ini)) {
		if ($line -match '^\s*\[(.+?)\]\s*$') {
			& $flush $section
			$section = $Matches[1]
			$out.Add($line) | Out-Null
			continue
		}
		if ($line -match '^\s*([A-Za-z_]\w*)\s*=\s*(.*?)\s*$') {
			$key = $Matches[1]
			$shipped = $Matches[2]

			# LOOKED UP IN THIS SECTION, not across the whole file.
			#
			# This was a flat table, and it read the LAST value any section
			# happened to give a name. Harmless only while every key name in the
			# ini was unique - which was never written down anywhere and so was
			# not a rule anybody could follow.
			#
			# It broke the day [Lighting] gained a bEnabled beside the one in
			# [Direction]: the merge carried [Direction]'s 1 into [Lighting],
			# silently switching on a feature that ships off, and reported it as
			# "kept" because from where it was standing that is exactly what it
			# had done. Nothing about the output said which section it meant.
			$here = if ($liveBySection.ContainsKey($section)) { $liveBySection[$section] } else { @{} }
			if ($here.ContainsKey($key)) {
				if ($here[$key] -ne $shipped) { $kept.Add("[$section] $key=$($here[$key])") | Out-Null }
				$out.Add("$key=$($here[$key])") | Out-Null
				continue
			}
			$added.Add("[$section] $key=$shipped") | Out-Null
		}
		$out.Add($line) | Out-Null
	}
	& $flush $section

	# Written through .NET rather than Set-Content: this file carries em dashes,
	# and Set-Content defaults to the system ANSI codepage, which mangles them.
	[System.IO.File]::WriteAllLines($liveIni, $out, $utf8NoBom)

	Write-Host "  SD.ini merged - $($kept.Count) preserved, $($carried.Count) menu-written carried, $($added.Count) new, $($dropped.Count) retired" -ForegroundColor DarkGray
	foreach ($k in $kept)    { Write-Host "      kept    $k" -ForegroundColor DarkGray }
	foreach ($c in $carried) { Write-Host "      carried $c" -ForegroundColor DarkGray }
	foreach ($a in $added)   { Write-Host "      new     $a" -ForegroundColor DarkGray }
	# Louder than the rest on purpose: this is the one line that says a value the
	# player set is being thrown away, and it should never scroll past unnoticed.
	foreach ($d in $dropped) { Write-Host "      retired $d (no longer read; removed)" -ForegroundColor Yellow }
}

# MCM Helper config, if present. Optional at both ends: Scene Director works
# without MCM Helper, and MCM Helper needs no plugin from Scene Director.
$mcmSource = Join-Path $root 'package\MCM'
if (Test-Path $mcmSource) {
	Copy-Item $mcmSource -Destination $modRoot -Recurse -Force
	Write-Host "  MCM config staged" -ForegroundColor DarkGray
}

# MO2 shows an unmanaged folder as "not installed by MO2"; a meta.ini makes it a
# first-class entry with a readable version.
$stamp = (Get-Date).ToString('yyyy.M.d')
$meta = @"
[General]
gameName=
modid=0
version=d$stamp
newestVersion=
category=0
nexusFileStatus=1
installationFile=
repository=
comments=Scene Director $version - cinematic dialogue staging (observation build)
"@
Set-Content -Path (Join-Path $modRoot 'meta.ini') -Value $meta -Encoding utf8

$deployed = Get-Item (Join-Path $plugins 'SceneDirector.dll')
Write-Host ""
Write-Host "Deployed Scene Director $version" -ForegroundColor Green
Write-Host "  -> $plugins"
Write-Host "  DLL $([math]::Round($deployed.Length/1kb,1)) KB, built $($deployed.LastWriteTime)"
Write-Host ""
Write-Host "Next: enable '$modName' in Mod Organizer 2, load a save, talk to an NPC." -ForegroundColor Yellow

# Resolve the log path rather than assuming it. This machine's Documents folder is
# redirected into OneDrive, so the literal "Documents\My Games\..." that every
# SKSE guide prints is not where the log actually lands.
$docs = [Environment]::GetFolderPath('MyDocuments')
$log = Join-Path $docs 'My Games\Skyrim Special Edition\SKSE\SceneDirector.log'
Write-Host "Then read: $log"
if (Test-Path $log) {
	Write-Host "  (existing log last written $((Get-Item $log).LastWriteTime))" -ForegroundColor DarkGray
}
