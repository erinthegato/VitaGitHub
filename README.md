# VitaGitHub

A GitHub client for the PS Vita. Browse releases, check GitHub Actions runs and
download files (release assets, source zips, build artifacts) straight to your
Vita. Every request goes over HTTPS (TLS), and the certificate is checked
against a bundled `cacert.pem`.

## Features
- Browse **releases** of any public repo. Shows release notes and downloads assets or the source zip.
- **Sign in** with a personal access token to see your **private repos**.
- **GitHub Actions:** list workflow runs (passed / failed / running) and download their **artifacts**.
- Downloads go to **`uma0:VitaGitHub/<owner_repo>/`**. If uma0 isn't mounted, they fall back to `ux0:data/VitaGitHub/downloads/`.
- Large downloads show a progress bar and speed, and can be cancelled with O.

## Controls
| Button | Action |
|---|---|
| X | Open / download |
| O | Back / cancel download |
| Triangle | Release notes (files screen) / remove saved repo (home) |
| SELECT | Refresh (releases/runs) / sign out (home) |
| L / R | Page up / down |
| START | Exit |

(X and O swap automatically on Japanese consoles.)

## Signing in (private repos + Actions)
1. On GitHub: **Settings → Developer settings → Personal access tokens → Fine-grained tokens → Generate new token**.
2. Choose the repos it can see. Give it these **read-only** permissions:
   - **Contents: Read** (releases, private repo files)
   - **Actions: Read** (workflow runs and artifacts)
   - Metadata: Read (added automatically)
3. Set an expiry date.
4. Put the token in a text file called `token.txt` at `ux0:data/VitaGitHub/token.txt`
   (VitaDrop, FTP or USB all work). You can also type it on the Vita keyboard,
   but it's long.
5. In the app, choose **Sign in with a GitHub token...**. The app also signs in
   automatically on every launch.

> The token is stored as plain text on the memory card. Keep it read-only and
> limited to the repos you need. Sign out (SELECT on the home screen) deletes it.
> It is only ever sent to `api.github.com`.

Signing in also raises GitHub's limit from 60 to 5000 requests per hour.
GitHub only lets signed-in users download Actions artifacts, even from public repos.

## Building
This builds on **GitHub Actions**, so you don't need VitaSDK on your computer:
push to `main` and download `VitaGitHub.vpk` from the run's artifacts.
Push a tag like `v1.0` and the VPK is attached to a GitHub release.

To build locally with VitaSDK installed:
```sh
cmake -S . -B build && cmake --build build
```
`cacert.pem` is downloaded automatically during the first build.

The app is built as **unsafe** homebrew so it can write to `uma0:`.
VitaShell will ask you to confirm that when installing.

## Troubleshooting
- **TLS error:** check that the Vita's date and time are correct. Certificates fail if the clock is wrong.
- **Not found:** the repo may be private. Sign in, and make sure the token can see that repo.
- **Download failed: HTTP 403:** the token is missing *Actions: Read* or *Contents: Read*.
