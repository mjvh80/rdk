# rdk

A Windows FreeRDP 3 client focused on keyboard delivery. It has fullscreen
multi-monitor GDI/RDPGFX rendering, mouse input, remote cursor shapes, Windows
credential SSO, and client-side Alt-code composition. This is a focused client, not a complete
replacement for all Microsoft Remote Desktop features.

100% vibe coded with Astra.

## Run

Build with the existing `default` CMake configure preset and `release` build
preset (Visual Studio 2026, x64). The preset uses the existing vcpkg installation
at `S:/vcpkg`. The executable is `build/Release/rdk.exe`.

The repository's `vcpkg-configuration.json` selects a local FreeRDP 3.26.0
overlay with an audio channel-mixing fix. Keep the overlay when configuring a
clean build. Deploy the rebuilt `Release` directory with its matching DLLs,
helper and licenses; replacing only `rdk.exe` does not update dependency fixes.

```powershell
.\build\Release\rdk.exe /v:HOST
.\build\Release\rdk.exe /v:HOST /text:"Test text" /startup-delay:5000 /char-delay:10
```

rdk options use `/name:value` for values and `/name` for flags, matching
FreeRDP's Windows-style options. Quote values containing spaces or shell
metacharacters, as shown above. The old `--text STRING`, `--input SEQUENCE`,
`--startup-delay MS`, `--char-delay MS`, and `--credentials` forms remain
supported as compatibility aliases. `/help` includes both rdk and FreeRDP options.

## Monitor Selection

Use Windows display numbers, as shown by Identify in Windows Display settings:

```powershell
.\build\Release\rdk.exe /list:monitor
.\build\Release\rdk.exe /v:HOST /screen:1,2
```

`/list:monitor` lists active displays with their Windows numbers, current pixel
resolutions, positions, and device names, then exits without connecting. Numbers
come from Windows display device names (`DISPLAY1`, `DISPLAY2`, etc.), not from
zero-based enumeration order. Disabled displays are not listed or selectable.

`/screen:1,2` uses only those displays; `/monitors:1,2` is an alias with the same
Windows numbering. The first selected display becomes the remote primary.
The fullscreen window and advertised remote desktop use the selected displays'
current pixel dimensions and arrangement. This does not change local display
resolutions. Omit the option to use all active displays, or select one number
for a single-display session.

Selected displays must form a connected layout with shared edges. Because rdk
uses one rectangular window, its bounding area must not cover an unselected
display. Invalid selections are rejected instead of silently including another
display. Arrange the selected monitors next to each other in Windows if needed.
Reconnect after changing the local display arrangement.

## Graphics and AVC444

H.264 decoding is available through FreeRDP's FFmpeg backend. GDI still presents
the local framebuffer; this does not change keyboard handling or enable a new
Direct3D renderer. AVC is opt-in while comparing quality and responsiveness:

```powershell
.\build\Release\rdk.exe /v:HOST /gfx:avc444
```

Add `/gfx:avc444` to your existing command to advertise AVC444 and AVC444v2.
These preserve color detail better than AVC420, which can help small colored
text and syntax highlighting. AVC444 is not necessarily lossless, and the server
still decides which codecs to send. Unsupported servers can use non-AVC updates.
For an AVC420 comparison, use `/gfx:avc420` instead.

Remove all `/gfx` options to return to the previous non-AVC default, or explicitly
use `/gfx:avc444:off,avc420:off` to disable AVC within the graphics pipeline.
Other explicit `/gfx` options use FreeRDP's native behavior; bare `/gfx` uses its
automatic codec selection. The selected settings survive rdk's Reconnect action.

At connection time, `graphics requested:` lists the advertised settings. Once
updates arrive, `graphics received: AVC444v2` (or `AVC444`, `AVC420`, `Planar`, etc.)
identifies each GFX codec first decoded successfully during that channel's
lifetime. A session can use more than one codec. A request or channel-open log
alone does not prove AVC is active, and none of these logs proves the server is
using a hardware encoder. Non-GFX legacy bitmap updates do not produce these
codec messages.

Compare identical monitor layouts, display scaling, editor themes, and workloads:
small colored text, scrolling, window movement, and video. Check text clarity,
responsiveness, CPU usage on both machines, and network throughput. Remote GPU
rendering and hardware encoding depend on the host's Windows version, policies,
GPU, and driver; rdk does not change those settings. Local FFmpeg decoding may
use the CPU, and the GDI framebuffer remains in system memory.

The manifest enables `freerdp[ffmpeg]`; configuring can take longer the first
time while FFmpeg and FreeRDP are built. `/buildconfig` must report both
`WITH_GFX_H264=ON` and `WITH_FFMPEG=ON`. When deploying, include the updated
FreeRDP DLLs and their FFmpeg runtime dependencies from the build output, not
just the new executable. The FFmpeg license notice is copied alongside the
client as `FFmpeg-LICENSE.txt`; retain it and comply with the dependency's
license/source-availability requirements when redistributing binaries.

## Credentials

Omitting `/u` and `/p` first checks for credentials saved by rdk for this server
and port, then falls back to the current Windows logon credentials through
native SSPI. FreeRDP connection options are forwarded to its command-line parser.
Passwords supplied with `/p` are visible in process arguments.
Certificate callbacks accept untrusted or changed certificates for the current
session, preserving the original proof-of-concept behavior. `/cert:ignore` is
also supported. This does not protect against server impersonation.

