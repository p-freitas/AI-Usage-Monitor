# AI Usage Monitor project memory

When the maintainer requests a signed release, bump `res/version.h`, test and commit the release on `main`, and push `main` and the matching `v<version>` tag. Wait for GitHub Actions to build the draft. Sign its exact checksum file with the existing Windows release key, verify the uploaded installer, checksum and signature, then publish the draft. Follow [the release runbook](docs/releasing.md) for the commands and verification steps.
