# FreeRDP Overlay

Snapshot of the Microsoft vcpkg `freerdp` 3.26.0 port used by this project.
The stock port configuration and its four dependency/build patches are retained.
The copied port files are covered by `vcpkg-LICENSE.txt`; FreeRDP's own license is
retained by the port and installed with the package.

Local changes:

- Port revision 2.
- `audio-channel-mix.patch`: pass the channel mixer's `fmt` to input-frame creation
  and resample-frame setup instead of the original capture format.
- `windows-keepalive.patch`: apply and read back TCP_KEEPIDLE, TCP_KEEPINTVL and
  TCP_KEEPCNT on Windows when keepalive and auto-reconnection are enabled. This
  honors the existing FreeRDP settings, warns on failure, and leaves the default
  non-recovery path unchanged. Requires a Windows version supporting these
  socket options (Windows 10 1709 or newer); rdk already uses modern Windows APIs.

The unpatched FFmpeg encoder can dereference a null second sample plane when
mono PCM capture is converted to stereo. The public-API regression in
`tests/audio_encode_test.c` reproduces that crash with synthetic samples and tests
both conversion directions plus unchanged channel counts after the patch.

The recovery regression makes loopback-only FreeRDP connection attempts and
captures the debug message emitted after native socket option readback, testing
both opt-in and default behavior. FreeRDP 3.26's TransportAttach override is not
used by its normal connection path, so applying timings only in a client-side
TransportAttach hook does not work. Ship the rebuilt FreeRDP DLLs with rdk.

`vcpkg-configuration.json` enables this overlay for normal manifest builds.
Reassess the patches when updating FreeRDP; do not silently drop them or edit only a
generated build-tree source. `cmake/Camera.cmake` also pins FreeRDP 3.26.0 for its
private camera ABI and must stay aligned with any dependency version update.