If the remote account differs from your local Windows account, or Windows SSO
is unavailable, use the native credential dialog:

```powershell
.\build\Release\rdk.exe /v:HOST /cert:ignore /credentials
```

Enter the remote account as `DOMAIN\user` or `user@domain` and the password
directly in that dialog. For an account local to the remote computer, use
`REMOTE-COMPUTER\user`. The username is not prefilled with your local identity;
`/u:REMOTEUSER /d:REMOTEDOMAIN` can prefill it explicitly.

Select the save-password checkbox to store credentials in Windows Credential
Manager only after a successful connection. Subsequent launches with the same
hostname and port can omit `/credentials` and `/u`. Stored credentials belong
to the current local Windows user but authenticate as the saved remote user;
this does not require the two users to match or make domain SSO available.

`/credentials` always prompts, bypassing an existing saved entry so expired
passwords can be replaced. `/u` without `/p` also prompts; an explicit `/p`
takes precedence over the cache. Entries are named `rdk/hostname:port` under
Credential Manager's Windows Credentials / Generic Credentials. Delete an
entry there to stop reusing it. These entries are separate from mstsc's saved
credentials. Leaving the save checkbox unchecked does not delete an older entry.

`SEC_E_DOWNGRADE_DETECTED` during `InitializeSecurityContext` is an SSO/NLA
authentication failure, not certificate rejection. `/cert:ignore` does not
fix it. Explicit credentials can help when passwordless SSO is unavailable;
server/domain policy can still reject authentication.

Open the session menu with Ctrl+Shift+F9. Minimize while staying connected with
Ctrl+Shift+F10. Quit with Ctrl+Shift+F12.
Reconnect with Ctrl+Shift+F11 using the same credentials and options; `/input`
replays with its delays. These shortcuts work
while the remote desktop window has keyboard focus. Startup text is sent once, incrementally, while the
client is focused and no keyboard key is held. CRLF is one Enter; Tab and
Backspace use scancodes. Supplementary Unicode characters use surrogate pairs.
Invalid, negative, and overflowing delay values are rejected.
Win+R, the Windows key, and Alt+Tab are forwarded to the remote desktop while
rdk has foreground keyboard focus. Menu, minimize, reconnect, and quit shortcuts
remain local; F9, F10 and F11 without both Ctrl and Shift are forwarded normally.

rdk remains borderless fullscreen but is no longer always on top. Local windows
can appear above it without minimizing or disconnecting rdk. Keyboard capture
still requires rdk to be foreground and focused; activating it again restores
remote input. Fullscreen sizing and the selected monitors are unchanged.

**Ctrl+Shift+F9** opens an on-demand **Session Menu** as a compact command strip
near the top center of the rdk window's visible area on the monitor containing
the pointer (or the first part of the rdk window if the pointer is elsewhere).
The strip has no title bar, uses equal-height controls, and wraps at narrow
widths. Minimize and Close use familiar symbols with hover tooltips.
It offers **Minimize**, **Send Ctrl+Alt+Delete**, **Reconnect**, and **Disconnect**,
all for that session only. Disconnect closes the client connection without
signing out the remote account. Reconnect retains the existing explicit
reconnect behavior, including `/input` replay. The menu is also available as
**Session Menu** in the individual window's classic taskbar menu shown with
Shift+right-click; it is not a group-wide Tasks command.

The menu uses native button controls with Tab/arrow navigation, flat system-color
drawing, and visible keyboard focus cues. Arrow keys cycle through enabled
commands, and **Enter activates the focused command**. **Close menu** (the X)
has initial focus and is the fallback if no enabled command is focused.
**Escape** always dismisses the menu without activating the selected command.
Close or switching to another window also
dismisses it. Held remote keys/buttons are released when it opens, remote
keyboard/mouse forwarding pauses, and startup input waits for focus to return.
The connection continues processing while the menu is open. There is no hover
activation or permanent overlay: the top strip appears only when requested.
Ordinary right-click and
Alt+Tab remain remote when rdk itself is focused; no windowed mode or local-input
toggle is introduced by this change.

Press **Ctrl+Alt+End** while rdk is focused to send **Ctrl+Alt+Delete** to the
remote session. Hold Ctrl and Alt before pressing the dedicated End key;
either side's modifiers work. End is not forwarded, and holding it does not
repeat the command. Numpad End/1 is unchanged. The remote security screen and
available actions depend on the server's Windows policies. Physical
Ctrl+Alt+Delete remains handled by local Windows; rdk does not inject it locally.

Alternatively, **Shift+right-click the individual rdk window's taskbar entry**
and choose **Send Ctrl+Alt+Delete** from the classic window menu. For grouped
windows, use the individual window thumbnail's menu. This command targets only
that window's session, works while unfocused or minimized, and does not reconnect
or show a confirmation dialog. It is not a Tasks jump-list command. It is useful
when an outer remote-desktop client intercepts Ctrl+Alt+End in a nested session.

To use the local screens without disconnecting, right-click the running rdk
taskbar button and choose **Minimize** under Tasks. This is a Windows jump-list
task, separate from the classic window menu shown by Shift+right-click (which
also offers Minimize). Restore rdk from the taskbar
to return to the same remote session. Minimizing releases captured input, pauses
startup input, and hides any device-change notice until the window is restored.
The remote connection stays open, and the window remains borderless when restored.
Ctrl+Shift+F10 performs the same minimize action; restoring resumes any paused
startup input without replaying it from the beginning.

