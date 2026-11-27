# DiskMedic

**English** · [Français](README.fr.md)

A Linux tool written in C to **diagnose and maintain SATA, SAS/SCSI and NVMe disks**. It produces a readable SMART report, verifies the disk by reading it, detects and repairs slow blocks, and erases disks with the method that suits their type: zeros for a hard disk, TRIM or a firmware erase (ATA Secure Erase, NVMe Sanitize or Format NVM) for an SSD. It runs from the command line or from an interactive menu that processes **several disks in parallel**.

- A single C99 source file, with no external library.
- A static binary of about 1 MB that runs on any x86-64 Linux distribution.
- MIT license: free to use, copy, modify and share.

> [!NOTE]
> DiskMedic was largely vibe-coded with an AI assistant. It is covered by automated tests, but it has not been tested on every type of disk: use it with care on disks that hold important data.

> [!WARNING]
> `--zero`, `--slow-repair`, `--trim` and `--secure-erase` **destroy data**. Each one requires typing exactly `YES`, and a disk that is mounted or in use is refused. Still, double-check the target device (`lsblk -o NAME,MODEL,SERIAL,SIZE`): no recovery is possible.

---

## Contents

- [Features](#features)
- [Installation](#installation)
- [Usage](#usage)
- [Interactive interface (TUI)](#interactive-interface-tui)
- [Technical details](#technical-details)
- [Tests and CI](#tests-and-ci)
- [Known limitations](#known-limitations)
- [License](#license)

---

## Features

| Feature | SATA | SAS / SCSI | NVMe |
|---|:-:|:-:|:-:|
| `--verify`: full read | ✅ | ✅ | ✅ |
| `--scan-slow` / `--slow-repair`: slow blocks | ✅ | ✅ | ✅ |
| `--zero`: write zeros + read back | ✅ | ✅ | ✅ |
| `--trim`: TRIM of the whole disk + read back | ✅ | ✅ (UNMAP) | ✅ (Deallocate) |
| `--smart`: health report | ✅ ATA SMART attributes | ✅ LOG SENSE pages + defect list | ✅ SMART/Health log |
| Disk identity card in the TUI menu | ✅ | ✅ | ✅ |
| `--secure-erase` | ✅ ATA Security Erase | ❌ refused | ✅ Sanitize or Format NVM |

All three sector formats are supported: **512n** (512/512), **512e** (512 logical / 4096 physical) and **4Kn** (4096/4096).

### SSDs and NVMe

Writing zeros over an SSD neither heals it nor erases it securely. The flash memory wears with every write, the spare blocks keep copies of the old data, and the controller, which believes the disk is full, slows down until the next TRIM. DiskMedic therefore detects the media type before any write:

| Operation | Hard disk | SSD (SATA, SAS or NVMe) |
|---|---|---|
| `--zero` | Runs | **Refused**, with an explanation. `--zero --force` runs it anyway |
| `--slow-repair` | Runs | **Refused**: an SSD refreshes its data by itself, and slowness there points to the controller or the flash. `--slow-repair --force` runs it anyway |
| `--scan-slow` | Runs | Runs, with a note on how to read the results |
| `--trim` | Rarely supported (clean refusal, nothing is modified) | Empties the disk in seconds, without wear |
| `--secure-erase` | ATA Security Erase | ATA Security Erase (SATA), or an NVMe method chosen from the disk's capabilities |

The media type is read from the firmware: every NVMe device is an SSD, then comes IDENTIFY word 217 for ATA, then VPD page `B1h` for SCSI. The kernel's `rotational` flag is only a last resort, because it can be wrong: an NVMe device backed by a loop file, for example, shows up as rotational.

For an NVMe device, `--secure-erase` chooses the method from the capabilities the controller reports (Identify Controller: `OACS`, `SANICAP`, `FNA`), in this order:

1. **Cryptographic Sanitize**: the disk changes its internal encryption key. It is instant and causes no wear, and it also covers the spare blocks and caches.
2. **Block-erase Sanitize**.
3. **Cryptographic Format NVM** (`SES=2`), if the disk reports it.
4. **Format NVM with user data erase** (`SES=1`), keeping the current sector format.

The method is announced before the `YES` confirmation. Sanitize acts on the whole controller: with several active namespaces, DiskMedic does not use it and falls back to Format NVM, which only erases the target namespace, when the disk allows it. Otherwise it refuses. Sanitize overwrite is never used, because it wears the flash. DiskMedic checks the capabilities itself instead of relying on the disk to refuse: QEMU's NVMe emulation, for example, accepts a cryptographic erase that it does not report.

## Installation

### Option 1: prebuilt binary (no dependencies)

Download the archive and `SHA256SUMS` from the [Releases](../../releases/latest) page. The binary is statically linked and does not depend on any library on the machine.

```sh
sha256sum -c SHA256SUMS --ignore-missing   # optional: integrity check
tar -xzf diskmedic-1.15.0-linux-x86_64.tar.gz
cd diskmedic-1.15.0-linux-x86_64
sudo ./diskmedic --help
```

The only requirements are x86-64 Linux, kernel 3.2 or later, and root privileges.

### Option 2: build from source

**Build dependencies**: a C99 compiler (gcc), `make` and the Linux kernel headers. The code uses `<linux/fs.h>`, `<linux/nvme_ioctl.h>` and `<scsi/sg.h>`. A static binary also needs the static libc.

| Distribution | Packages |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential linux-libc-dev` |
| Fedora / RHEL | `sudo dnf install gcc make kernel-headers glibc-static` |
| Arch Linux | `sudo pacman -S base-devel linux-api-headers` |
| openSUSE | `sudo zypper install gcc make linux-glibc-devel glibc-devel-static` |

```sh
make                 # builds ./diskmedic (-O2 -Wall -Wextra -Werror -std=c99)
make test            # tests that do not need root

# Static binary, portable across distributions:
cc -O2 -Wall -Wextra -Werror -std=c99 -static diskmedic.c -o diskmedic && strip diskmedic
```

The code has been built and tested with gcc 13 and glibc on Ubuntu 24.04. Other compilers or libcs (clang, musl) should work, but have not been tested.

**Running**: nothing is needed besides the kernel. You must be root, or have `CAP_SYS_RAWIO` plus access to the device, to send raw ATA, SCSI or NVMe commands and open disks exclusively.

## Usage

```sh
sudo ./diskmedic /dev/sdX --smart                          # identity + SMART report
sudo ./diskmedic /dev/sdX --verify --log verify.csv        # full read
sudo ./diskmedic /dev/sdX --scan-slow --slow-factor 4      # look for slow blocks
sudo ./diskmedic /dev/sdX --slow-repair --log repair.csv   # scan + targeted rewrite  ⚠
sudo ./diskmedic /dev/sdX --zero                           # zeros (hard disk)        ⚠
sudo ./diskmedic /dev/nvme0n1 --trim                       # full TRIM (SSD)          ⚠
sudo ./diskmedic /dev/nvme0n1 --secure-erase               # firmware erase           ⚠
sudo ./diskmedic                                           # multi-disk menu
sudo ./diskmedic --tui /dev/sdX                            # menu for a single disk
```

Only one operation is accepted per run.

| Option | Purpose |
|---|---|
| `--smart` (alias `--smart-only`) | Disk identity and readable SMART report |
| `--verify` | Full read, without writing |
| `--scan-slow` (alias `--slow-scan`) | Looks for abnormally slow blocks (read-only) |
| `--slow-factor N` | Slowness threshold = median × N (default: 4, minimum: 1) |
| `--log FILE` | CSV log of bad or slow sectors (`--verify`, `--scan-slow`, `--slow-repair`, `--zero`) |
| `--slow-repair` | Scan, then rewrite the confirmed slow zones only (destructive) |
| `--zero` | Writes zeros over the whole disk, then reads back to verify (destructive; refused on SSD) |
| `--trim` | TRIM of the whole disk in 1 GiB chunks, then reads back samples (destructive) |
| `--secure-erase` | ATA Security Erase (*enhanced* mode if offered), or NVMe Sanitize / Format NVM (destructive) |
| `--force` | With `--zero` or `--slow-repair`: runs the operation even on an SSD |
| `--tui` | Interactive menu; without a device, multi-disk selection |
| `--no-color` | No colors (the `NO_COLOR` environment variable does the same) |
| `--help`, `-h` / `--version` | Help / version |

**Exit codes**

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Error, refused operation (SSD, Secure Erase not possible, TRIM not supported), unreadable sectors found, or at least one failed disk (multi-disk mode) |
| `2` | Invalid options, missing `YES` confirmation, or TUI mode without a terminal |
| `3` / `4` / `5` | `--smart` only: the report was read, and its verdict is WARNING / CRITICAL / UNKNOWN (`0` = HEALTHY) |

**CSV log**: it has the columns `timestamp_iso8601,device,operation,LBA,status,action`. LBAs are expressed in logical sectors.

- Every unreadable LBA is logged on its own line as soon as it is found, on pass 1 or pass 2 of a scan.
- A block that fails on pass 1 but reads fine on pass 2 gets a `TRANSIENT READ ERROR` line; the scan then ends with exit code `1`.
- After `--slow-repair`, each lost LBA gets a second line with its final outcome (`REWRITE VERIFIED…` or `FAILED…`).
- A slow zone is logged with its LBA range (`SLOW: zone LBA 21120-21247 (64 KiB) …`).

## Interactive interface (TUI)

`sudo ./diskmedic` opens the disk selection:

| Key | Action |
|---|---|
| ↑ / ↓ (or `k` / `j`), Page Up / Down, Home / End | Move |
| Space | Select or deselect a disk |
| `a` | Select or deselect all |
| Enter | Confirm, then choose the operation: ↑/↓ + Enter, or 1 to 7 directly (7 = TRIM); `q` goes back to the selection |
| `q` | Quit |

Each line shows the disk's model, capacity and type (HDD, SSD, NVMe). A disk marked **IN USE** is mounted or held (swap, LVM, RAID…), so only read-only operations will run on it. **SYSTEM** marks the disk that holds the root filesystem.

While you choose the operation, the selected disks stay on screen with their serial number, capacity and type. They are listed again before a destructive operation.

The selected disks are **processed in parallel**, one process per disk, and followed on a full-screen dashboard:

```
╔════════════════════════════════════════════════════════════════════════════════════════╗
║DiskMedic 1.15.0 · Slow scan · 3 disk(s) in parallel                            00:00:26║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║> /dev/sde · scsi_debug · 209.7 MB                                    RUNNING · 00:00:26║
║  Step: Pass 1/2: timing                                                  unreadable: 10║
║  ███████████████████████████████░░░░░░░░░░░░░░░░░░ 64.0 % · 68.9 MiB/s · left 00:00:14 ║
║  › Unreadable sector: LBA 4669 (No data available)                                     ║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║  /dev/sdf · scsi_debug · 209.7 MB                                    RUNNING · 00:00:26║
║  Step: Pass 1/2: timing                                                  unreadable: 10║
║  ███████████████████████████████░░░░░░░░░░░░░░░░░░ 64.0 % · 69.0 MiB/s · left 00:00:14 ║
║  › Unreadable sector: LBA 4669 (No data available)                                     ║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║  /dev/sdg · scsi_debug · 209.7 MB                                    RUNNING · 00:00:26║
║  Step: Pass 1/2: timing                                                  unreadable: 10║
║  ███████████████████████████████░░░░░░░░░░░░░░░░░░ 64.0 % · 68.9 MiB/s · left 00:00:14 ║
║  › Unreadable sector: LBA 4669 (No data available)                                     ║
╠════════════════════════════════════════════════════════════════════════════════════════╣
║Running: 3 · Finished: 0 (OK: 0, failed: 0)                       all done in ≈ 00:00:14║
║Logs: /root/diskmedic-20261127-031832                                                   ║
║↑/↓ choose a disk · Enter view its log · q interrupt all operations                     ║
╚════════════════════════════════════════════════════════════════════════════════════════╝
```

For each disk, the dashboard shows:

- the state and elapsed time;
- the current step and the number of unreadable sectors found;
- the rate and time left;
- the last message of the operation;
- an "updated N s ago" alert when the disk stops making progress (retries on a bad sector).

In a small window, the display switches to two lines per disk.

- **Following one disk**: ↑/↓ chooses a disk, Enter shows its log full screen and keeps it up to date while the operations go on; `q`, Esc or Enter goes back to the dashboard. The bottom line also gives the time left for the slowest disk.
- **Interrupting**: `q` or Ctrl+C, then `y` to confirm. The summary shows the step and percentage each disk had reached. A Secure Erase cannot be interrupted, because the disk's firmware runs it.
- **Logs**: a `diskmedic-YYYYMMDD-HHMMSS/` directory is created in the current directory (or in `/tmp`). It holds one `sdX.log` per disk, plus an `sdX.csv` for verification and scans. Under `sudo`, the files are given to the calling user.
- **End**: the normal terminal first shows an `[OK]` / `[FAILED]` / `[INTERRUPTED]` summary, then each disk's report, then a one-line recap. For SMART, a disk whose verdict is not healthy shows as `[WARNING]`, `[CRITICAL]` or `[UNKNOWN]`, and the dashboard counts it as a SMART alert.
- **Safety**: before a destructive operation, the list of target disks is shown in red, with the model, serial number, capacity and planned operation for each one. If a single one of them is mounted or in use, nothing is started.
- **Terminal**: the screen is redrawn when the window is resized. If the program is stopped (Ctrl+C in the single-disk menu, `kill`), the terminal is restored. Without a UTF-8 locale, frames and bars are drawn in plain ASCII.
- **SSDs in the selection**: for a zero-fill, DiskMedic lists the SSDs and offers `t` (TRIM on the SSDs, zeros on the hard disks), `z` (zeros everywhere) or cancellation. For a slow-block repair, it offers `s` (scan only on the SSDs), `r` (repair everywhere) or cancellation.

With `--tui /dev/sdX`, the menu covers a single disk. Its header shows the disk's identity card, whatever its type:

- SATA: model, serial, firmware;
- NVMe: health, temperature, remaining life;
- SAS: firmware assessment, temperature.

It also shows the capacity, the sector format and the media type (SSD or hard disk). The operations are listed one per line with a short description: move with ↑/↓ and press Enter, or press 1 to 7 directly. An operation that the disk cannot run (Secure Erase on SAS or behind a USB bridge) is greyed out with the reason. The SMART report opens in a scrollable viewer (↑/↓, PgUp/PgDn, `q` to close). Before a destructive operation, the disk's model, serial number and capacity are shown again. On an SSD, choices 4 and 5 offer the suitable alternative (scan only, TRIM) or the operation anyway. Verification, slow scan and repair write their CSV log to a new `diskmedic-YYYYMMDD-HHMMSS/` directory, as in multi-disk mode.

## Technical details

### Hardware access

| Item | Implementation |
|---|---|
| ATA commands (SATA, including behind a USB/SAT bridge) | `SG_IO` + ATA PASS-THROUGH(16), opcode `85h`. Return registers read from sense descriptor `09h` or from fixed sense (ASC/ASCQ `00/1Dh`) |
| ATA SMART | IDENTIFY DEVICE (`ECh`), SMART READ DATA (`D0h`), READ THRESHOLDS (`D1h`), RETURN STATUS (`DAh`) |
| NVMe | `NVME_IOCTL_ADMIN_CMD`: Identify Controller / Namespace / active namespace list (CNS `01h`, `00h`, `02h`), Get Log Page `02h` (SMART/Health) and `81h` (Sanitize status), Sanitize (`84h`), Format NVM (`80h`) |
| SAS / SCSI | INQUIRY + VPD pages `80h`/`B1h`, LOG SENSE (`2Fh`, `0Dh`, `03h`, `02h`, `05h`, `06h`, `0Eh`, `15h`, `11h`), READ DEFECT DATA(10) for the grown defect list (G list) |
| Type detection | NVMe (`NVME_IOCTL_ID`), then ATA IDENTIFY, then SCSI INQUIRY |
| SSD / hard disk detection | NVMe, IDENTIFY word 217 (rotation rate), VPD `B1h`, then `/sys/dev/block/M:m/queue/rotational` |
| TRIM | `BLKDISCARD` in 1 GiB chunks (the kernel turns it into DATA SET MANAGEMENT, UNMAP or Deallocate) |
| Geometry | `BLKGETSIZE64`, `BLKSSZGET` (logical sector), `BLKPBSZGET` (physical sector) |

### Measurement reliability and safety

- **Exclusive claim**: before any write, the disk is opened with `O_EXCL`. The kernel refuses this if the disk, or one of its partitions, is mounted, used as swap, or part of a RAID or LVM setup.
- **`O_DIRECT`**: reads and writes bypass the page cache, with aligned buffers and `posix_fadvise(DONTNEED)`. The program falls back to buffered I/O if the device does not support `O_DIRECT`.
- **Physical-sector re-reads**: an unreadable block is first re-read physical sector by physical sector, then logical sector by logical sector for the failed physical sectors only. On a 512e disk, a fully lost physical sector thus costs 4 failed reads instead of 9, and each failure can take the disk several seconds of retries.
- **Secure Erase**: the temporary ATA password is `diskmedic`, and it is shown before the operation. After an interruption, unlock the disk with:
  `hdparm --user-master u --security-unlock diskmedic /dev/sdX`, then `--security-disable diskmedic`.
  A disk locked by a version older than the project's rename (zero-fix) has the password `zero-fix`.
- **NVMe Sanitize**: it is started with `AUSE=1`, which allows leaving the failure mode if the erase fails. Its progress is read from log `81h`. The disk carries on by itself, even after a power loss or if DiskMedic stops.

### Slow-block scan algorithm

1. **Pass 1, timing**: each 1 MiB block is timed. The threshold is the *local* median × `--slow-factor`, with a minimum gap of 5 ms above that median. The local median is computed over each 1 GiB region, because a hard disk reads its outer tracks (start of the disk) about twice as fast as its inner tracks (end of the disk); it is kept between 0.5 and 1.5 × the global median (computed over a sample of at most 1 million measurements), so that a damaged area covering most of its region cannot raise its own threshold. Without this floor, on very fast media (NVMe, median below 1 ms), ordinary system pauses would be enough to exceed the threshold. A re-read on a hard disk costs at least one platter revolution (8 to 11 ms), so it is still detected.
2. **Pass 2, confirmation**: only the blocks that were slow or unreadable on pass 1 are read again, each one after reading 64 MiB elsewhere to flush the disk's cache. This takes seconds instead of a second full read of the disk. When there are so many candidates that these flushes would cost more than a full read, the whole disk is read again instead. A block is kept only if it exceeds the threshold on both passes. A block that is slow on one pass only is classified as *intermittent* and is never rewritten. A read error is located sector by sector as soon as it occurs, on pass 1 or 2; a block that fails on pass 1 and reads fine on pass 2 is reported as a *transient read error*.
3. **Location**: each confirmed block is read again in 64 KiB zones (or zones of the physical sector size, if larger), from the last zone to the first. Beforehand, 64 MiB are read elsewhere on the disk to flush its cache. Each zone is compared to a healthy reference zone: a zone is slow if it carries at least half of the block's delay, or if it exceeds the factor × the reference with a gap of at least 1 ms.
4. **Targeted rewrite** (`--slow-repair`): only the slow zones are rewritten, with their own data, then read back and compared. In an unreadable block, only the physical sectors that contain a lost sector are rewritten; the lost part is replaced with zeros, which lets the firmware reallocate the sector.
5. **Deferred check**: each rewritten zone is timed again after another cache flush, then classified as `FAST` or `STILL SLOW`.

### Zero-fill (`--zero`)

1. **Writing**: zeros are written in 1 MiB blocks with `O_DIRECT`. If a block cannot be written, it is written again physical sector by physical sector, then logical sector by logical sector inside a physical sector that still fails. Each unwritable LBA is reported and logged (`zero`, `DEFECTIVE: write failed`), and the erase goes on: the rest of the disk is erased anyway.
2. **Read-back**: after a sync, the whole disk is read back. An unreadable block is located sector by sector (`zero-verify`, `DEFECTIVE: read failed`), and each sector that does not read back as zeros is logged (`DEFECTIVE: not zero after write`).
3. **Result**: the operation fails (exit code `1`) if at least one sector could not be written, read back or zeroed, with a summary of the three counts.

### Parallel execution

- **Isolation**: each disk is handled by a child process (`fork`), with its own sector sizes, CSV log and `O_DIRECT` descriptor. No state is shared between disks.
- **Progress**: each process publishes its step, bytes processed, rates and unreadable sectors in an `mmap(MAP_SHARED)` area. This area is protected by a sequence lock, which the dashboard reads without blocking, four times per second.
- **Output**: each process's standard output and standard error are redirected to its `.log` file.
- **Keyboard**: it is read without signals, so Ctrl+C becomes a key and an interruption is always confirmed. `SIGTERM` and `SIGHUP` stop the child processes cleanly.

### Resources used

- **Memory**:
  - verification and zero-fill: a few MiB;
  - slow scan: about 4 MiB of RAM per TB of disk, plus 8 MiB for the measurement sample.
- **I/O**: 1 MiB blocks. In multi-disk mode, disks on the same controller, USB hub or enclosure share its bandwidth.

### Repository layout

```
diskmedic.c                 Single source file (CLI, ATA/NVMe/SAS SMART, disk I/O, scan, TUI)
Makefile                   make / make test / make test-loop / make clean
tests/run_tests.sh         Tests without root (options, exit codes, SMART rendering)
tests/smart_fixture_test.c ATA/NVMe/SAS SMART reports generated from fabricated data
tests/run_loop_tests.sh    Root tests on virtual disks (loop + device-mapper)
tests/run_sector_tests.sh  Root tests for 512n/512e/4Kn, SSD, TRIM and multi-disk (scsi_debug)
tests/run_nvme_tests.sh    Root NVMe tests (nvme-loop + QEMU virtual machine)
.github/workflows/ci.yml   GitHub Actions CI
dist/                      Release archive and SHA256SUMS (not in git, published in Releases)
LICENSE                    MIT license
```

## Tests and CI

The tests **never use a real disk**, only files, loop devices, in-memory virtual SCSI disks and emulated NVMe controllers.

| Command | Privileges | Content |
|---|---|---|
| `make test` | user | Options, exit codes, SMART rendering on fabricated data (SATA HDD/SSD, NVMe, SAS), SAT decoding, no colors outside a terminal, width ≤ 80 columns |
| `sudo sh tests/run_loop_tests.sh` | root | Verification, scan, repair, zero-fill and refusal of a mounted disk, on a loop device. Unreadable sectors simulated with `dm-error` and `dm-flakey` (including a zero-fill that goes on past unwritable sectors), slow sectors with `dm-delay`, and a transient read error (the error table is swapped for a healthy one between the two passes). Checks the CSV log line by line |
| `sudo sh tests/run_sector_tests.sh` | root | The same operations in 512n, 512e and 4Kn with `scsi_debug`, which presents itself as an SSD: `--zero` and `--slow-repair` refused without `--force`, TRIM refused without UNMAP and then successful with it. Also checks the exact LBA reported, that a single physical sector is rewritten, the number of failed reads, and the keyboard-driven multi-disk menu in a pseudo-terminal (including the TRIM choice for SSDs), the CSV log of the single-disk menu, and a disk that predicts a failure (test mode enabled with `sdparm`): exit code `4` and `[CRITICAL]` in the summary |
| `sudo sh tests/run_nvme_tests.sh` | root | **nvme-loop** (the kernel's NVMe target, without Format or Sanitize): SMART, `--zero` refused, Secure Erase refused before confirmation, TRIM. **QEMU** (a virtual machine that boots in a few seconds, with three emulated NVMe controllers): Format NVM actually run on 512 B and 4 KiB sectors (format kept), and on a controller with two namespaces (the other namespace stays intact) |

`make test-loop` runs the three root scripts. They skip themselves (`SKIP`) if privileges, kernel modules or tools are missing.

The choice of the NVMe method (cryptographic or block Sanitize, Format, several namespaces) is also tested by `make test`, on fabricated Identify data.

**Tools needed for the root tests**:

- `util-linux`: `losetup`, `blockdev`, `script`;
- `dmsetup` (package `dmsetup` or `device-mapper`);
- `e2fsprogs`: `mkfs.ext4`;
- `coreutils`: `dd`, `md5sum`, `timeout`;
- `udevadm`;
- `sdparm` (optional: SMART CRITICAL test);
- the kernel modules `dm-delay`, `dm-flakey` and `scsi_debug`.

On Ubuntu, `scsi_debug` comes with `linux-modules-extra-$(uname -r)`.

**Additional tools for the NVMe tests**:

- the kernel modules `nvmet` and `nvme-loop` (nvme-loop part);
- `qemu-system-x86`, `busybox-static`, `cpio`, `gzip` and the static libc (QEMU part);
- a guest kernel with the NVMe driver built in: `DISKMEDIC_TEST_KERNEL`, otherwise the WSL kernel (`C:\Program Files\WSL\tools\kernel`), otherwise `/boot/vmlinuz-$(uname -r)`. KVM is used when available; otherwise software emulation is slower but works.

On Ubuntu: `sudo apt install qemu-system-x86 busybox-static cpio`.

**GitHub Actions CI** (`ubuntu-latest`) builds with warnings treated as errors, then runs all four suites on every push and pull request.

## Known limitations

- **Secure Erase**: ATA Security Erase and, for NVMe, Sanitize and Format NVM. ATA Sanitize (SATA) and FORMAT UNIT / SANITIZE (SAS) are not implemented: the option is refused on SAS disks, without modifying anything.
- **NVMe Sanitize**: neither nvme-loop nor QEMU 8.2 emulates Sanitize, so the automated tests only cover the choice of method, on fabricated data. The command itself follows the NVMe specification. Format NVM is tested in QEMU.
- **SSD detection**: it relies on what the firmware reports. A disk that does not report its rotation rate, for example behind a USB bridge, is judged from the kernel's flag, which can be wrong. The rare NVMe hard disks would be treated as SSDs.
- **`--zero`** writes up to the size seen by the kernel. Hidden areas (HPA/DCO) and blocks reserved by the firmware are not reached; Secure Erase should cover them.
- **Reallocated sectors**: they cannot be "repaired". A rewrite can only trigger the reallocation of a *pending* sector.
- **USB bridges**: some enclosures pass through neither ATA commands nor SMART pages. The report then says `UNKNOWN` rather than `HEALTHY`.
- **Disk types**: DiskMedic has not been tested on every type of disk (models, interfaces, USB bridges, controllers). Feedback is welcome, ideally with the output of `smartctl -x`.
- **Platform**: Linux only (`SG_IO`, `BLK*` and NVMe ioctls).

## License

Released under the **MIT license**: see [LICENSE](LICENSE).

You may freely use, copy, modify, merge, publish, distribute, sublicense and sell this software, including for commercial purposes. The only condition is to keep the copyright notice and the license text in all copies or substantial portions of the code.

The software is provided **"as is", without any warranty**. The authors are not liable for any data loss or hardware damage that may result from its use. This applies especially to a tool that can erase disks.
