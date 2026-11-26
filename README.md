# Zip Unzipper for Flipper Zero

Zip Unzipper is an offline Flipper Zero external application. Select a `.zip`
file from the SD card and choose to extract beside it or select another
destination. For another destination, select any file inside the desired
folder. Then choose to put the archive contents directly in that destination
or in a new folder named `<archive>_unzipped`. Existing files are never
overwritten, including when extracting directly into the destination.

The app supports ordinary ZIP archives containing stored (method 0) and
DEFLATE (method 8) files. Encrypted, split, ZIP64, and other compression
formats are not supported. Each file is limited to 64 MiB and the total uncompressed
content is limited to 128 MiB. Unsafe absolute paths and `..` path components
are rejected.

## Build

Place this folder under `applications_user/zip_unzipper` in a Flipper Zero
firmware checkout, then build with:

```powershell
./fbt fap_zip_unzipper
```

The generated `.fap` can be installed from the SD card using the Flipper
Zero's Apps installer. The app performs all archive reading and extraction
locally and does not use a network connection.

## Native extractor tests

On Windows with Emscripten and Node.js available, run
`.\tests\native\run.ps1` from this folder. These tests compile the extractor
against a small storage mock and exercise real ZIP archives; they do not
replace building and testing the `.fap` on Flipper Zero hardware.