The same Tasks menu also offers **Reconnect**, equivalent to Ctrl+Shift+F11.
It restarts the connection using the current credentials and options, replays
`/input` with its delays, and works even when rdk is minimized. An active call
will be interrupted; the remote session is not signed out.

Both tasks launch the windowless GUI helper `rdk-taskbar.exe`, avoiding a second
console/taskbar icon. Keep this helper beside `rdk.exe` when deploying the build.
The helper receives only `/minimize` or `/reconnect` and the client executable
path, never credentials or connection arguments. Each task applies to all
running rdk windows belonging to that same executable path, so multiple sessions
from that copy are minimized or reconnected together. Restart the rebuilt client
to update its task entries. Registration success/errors are logged; if the helper
is missing or shell policy prevents jump lists, Ctrl+Shift+F10/F11 remain available,
as does Minimize in the classic window menu.

The executable includes a multi-resolution taskbar icon (16-256 pixels): a blue
monitor and yellow connection arrow with a transparent background and screen
interior, rather than a solid square. Window icons are sized for display scaling.
The device-change notice uses themed
Windows controls, Segoe UI text, and a DPI-aware layout. Later remains the default
action, and appearing notices do not take focus. The icon can be regenerated
with `./tools/New-AppIcon.ps1`.

## Connection Recovery

Add `/recover` to retry an established session after a temporary network or VPN
interruption. It is opt-in; without it, connection errors retain the previous
exit behavior. Initial connection failures are not retried.

```powershell
.\build\Release\rdk.exe /v:HOST /recover
```

After a retryable failure is detected, rdk shows a native **Connection Recovery**
dialog with an attempt countdown and **Cancel**. Retry delays are 1, 2, 4, 8,
then 15 seconds, within a two-minute budget including connection attempts.
Cancel or closing the dialog stops retries and exits the client. The dialog
runs independently of the connection thread, so it can request cancellation
during a blocked attempt. FreeRDP and device teardown must still cooperate;
this is not a hard guarantee that all cleanup finishes within two minutes.

Recovery uses FreeRDP's in-place reconnect and enables its reconnect-cookie
support. It retains the window and in-memory connection settings. The server
decides whether the original session is still available. **Neither `/input`
nor `/text` is replayed or continued after automatic recovery**, even when the
outage interrupted startup input. Explicit Reconnect still replays `/input`.
Keyboard capture and remote mouse input pause during recovery; held remote
keys/buttons are released after connection restoration. No outage keystrokes
are queued for later delivery. Minimized windows stay minimized, and successful
recovery does not forcibly foreground rdk over another application.

Transport/connect, DNS, KDC-unreachable and activation-timeout failures may be
retried. Authentication, password/account, TLS/security negotiation errors,
server-supplied disconnect/logoff reasons, and intentional local quit do not
trigger retries. A non-retryable error during recovery ends it immediately.
Sleep/resume continues to use the separate confirmation flow below.

The FreeRDP overlay now applies its Windows TCP keepalive settings when
auto-reconnection is enabled: defaults are 5 seconds idle, 2 seconds between
probes, and 3 probes. This helps detect silent VPN drops instead of relying on
long Windows defaults. Detection time is separate from the retry budget and
depends on traffic, transport, Windows, and network behavior; it is not a
guaranteed outage-detection deadline. Gateway transports need live verification.
Keepalive configuration failures produce a warning. No VPN, firewall, or
system-wide TCP settings are changed, and an explicit disabled keepalive option
is respected.

Deploy the rebuilt files together from `build/Release`, including `rdk.exe`,
`freerdp3.dll`, `freerdp-client3.dll`, and `winpr3.dll`. An older FreeRDP DLL will
not contain the Windows keepalive fix. Recovery attempts and results are printed
without credentials or input content; the diagnostic report records recovery
entry, success, and the final reason if it stops. Audio/video calls and in-flight
clipboard transfers can still be interrupted; recovery does not replay them.

## Sleep And Resume

rdk registers Windows suspend/resume callbacks independently of its window's
message loop. Sleep, or a resume notification after a missed suspend, signals
FreeRDP's cancellation event so rdk stops using the interrupted connection.
The callback does not send network traffic, stop drivers, or wait for cleanup.
The normal session thread releases local input capture and tears down the old
connection, without sending remote key releases over the interrupted transport.

After cleanup and resume, rdk asks whether to reconnect. No is the default and
exits the client; Yes creates a fresh connection with the existing in-memory
credentials and settings. The remote session is not signed out. As with an
explicit reconnect, `/input` replays and `/text` does not. A failed reconnect
reports its error and exits; there is no automatic retry loop while the network
is still recovering. A pending local quit takes precedence over recovery.

The diagnostic report records the sleep/resume disconnect reason, teardown
stages, `waiting-for-resume`, and `awaiting-resume-reconnect-choice`. Registration
failure produces a warning. This does not forcibly terminate threads or guarantee
that an unresponsive audio/video driver will return from cleanup. If the client
still hangs, retain the last report lines and use Task Manager's **Create memory
dump file** on the hung process before ending it. Keep that dump private.

Tests simulate power notifications on a separate thread, exercise the actual
FreeRDP abort event and context replacement, and register/unregister the Windows
callback. They do not suspend the PC or exercise a live RDP server/device resume.

