# Release process

Releases are authorized only by a verified, signed, annotated `vX.Y.Z` tag.
The tag version must equal both `VERSION` and the CMake project version.

## Qualification

From a clean checkout, run every gate in a distinct build root:

```sh
make check
make asan-ubsan
make tsan
make fuzz-smoke
make install-check
```

Then verify the real confinement path against the pinned Warden release:

```sh
make -C /path/to/maelys-warden code-runner-e2e \
  CODE_RUNNER="$PWD/build/release/maelys-code-runner"
```

The integration must report `seatbelt` on macOS or `bubblewrap` on Linux.
The generic POSIX backend does not qualify a production release.

Update `VERSION`, `project(... VERSION ...)`, `CHANGELOG.md` and the release
notes together. Dependency changes additionally require immutable commit pins
and an updated `THIRD_PARTY_NOTICES.md`.

## Local artifact check

```sh
make package-release TARGET=macos-arm64
shasum -a 256 -c dist/maelys-code-runner-*.sha256
```

`scripts/package-release.py` builds from source, installs through CMake and
creates a deterministic archive containing the binary, documentation and all
license texts.

## Authorization ceremony

After the protected branch and CI are green:

```sh
version="$(cat VERSION)"
git tag -s -a "v${version}" -m "maelys-code-runner ${version}"
git verify-tag "v${version}"
git push origin "v${version}"
```

The release workflow independently verifies the tag through the GitHub API,
builds all supported targets, attests the archives, checks every sidecar hash
and publishes the GitHub Release. A lightweight or unverified tag fails closed.
