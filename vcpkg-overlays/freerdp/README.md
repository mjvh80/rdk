# FreeRDP Overlay

Snapshot of the Microsoft vcpkg `freerdp` 3.26.0 port used by this project.
The stock port configuration and its four dependency/build patches are retained.
The copied port files are covered by `vcpkg-LICENSE.txt`; FreeRDP's own license is
retained by the port and installed with the package.

Local changes:

- Port revision 1.
- `audio-channel-mix.patch`: pass the channel mixer's `fmt` to input-frame creation
  and resample-frame setup instead of the original capture format.

The unpatched FFmpeg encoder can dereference a null second sample plane when
mono PCM capture is converted to stereo. The public-API regression in
`tests/audio_encode_test.c` reproduces that crash with synthetic samples and tests
both conversion directions plus unchanged channel counts after the patch.

`vcpkg-configuration.json` enables this overlay for normal manifest builds.
Reassess the patch when updating FreeRDP; do not silently drop it or edit only a
generated build-tree source. `cmake/Camera.cmake` also pins FreeRDP 3.26.0 for its
private camera ABI and must stay aligned with any dependency version update.