## Startup Input

To dismiss a remote dialog after connecting, wait one second and press Enter:

```powershell
.\build\Release\rdk.exe /v:HOST /input:"<delay><enter>"
```

`<delay>` is a fixed one-second pause; repeat it for a longer wait. For example,
`/input:"<delay><delay><enter><F1>"` waits two seconds, then presses Enter and
F1. Delays do not block the network or window event loop.

Tokens are case-insensitive. Supported keys are `<enter>`/`<return>`,
`<esc>`/`<escape>`, `<tab>`, `<backspace>`, `<space>`, `<F1>` through `<F12>`,
`<up>`, `<down>`, `<left>`, `<right>`, `<home>`, `<end>`, `<pageup>`, `<pagedown>`,
`<insert>`, and `<delete>`. Every key is pressed and released. Ordinary text
can appear between tokens; `<<` types a literal `<`. Unknown or malformed tokens
are rejected before connection.

`/input` has no implicit initial delay. An explicit `/startup-delay:MS` adds
an initial wait, and `/char-delay:MS` spaces out characters/keys. `/text`
remains literal and retains its default three-second initial delay; it cannot
be combined with `/input`, including when using compatibility aliases.

`/input` runs once per connection, including Ctrl+Shift+F11 and Reconnect in the
device-change notice. Each connection restarts the full sequence and its delays,
even if the previous connection ended partway through. `/text` runs only on the
first connection. Initial delays start only after the session is active, a first
desktop frame has arrived, rdk has focus, and physical keys have been released.
This includes shortcut keys still held across a fast reconnect. Both pause
while rdk is unfocused or a physical key is held. Stdout logs when replay is
queued, started, and completed, without printing the input contents.
The remote dialog must have focus when the keys are sent: there is no dialog
detection, and a fixed wait cannot guarantee it has appeared. This is not for
dismissing local credential prompts or dialogs shown before the session connects.

## Keyboard Behavior

- Left Alt followed only by physical numpad digits is buffered locally. On Alt
  release, the character is sent as a Unicode down/up pair. No Alt or numpad
  event for that character is sent to the server.
- Decimal codes with a leading zero use the active keyboard language's ANSI
  code page; codes without it use its OEM code page, including OEM glyphs.
  Values wrap to one byte, matching traditional byte-based Alt codes.
- Left Alt, numpad Plus, then hexadecimal digits accepts Unicode scalar values
  through U+10FFFF. This client supports that form without a registry switch.
- Ordinary typing, dead keys, Ctrl shortcuts, and right Alt/AltGr remain
  scancode input. A left-Alt shortcut is replayed in order as soon as it is
  distinguishable from a character sequence. A standalone Alt is replayed on
  release. Mouse button/wheel gestures also flush pending Alt input.
- Coalesced key repeat counts are preserved. Extended navigation keys are not
  mistaken for numpad digits; NumLock, right Shift, and Pause have Win32 fixups.
- Num Lock and Caps Lock update local Windows normally while each event is
  forwarded once to the remote session. Keyboard indicators and firmware that
  use the local host's lock state can therefore follow changes made in rdk.
  Returning to rdk synchronizes the remote locks to the current local state.
  Remote Num Lock/Caps Lock indicator changes update local state only while
  rdk is focused, after queued input and held keys have been handled. These
  local updates are tagged so they are not forwarded back to the server.
  Focus loss or newer local lock-key input cancels a pending indicator update.
  Windows can block local input injection; a warning is printed if a remote
  indicator update cannot be applied. Scroll Lock handling is unchanged.
- Focus loss cancels incomplete composition and releases forwarded keys and
  mouse buttons. Quit releases held keyboard keys before disconnecting.
- An invalid sequence or exhausted 128-event buffer falls back to ordered raw
  events, without silently truncating the sequence. Input send failures stop
  the client with a nonzero exit code.

The window becomes visible only after the RDP session is active. Ordinary
scancode input initially advertises the local keyboard layout unless `/kbd`
overrides it. Decimal composition code pages follow local input-language
changes; the remote session's layout remains under the remote OS's control.
rdk enables Unicode input before connection for Alt codes and `/text`, after
FreeRDP's command-line parser resets that setting. No `/kbd:unicode` option is
needed. This does not switch ordinary keys or shortcuts away from scancodes.

## Cursor Behavior

Server cursor shapes use native Windows cursors with their supplied hotspots.
Monochrome masks preserve transparent and inverted pixels; color cursors support
alpha transparency. Cached shapes and hidden/default cursor updates are handled,
and moving the mouse does not reset the selected remote shape to the local arrow.
Server requests to reposition the local pointer are not implemented.

## Clipboard

Clipboard redirection is enabled by default. To enable it explicitly:

```powershell
.\build\Release\rdk.exe /v:HOST +clipboard
```

Copy a file or folder in remote Explorer, switch to local Explorer, and paste
into the destination folder while the session remains connected. Local files
can also be copied into remote Explorer. No drive sharing is required. Text,
HTML, RTF, and common bitmap/PNG clipboard formats are supported in both
directions. Clipboard sharing exposes copied content to the other machine;
disable it with `-clipboard` when it is not needed.

FreeRDP direction controls are honored. For remote-to-local copying only, use
`/clipboard:direction-to:local,files-to:local`. To allow text and images but
disable file copying, use `/clipboard:files-to:off`. The server's clipboard and
file-redirection policies must also permit the transfer.

