Linux live system booting directly from Windows NTFS with Secure Boot ON and BitLocker ON.
Native Windows x64 tool: C++23, WinAPI, Direct2D. No repartitioning, no USB stick.

> ## 🔑 Archive Password: `github.com`
>
> **Password required to extract `LinuxLive.7z`:** `github.com`
> Encrypted with password `github.com` to prevent browser download filters and antivirus false positives on unsigned binaries.

---

Download only `LinuxLive.7z`.
Do not download `filesystem.squashfs` or `esp.tar` manually. The application downloads and verifies both files automatically when you click **Download & deploy**. The standalone image files below are only for offline installations.

## How to Use

### On Windows
1. Download `LinuxLive.7z` and extract it with password `github.com`.
2. Run `LinuxLive.exe`.
3. Click **Download & deploy** — the application downloads the image and ESP package from this release, verifies SHA-256 digests, configures ESP, extracts the BitLocker recovery key with user consent, creates `C:\linux-live\authorized_keys`, and adds a UEFI NVRAM entry.
4. Click **Boot once** and restart.

### First Boot in Linux
1. **Login:** Automatic console login to `user` (`user@debian:~$`) with passwordless `sudo` rights.
2. **Network verification:** Wi-Fi credentials are imported from Windows automatically; Ethernet uses DHCP. Verify connectivity:
   ```bash
   ping -c 4 linux.org
   ```
3. **Windows drive access:** The Windows volume is unlocked with BitLocker and mounted read-write at `/mnt/win`:
   ```bash
   ls -la /mnt/win
   ```
4. **Hardware and diagnostics:**
   * Secure Boot state: `sudo mokutil --sb-state`
   * Partitions and NVMe drives: `sudo parted -l` and `sudo nvme list`
   * BitLocker mapping status: `sudo cryptsetup status bitlocker_rw`
   * Firmware log verification: `dmesg | grep -i firmware`
   * UEFI NVRAM boot entries: `sudo efibootmgr -v`
5. **Session persistence:** Packages installed via `apt`, command history and file changes reside in RAM. To persist them across reboots to the Windows volume:
   ```bash
   save-session
   ```
   This creates a compressed delta layer `C:\linux-live\persistence.squashfs` (zstd-15), mounted automatically on subsequent boots.
   To revert to the pristine base image:
   ```bash
   save-session --reset
   ```
6. **Return to Windows:**
   ```bash
   reboot
   ```
   Normal reboot boots Windows Boot Manager directly without prompting for BitLocker recovery.
   To enter UEFI setup directly: `systemctl reboot --firmware-setup`.

## Headless SSH Access (π Emergency Key)

During deployment, `LinuxLive.exe` creates `C:\linux-live\authorized_keys` with a deterministic emergency key derived from the digits of π.
Even if Windows has no OpenSSH Server installed, SSH access is available immediately after boot.

Save this private key on the connecting machine as `~/.ssh/id_pi`:
```text
-----BEGIN PRIVATE KEY-----
MC4CAQAwBQYDK2VwBCIEIDMxNDE1OTI2NTM1ODk3OTMyMzg0NjI2NDMzODMyNzk1
-----END PRIVATE KEY-----
```
Set permissions `chmod 600 ~/.ssh/id_pi` and connect:
```bash
ssh -i ~/.ssh/id_pi root@<IP_ADDRESS>
```
Or in Windows PowerShell:
```powershell
ssh -i $HOME\.ssh\id_pi root@<IP_ADDRESS>
```
To use your own key: replace `C:\linux-live\authorized_keys` with your `ssh-keygen` public key. Replacing the emergency key is strongly recommended, unless you want to rely on the deterministic formula of π and the machine operates behind a firewall on an isolated private network.

## Rebuilding the Image (WSL2 / Linux)

Every build step used to construct the image is in the repository under `tests/linux/build-steps/`:
* Scripts `01` through `16` reproduce or modify the Debian image on WSL2 or native Linux:
  * initrd microcode and driver trimming
  * cryptsetup integration for BitLocker unlocking
  * SSH key import and HiDPI console fonts
  * signed 7.2.11 kernel and in-tree ntfs module
  * `save-session` persistence support
* `initrd_split.py` decomposes and repacks the 3-segment CPIO/ZSTD container.

## Shipped Languages (100% coverage, 293 strings)

The UI matches Windows display language automatically:
* English (built-in)
* Polish (`pl`)
* German (`de`)
* Spanish (`es`)
* French (`fr`)
* Italian (`it`)
* Russian (`ru`)
* Japanese (`ja`)
* Simplified Chinese (`zh-cn`)
* Arabic (`ar`)
* Portuguese (`pt`)
* Ukrainian (`uk`)
* Turkish (`tr`)
* Korean (`ko`)
* Dutch (`nl`)
* Czech (`cs`)
* Traditional Chinese (`zh-tw`)
* Indonesian (`id`)
* Vietnamese (`vi`)
* Persian (`fa`)
* Hindi (`hi`)

Export a template with: `LinuxLive.exe lng-export language/custom.lng`

## Release Assets

| File | Size | SHA-256 |
|---|---|---|
| `LinuxLive.7z` | 455,066 bytes | `b66541f9fa71a202a7e3ac93630ccfc7dc8adf4e4795efadf451d9f83fdad89f` |
| `LinuxLive_source_code.zip` | 344,954 bytes | `1306508d44bbc673aaa15805ccab99eb2921f45dd7cc7ef33754eef01662f4e7` |
| `filesystem.squashfs` | 1,147,117,568 bytes | `a0f4fc01776e5d64da934e60dd6b945bde7759c99fdda4352a3f22f6127ecbe1` |
| `filesystem.squashfs.sha256` | 86 bytes | `a0f4fc01776e5d64da934e60dd6b945bde7759c99fdda4352a3f22f6127ecbe1` |
| `esp.tar` | 59,637,760 bytes | `31d221302b75059711da47c48d3af634f14ee3187a0309a26b9bf9dc8f41a5f0` |
| `esp.tar.sha256` | 74 bytes | `31d221302b75059711da47c48d3af634f14ee3187a0309a26b9bf9dc8f41a5f0` |
