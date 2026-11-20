# Releasing AI Usage Monitor

GitHub Actions builds and tests the Windows monitor. Tags in the form `v1.2.3` on the official repository produce a draft with the Inno Setup installer and its `SHA256SUMS.txt`. Review the Windows build and verify that its version matches the tag.

## Sign the files for the built-in updater

The updater trusts an ECDSA P-256 signature of `SHA256SUMS.txt`. Signing is free and uses the Windows key store under the signing user's account. Run this once on the maintainer's Windows account to initialize the key and write its public half to the repository:

```powershell
.\scripts\release-signing.ps1 -Initialize
```

Commit the public header before tagging the first release. The private key is non-exportable and stays in the Windows account that created it. Use that same account and signing PC for future releases. If the key becomes unavailable, ship a version with a new trusted public key for users to install manually. While the old key is still available, a release can add a new public key and be signed with the old key to allow rotation.

After the workflow finishes, download the installer and `SHA256SUMS.txt` from the draft you created. Review the workflow, commit and installer before approving them. From the checkout, sign the exact files you will publish:

```powershell
.\scripts\release-signing.ps1 -Installer .\dist\AIUsageMonitor-Setup-1.2.3.exe -Checksums .\dist\SHA256SUMS.txt
```

Attach the resulting `SHA256SUMS.txt.sig` to the draft, then publish. Attach all assets before making a release immutable. For older versions and releases missing a valid signature, users can download the installer from the release page.

## Authenticode publisher signature in Windows

Authenticode uses a separate code-signing identity. For a free trusted certificate, open-source maintainers can [apply to SignPath Foundation](https://signpath.org/apply.html). Approval is handled by SignPath. [Eligibility and terms](https://signpath.org/terms)

Once a signing identity is available, sign the application and configure Inno Setup with `AuthenticodeSignTool` to sign the installer and uninstaller during packaging. Verify the signature and timestamp on the finished package. Generate and sign the checksums again after any signing step changes the installer, before publishing. [Inno Setup SignTool](https://jrsoftware.org/ishelp/topic_setup_signtool.htm) · [Microsoft SignTool](https://learn.microsoft.com/en-us/windows/win32/seccrypto/signtool)