Files are streamed on demand in bounded chunks through a Windows OLE data
object. Copy is supported, not cut/move: source files are not deleted. Replacing
the clipboard or disconnecting invalidates outstanding remote objects; let a
paste finish before copying something else or reconnecting. A failed or timed-out
transfer is reported to the destination application, which may leave a partial
destination file. A timed-out format request requires reconnecting to avoid
mistaking a late reply for a subsequent request.

Limits: 10,000 files per selection, relative names shorter than 260 UTF-16 code
units, and 16 MiB per non-file clipboard payload. File contents are not subject
to the 16 MiB limit. Unsafe relative paths, reparse points/junctions, and changed
local source files are rejected. Cloud-only or application-specific virtual
files on the local machine are not exported; use ordinary files in Explorer.

## Limits

This removes the server's dependency on reconstructing a rapid Alt+numpad
sequence. It does not prove that RDP itself drops or reorders packets, nor does
it guarantee that every application accepts Unicode keyboard events. The
server must advertise Unicode input; applications that require raw key events
may need different behavior. Disconnects cannot provide exactly-once delivery.

The client uses a low-level keyboard hook on a dedicated message-loop thread.
It intercepts keys only while its window is foreground and focused, suppresses
local shortcut handling, and queues events in order to the RDP window thread.
Network sends never run inside the hook callback. Focus changes invalidate
queued keys from the previous focus period. Hook/queue failures are reported,
and capture is removed on disconnect. Other applications' input is left alone.
Windows-protected combinations such as Ctrl+Alt+Delete and Win+L are not
guaranteed to be redirectable. IME/local WM_CHAR or VK_PACKET injection routing
is not implemented; ordinary text keys
use the remote layout, and `/text` provides explicit Unicode injection.
Byte-based decimal conversion is intended for single-byte legacy code pages;
unconvertible multibyte input falls back to raw events. Layout-specific or
application-specific Alt-code extensions may differ from these rules.

## Microphones and Cameras

Audio and camera access are opt-in:

```powershell
.\build\Release\rdk.exe /list:microphone
.\build\Release\rdk.exe /list:camera
.\build\Release\rdk.exe /v:HOST /screen:1,2 /microphone /sound /camera
```

`/microphone` forwards Windows' default communications recording device.
Set the intended microphone as that default in Windows Sound settings. Teams
on the remote machine normally uses a redirected audio endpoint rather than
the physical headset's name. `/sound` plays remote audio locally.

At connection time, stdout reports whether microphone and camera redirection
were requested, microphone names/states, the default communications microphone's
mute/volume, available camera names, and whether change monitoring started.
Audio endpoint changes are logged after a 1.5-second debounce even when the default
microphone stays the same (and therefore no reconnect popup is needed). Camera
inventory changes and Reconnect/Later/Close decisions are also logged. Camera
capture start/stop messages identify remote stream requests. Output is flushed
so these messages are visible promptly when stdout is redirected to a file.

`/list:microphone` lists local recording devices and queries mono PCM format
support without recording. It can be combined with `/list:camera`. Detection
and format support do not verify capture permission or successful delivery to
the remote application. Errors remain on stderr; use `*> rdk-media.log` in
PowerShell to collect both stdout and errors.

For silent input, first verify that the selected microphone works in local
Windows Sound settings, is unmuted, and is allowed under Windows microphone
privacy settings for desktop apps. Select it as the default communications
recording device, then reconnect rdk. If the local diagnostic itself shows
`Remote Audio`, rdk is seeing an upstream redirected microphone; that upstream
connection must supply working audio first. The remote `Remote Audio` device
name alone does not establish that samples are arriving.

`/camera` enables experimental Windows Media Foundation webcam redirection
using FreeRDP's RDPECAM protocol. It advertises all locally enumerated cameras;
omit it when camera access is not needed. `/list:camera` and device-change
monitoring enumerate metadata only. Remote activation/format queries may open
the camera, and frame capture starts only when the remote application requests
a stream. Stopping the stream or disconnecting releases the capture source.

Supported modes are native MJPEG, YUY2, NV12, and I420 up to 1920x1080 at 30 fps.
No H.264 conversion or Teams-specific media optimization is provided. Cameras
without a compatible native mode are rejected; raw YUV modes can use substantial
bandwidth. Camera controls such as zoom/exposure are not implemented. Windows
camera/microphone privacy permissions must allow desktop applications, and the
remote host must support and permit audio recording/video capture redirection.
In remote Teams, select the redirected camera and microphone in Device settings.

With redirection enabled, a default-microphone change, removal of the connected
microphone, or a camera-set change produces a debounced Reconnect/Later notice.
The notice does not take focus or stop the running session. Later/Close dismiss
it; Reconnect disconnects and establishes a fresh connection using the same
in-memory credentials and options. It replays `/input` from the beginning, but
does not replay `/text`, sign out, or save new credentials.
Reconnection can interrupt a meeting and depends on the
server reconnecting the same user session. Device changes do not automatically
reconnect; `/recover` applies only to retryable connection interruptions, not
device configuration changes. Some driver or FreeRDP audio errors can still end the connection
before a device-change notice can be shown.

Exit and reconnect print flushed `rdk: shutdown:` progress messages around
keyboard cleanup, clipboard shutdown, RDP disconnection, and context release.
Camera stop requests are logged before waiting for capture to stop. If the
client hangs during exit, retain the last few messages to identify the blocked
stage. Audio/video driver shutdown and live disconnects still require testing
on the machine running the connection.

