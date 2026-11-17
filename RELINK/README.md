# LGPL relink / source offer (FriBidi)

`libavfilter.so.12` statically links FriBidi 1.0.16 (LGPL-2.1-or-later).
The source archive and the exact cross-build entry point are kept here so a
recipient can rebuild/replace FriBidi and relink the FFmpeg libraries.

## Source

- `RELINK/src/fribidi-1.0.16.tar.xz`
- Upstream: https://github.com/fribidi/fribidi/releases/tag/v1.0.16
- SHA-256: see `RELINK/src/SHA256SUMS`

## Rebuild (both OHOS ABIs)

```bash
# from the repo root, MSYS2 bash
bash build/make-subs-wrappers.sh
bash build/build-fribidi-subs.sh
```

The script installs `libfribidi.a` into `prebuilt/<abi>/subs/lib/`. FFmpeg is
then relinked with the same `build/build-ffmpeg.sh` invocation used for the
shipped binary. The repository contains the HarmonyOS app/native source and
the FFmpeg build scripts; no GPL component is involved.

The current release is not tagged v0.3.0 yet; this directory is the source
offer prepared before the final release gate.

## Application-side relink objects

RELINK/obj/arm64-v8a/ contains the ten libentry.so object files produced by
the DevEco release build, plus link-command.txt.

Procedure:

1. Rebuild libavfilter with the replacement FriBidi static library.
2. Copy the new libavfilter.so.12 into the FFmpeg libs directory.
3. Edit paths in RELINK/obj/arm64-v8a/link-command.txt if needed.
4. Run the command from RELINK/obj/arm64-v8a/ to relink libentry.so.

The object set is app-side (
api_init, ideo_transcode, etc.);
libavfilter itself is relinked separately with the FriBidi source/build script.
