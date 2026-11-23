# Releasing AI Usage Monitor

GitHub Actions builds and tests the Windows monitor. Tags in the form `v1.2.3` on the official repository produce a draft with the Inno Setup installer and its `SHA256SUMS.txt`. Review the Windows build and verify that its version matches the tag.

## Prepare the release

Update both version definitions in `res/version.h`, build and run `make test`, then commit the release on `main`. Push the commit and the matching tag together. Set `$version` to the version in the header.

```powershell
$tag = "v$version"
git tag -a $tag -m "AI Usage Monitor $version"
git push --atomic origin main "refs/tags/$tag"
```

Wait for the **Windows build and release** workflow for the tag to succeed, including the `draft-release` job. Confirm the tag points to the release commit and the release is still a draft.

## Sign the files for the built-in updater

The updater trusts an ECDSA P-256 signature of `SHA256SUMS.txt`. Signing is free and uses the Windows key store under the signing user's account. Run this once on the maintainer's Windows account to initialize the key and write its public half to the repository:

```powershell
.\scripts\release-signing.ps1 -Initialize
```

Commit the public header before tagging the first release. The private key is non-exportable and stays in the Windows account that created it. Use that same account and signing PC for future releases. If the key becomes unavailable, ship a version with a new trusted public key for users to install manually. While the old key is still available, a release can add a new public key and be signed with the old key to allow rotation.

Download the installer and `SHA256SUMS.txt` from the draft into a fresh directory. On the Windows account holding the existing trusted key, sign the downloaded files:

```powershell
$repo = "p-freitas/AI-Usage-Monitor"
$installer = "AIUsageMonitor-Setup-$version.exe"
$folder = Join-Path $env:TEMP "AIUsageMonitor-$tag"
gh release download $tag --repo $repo --dir $folder --pattern $installer --pattern SHA256SUMS.txt
$checksums = Join-Path $folder "SHA256SUMS.txt"
.\scripts\release-signing.ps1 -Installer (Join-Path $folder $installer) -Checksums $checksums
gh release upload $tag "$checksums.sig" --repo $repo
```

The script verifies the installer version, checksum and signature against the trusted signing key. Before publishing, compare the SHA-256 digests of all three local files to the draft's uploaded assets (`gh api "repos/$repo/releases/<release-id>/assets"`). After the digests match, publish with `gh release edit $tag --repo $repo --draft=false --latest` and confirm GitHub lists the release as published with all three files. For older versions and releases missing a valid signature, users can download the installer from the release page.

## Authenticode publisher signature in Windows

Authenticode uses a separate code-signing identity. For a free trusted certificate, open-source maintainers can [apply to SignPath Foundation](https://signpath.org/apply.html). Approval is handled by SignPath. [Eligibility and terms](https://signpath.org/terms)

Once a signing identity is available, sign the application and configure Inno Setup with `AuthenticodeSignTool` to sign the installer and uninstaller during packaging. Verify the signature and timestamp on the finished package. Generate and sign the checksums again after any signing step changes the installer, before publishing. [Inno Setup SignTool](https://jrsoftware.org/ishelp/topic_setup_signtool.htm) · [Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)