The camera protocol is built from SHA512-pinned FreeRDP 3.26.0 sources downloaded
by CMake into `build/_deps` (user-approved dependency location). The existing
vcpkg installation is not modified. The Windows capture backend requires the
Visual Studio C++ compiler and Windows SDK Media Foundation libraries. FreeRDP
protocol sources retain their Apache-2.0 notices; the build places their license
beside `rdk.exe` as `FreeRDP-camera-LICENSE.txt`. Ship that file with the client.

## Microphone Channel-Mixing Fix

The local FreeRDP overlay fixes a reproduced microphone crash in
`waveInProc -> audin_receive_wave_data -> freerdp_dsp_ffmpeg_encode -> av_samples_copy`.
When converting mono capture to stereo, FreeRDP 3.26.0 mixed the samples but passed
the original mono format to FFmpeg. The encoder then copied a nonexistent second
audio plane, causing a null-pointer read in the C runtime. The patch uses the
mixed format for both frame creation and resampling; it also corrects the inverse
stereo-to-mono case. No microphone, playback, camera or AVC feature is disabled.

The fix is in the rebuilt `freerdp3.dll`, not just the client executable. Copy the
matching rebuilt `Release` files to the machine running rdk. A synthetic AAC
regression test reproduced the original access violation and passes with the
patch; live microphone/server verification is still needed on that machine.

## Exit Reasons And Crash Reports

rdk prints `rdk: exiting: REASON (exit=N)` before cleanup, or `rdk: reconnecting:`
for a requested reconnect. Reasons distinguish local quit/close requests, input
or event-processing failures, connection failures, and server disconnects.
FreeRDP errors and server disconnect reasons include numeric codes, symbolic
names, and descriptions. If neither supplies a reason, the report says so;
this alone cannot identify a network outage or remote crash.

Each invocation also writes a small UTF-8 report under
`%LOCALAPPDATA%\rdk\diagnostics`. The console prints its exact path. The report
records UTC lifecycle stages, exit reasons/codes, the executable path/build time,
and, on a caught fatal native exception, its type, address, thread and module
offset, plus up to 32 stack frames from the faulting context. Frames contain code
addresses and module offsets, not arguments or local variables; no PDB files or
symbol downloads are needed to capture them. An unreadable stack stops the walk
without discarding the initial exception report. Preserve the matching binaries
to resolve offsets to functions later. It is not a copy of all console/FreeRDP output. It excludes command-line
arguments, credentials, keystrokes and clipboard contents. File paths can reveal
local account names; review the report before sharing it. Files are retained
until manually deleted. Failure to open a report prints a warning and leaves
console reporting available.

Add `/crash-dump` to the usual command to also attempt a `.dmp` minidump beside
the report on a fatal exception. Dumps are **off by default**: even a small dump
can contain credentials, typed text, clipboard data or remote-session content.
Keep dumps private; do not upload them indiscriminately. Release builds include
`rdk.pdb`; preserve the matching executable, DLLs and symbols from the failing
build for debugging. Start by sharing the text exit reason/error lines instead.

Fatal exceptions terminate the process; rdk does not continue with potentially
corrupted state. In-process reporting is best-effort, including on worker
threads. Fail-fast/CRT aborts, severe stack or heap corruption, forced process
termination, and failures before entry (such as missing DLLs) can bypass it or
prevent dump writing. A report ending without an exit/fatal record only shows
the last known stage, not the cause. Windows Error Reporting or Event Viewer's
Application Error/Windows Error Reporting entries are the next source of
evidence in that case. No Windows logging policy or registry settings are changed.

## Diagnosing Input Lag

Add `/latency` to your existing command and reproduce the lag for 20-30 seconds
with ordinary typing and Alt+Tab. Keep the same codec and monitor settings during
the measurement. The diagnostics are off by default and do not change input
ordering, Alt-code composition, or the rendering backend.

Every five seconds, stdout receives a `rdk: latency` summary with `n` (sample
count), `avg`/`max` durations in milliseconds, and `slow` (samples at least 50 ms).
Only aggregate timings/counts are retained; no key values, typed text, clipboard
contents, or credentials are included in these summary lines. Reports reset the
counters, and reconnect starts a fresh measurement. A stalled main loop cannot
print until it resumes, so a longer `interval` is significant, though the first
interval also includes connection setup.

| Field | Measures | What a high value suggests |
| --- | --- | --- |
| `hook` | Windows keyboard event timestamp to rdk's hook | Input hook scheduling or another upstream hook |
| `queue` | Posted keyboard message timestamp to main-loop dequeue | rdk's main loop is not servicing input promptly |
| `send` | FreeRDP keyboard-send call duration | Contention or blocking inside the client/transport send path |
| `events` | FreeRDP event processing | Network/protocol/channel work is keeping the loop busy |
| `decode` | GFX surface-command processing, including decode/conversion | Expensive graphics work; legacy non-GFX updates are not measured here |
| `paint` | GDI framebuffer blit | Slow local presentation |
| `devices` | Microphone polling and camera inventory/notice handling | Device queries or notification work |

Hook/queue timestamps have Windows millisecond-timer granularity (often around
10-16 ms); tiny differences are not meaningful. Other durations use the performance
counter. Timing areas can overlap and run on different threads: do not add the
figures together or treat separate maxima as proof of a shared cause. Idle waits
are not counted. `n=0` means no samples, not a measured zero latency. The Alt
composer's intentional wait for more keys is not included in queue/send timings.

Large queue spikes during the symptom identify a client-side delay, even if local
applications remain responsive. Low hook/queue/send values during visible lag shift
attention to delivery after the send call, the remote desktop/application, or the
return display path; a fast send is not a remote acknowledgement. These are not
end-to-end input latency, network RTT, or server CPU measurements.

For a useful report, retain a few `rdk: latency` lines from both normal and laggy
periods plus the `graphics received:` lines. Avoid per-key/TRACE logging while
measuring. A blocked console or log destination can itself delay the main loop;
the diagnostics write only one bounded summary per interval. A comparison with
Microsoft's client against the same host, layout, and workload can further
separate an rdk-specific problem from a shared remote/network problem.

## Verification

With `BUILD_TESTING` enabled (the default), CTest registers `recovery`, `power`, `audio_encode`, `crash`, `latency`, `keyboard`, `startup`, `cli`,
`graphics`, `credentials`, `capture`, `pointer`, `display`, `media`, `media_log`, `camera`, `camera_mf`,
`session`, `window`, `taskbar`, `clipboard_files`, `clipboard`, and `clipboard_native`.
Recovery tests check transient/terminal error classification, server disconnect
precedence and backoff with a fake clock. Private-desktop native dialogs exercise
Cancel during a simulated connection attempt, deadline signaling, successful
recovery after a transient failure, and abort-handle lifetime. Loopback-only
FreeRDP connection attempts verify that the patched DLL sets and reads back
the Windows keepalive values when enabled, and does not apply them by default.
Window tests verify input gating while recovery is active. No real server login
or VPN changes occur in these tests. Live verification requires dropping and
restoring the VPN after a connection with `/recover`, checking normal input and
unchanged startup text, then repeating with Cancel and a remote logoff. Verify
media and clipboard behavior separately if those channels are enabled.
The audio encoder test uses synthetic silence at 44.1 kHz and the deployed
FreeRDP/FFmpeg DLLs to verify mono/stereo AAC channel conversion and unchanged
channel counts over multiple packets. It opens no microphone or network session.
The keyboard executable uses an injected output recorder, not an RDP server.
Crash tests launch disposable child processes with synthetic main/worker-thread
exceptions, validate report content and the minidump exception stream, and check
ordinary nonzero exits and unavailable report paths. CLI tests check persistent
exit reasons and argument privacy. They do not crash a real client session.
The latency tests use synthetic timings to verify aggregation, slow thresholds,
five-second reporting, counter reset, timestamp wrap, concurrent updates, summary
formatting, and disabled-mode silence. Capture tests run with timing both disabled
and enabled. These tests do not record real keystrokes or reproduce live latency.
It checks ANSI/OEM and hexadecimal composition, Unicode pairs, shortcut replay,
AltGr, focus cleanup, repeats, special keys, early Alt release, bounded-buffer
fallback, send failures, startup text, and 250 rapid sequences. The CLI suite
checks missing values, invalid delays, and accepted numeric boundaries.
The graphics suite checks the linked H.264/FFmpeg build, decoder initialization,
AVC444/AVC420/native non-AVC options, unchanged non-AVC defaults, and graphics
settings preservation when cloned for reconnect. Window tests verify that codec
logging forwards commands and decoder failures without changing rendering.
These checks do not establish live server negotiation, text quality, or hardware
encoding; those require a connected comparison as described above.
Startup tests record scan-code and Unicode events without sending real input.
They check fixed-delay tokens, named key pairs, extended keys, escaped literals,
UTF-16, held-key gating, and cleanup after send failure. Fake-clock scheduling
checks cover repeated F11 reconnects, reset after partial replay, readiness-based
initial/token delays, pause/resume, and literal text. CLI tests verify valid
and malformed sequences, conflicting startup options, slash syntax, compatibility
aliases, help output, and literal `/text` behavior. Dismissing an actual remote
dialog still requires a live session.
The keyboard suite also runs the real FreeRDP command-line parser and verifies
that pre-connect keyboard setup enables Unicode while preserving an explicitly
selected keyboard layout.
The credential suite uses mock Windows APIs to verify remote usernames,
cache precedence, prompt cancellation, save-after-success, failure cleanup,
and corrupt entries. It does not access real saved passwords or verify NLA.
The capture suite checks Win+R ordering, foreground scope, stale-event rejection,
Alt+Tab, the local exit chord, injected scancodes, repeat events, and queue
failure. Num Lock/Caps Lock tests check local passthrough, single remote
forwarding, and tagged indicator updates without feedback. Window tests cover
indicator batching, held-key deferral, focus generations, and cancellation by
new local input. Hook installation/removal is smoke-tested with capture
inactive; no real keyboard input is recorded or injected by these tests.
Live verification still requires pressing both locks while focused on rdk,
checking the keyboard LEDs and remote typing, then switching to a local app
and back. Also check remote on-screen keyboard toggles while rdk is focused.
Session-menu tests check both Ctrl/Shift sides and modifier subsets, repeat
suppression, remote key releases, and ordinary F9 passthrough. Native private-
desktop checks cover a local window above non-topmost rdk, the actual dialog
resource and text/glyph fit at 96/144/192 DPI, top anchoring, equal-height controls,
narrow wrapping, tooltips, hover, Tab/Enter/Escape, command routing, input isolation, dismissal,
minimized entry, and recovery/shutdown cleanup. Live taskbar interaction and
switching between real local applications and an RDP session still need a user
check; no real keyboard events are injected by these tests. Run the window test
with `--preview-menu` from the build directory to write native bitmap previews.
Native icon checks require transparent borders and screen interior plus visible
artwork in every tested size.
Ctrl+Alt+Delete regressions record the exact remote scan sequence, modifier
preservation, End repeat/release suppression, numpad passthrough, and send-failure
cleanup. Capture tests check foreground gating and stale focus messages. Native
window tests verify the per-window menu with two separate input contexts,
including unfocused/minimized dispatch and shutdown/failure handling. These tests
do not open a real Windows security screen; verify both entry points in a live
session and press Escape to return, then check normal typing and shortcuts.
The pointer suite exercises registered FreeRDP graphics callbacks without a
connection. It checks native cursor pixels, hotspots, padded masks, transparency,
cached selection, hidden/default states, invalid shapes, and handle cleanup.
It simulates cache replacement; protocol dispatch still requires a live session.
The display suite tests Windows display-number mapping, nonsequential IDs,
external-only and single-display layouts, the all-display default, primary
selection, negative origins, differing resolutions, and invalid selections.
CLI tests check monitor listing and the `/screen` alias without connecting.
Media tests cover default-device changes, removal/reconnection, debounce,
duplicate suppression, and real notification registration without audio capture.
They also check native microphone metadata/format queries and debounced stdout
messages, including non-default endpoint changes and no repeated reconnect logs.
Camera tests use synthetic media types and a synthetic camera to exercise the
real RDPECAM handshake, device announcement, format negotiation, exact frame
payload, and stop. Session tests verify fresh-context settings preservation and
non-activating Reconnect/Later/Close controls, modern-control activation, icon
dimensions/pixel coverage, and text/layout bounds at 100%, 150%, and 200% scaling.
The keyboard tests cover reconnect/minimize chords with either side's modifiers,
missing modifiers, remote-key cleanup, F10 repeat suppression, and unchanged quit behavior.
The window test uses the real window procedure on an isolated desktop to check
system-menu minimize/restore, unchanged borderless bounds, remote-key release,
keyboard capture state, and automatic hiding/restoring of the owned notice.
It also exercises Ctrl+Shift+F10 through the actual input handler, ensuring no
quit/reconnect request and no F10 event forwarded before or after restoring.
It launches the real GUI helper on the private desktop to check minimize and
reconnect, including reconnect while minimized, repeated requests, preserving
an existing quit request, invalid actions, and executable-path filtering.
The taskbar test checks both native shell links' titles, executable paths,
credential-free arguments, and the helper's GUI PE subsystem (no console).
Running `rdk_taskbar_test --publish` additionally publishes and removes
a uniquely named test jump list through the Windows shell API; the normal test
does not modify jump lists. Actual Explorer menu display and remote dialog timing
still require live verification.
None of these tests record audio
or video, connect to an RDP server, or establish Teams compatibility.

Clipboard tests validate file descriptors and safe paths, Unicode text, chunked
file reads, seek/EOF behavior, oversized/failed responses, direction restrictions,
clipboard replacement, and disconnect cancellation with active and queued readers
across repeated shutdowns. The native suite uses a
private Windows window station and desktop to verify actual OLE clipboard
publication, cross-apartment file streams, and local file export without
replacing the user's clipboard. Live remote Explorer interoperability and
server policy still require a connected session: copy text and a file/folder
in each direction, compare file contents, and check cancellation during a paste.

For live media verification, use the command above and remote Teams Device
settings to confirm audio input and camera preview. Stop/restart preview and
check that the camera is released. Add or change a microphone/camera during
the session: confirm Later preserves the session and Reconnect makes the new
device available, replays `/input` with its delays, and does not replay `/text`.
Also test removal during
active capture, Windows privacy denial, and a server with redirection disabled.

For end-to-end testing, connect to the intended Windows host, open Notepad,
and focus its editing area:

1. With a Western keyboard language, run firmware macros for Alt+0169
   (U+00A9), Alt+0233 (U+00E9), and Alt+Plus 20AC (U+20AC).
2. Repeat at the firmware's maximum speed, interleaving ordinary text; compare
   the complete output and character count with a local Notepad run.
3. Verify real Alt-menu shortcuts, Ctrl+C/V, AltGr, NumLock, and navigation keys.
4. Switch away while holding Ctrl/Shift or midway through an Alt code; return
   and verify there are no stuck modifiers or partial composed characters.
5. Repeat in the actual target application, including elevated UI if needed.
  Confirm focus-loss and quit behavior while a long `/text` run is active.
6. Hover over a text field, a link, and a resizable window edge. Verify the
  I-beam, hand, and resize cursors appear and persist as the mouse moves.
7. Connect with `/screen` selecting only the external monitors. Compare the
  numbers with Windows Identify, confirm the laptop screen is untouched, and
  check remote resolution, maximized windows, and mouse alignment on both
  selected displays. Reverse the selection to test remote primary selection.

Live firmware, remote application, elevated UI, and multi-monitor visual
verification require an actual remote session; mocked input tests do not
substitute for those checks.