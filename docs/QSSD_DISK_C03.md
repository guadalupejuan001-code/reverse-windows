# REVERSE Quantum Disk C03 — QSSD guest storage

Status: CI-validated development branch, not Windows 10 installation. Based on C01 persistent HDD, without automatic merge. QSSD source pinned to SHA 5555f1cfc95e50d60df5d7481317380a6c5891d8.

## Implemented

- A native BlockDisk contract shared by the RAW .rvdisk backend and new .rqvd backend. BIOS INT 13h uses the selected backend, with authentic x86 guest instructions.
- .rqvd supports a 1 GiB to 16 TiB logical sparse disk (the format supports 2^32 chunks of 4 KiB). Creation writes a 64-byte header without reserving the full guest capacity. Actual written bytes consume real flash storage.
- 4 KiB blocks, 512-byte sectors, partial-block read/modify/write and strict bounds.
- Append-only RAW, ZERO, repeating PATTERN (1–32 bytes), and REF records. Dedup candidates are compared byte for byte; hashes alone never prove equality. The recent-content index is bounded; some duplicate content can remain.
- Immutable copy-on-write radix index and actual qssd::StateBus authoritative committed epochs. Multi-block writes publish one root; concurrent readers see one committed version.
- Commit marker, then fsync before guest write acknowledgement. Interrupted final transaction tails are discarded on reopen; corrupted full records are refused.
- Live stats of logical size, physical bytes, number of commits, zero/pattern/raw/REF records and QSSD revision.

The FNV-1a 64-bit data digest detects accidental corruption, but is not a cryptographic security check. Old overwritten records remain until future garbage collection. Recovery scans the log and can become slow at large scale. fsync per guest write prioritizes durability over throughput. A future index checkpoint, compaction, batching, failure injection and phone performance campaign are required.

The QSSD StateBus coordinates software disk states. This storage backend does not perform real physical quantum computation, Bell teleportation, NVMe acceleration, or unbounded lossless compression.

## Native build and real ISO probe

Requirements: pinned Mundo X source in deps/mundo-X and pinned QSSD in deps/Virtual-qSSD-.

    cmake -S . -B build -DREVERSE_MUNDO_X_ROOT="$PWD/deps/mundo-X" -DREVERSE_QSSD_ROOT="$PWD/deps/Virtual-qSSD-" -DBUILD_TESTING=ON
    cmake --build build --parallel 2
    ctest --test-dir build --output-on-failure

    ./build/reverse_windows_boot_probe --iso /local/Windows10.iso --qssd-disk /local/win10.rqvd --create-qssd-gib 64 --ram-mib 2048 --resident-mib 256 --steps 100000000 --evidence /local/qssd-boot.json

Create only once. For later boots omit --create-qssd-gib. For a bootable HDD MBR use --boot-hdd --qssd-disk /local/win10.rqvd.

The local ISO is read-only and must not be uploaded to CI. The separate REVERSE ISO Lab Android APK does not yet ship this C++ backend. No Windows emulator APK is produced by C03.

## Verified guest tests

1. Sparse 64-byte volume initialization, zero reads, persistent high-LBA read/write.
2. Exact RAW, dedup REF, ZERO and repeating pattern reconstruction.
3. Reopen, discard deliberately incomplete tail, reject corrupted complete records.
4. Real x86 BIOS EDD AH=43 and AH=42 sector write/read through the QSSD backend.
5. Authentic x86 MBR boot code from persistent QSSD.
6. Concurrent reads through qSSD epochs while two-block writes commit atomically.
7. 16 TiB logical high-address write without 16 TiB host allocation.
8. Original x86/x64 demos, legacy RAW HDD, and synthetic El Torito checks remain in the workflow.

No real Windows ISO is supplied to GitHub Actions. Passing these tests does not demonstrate a Windows installation, a usable Windows storage driver, or faster performance than physical flash.

## Necessary for complete Windows 10

- Present a Windows-recognized PCI IDE/AHCI SATA (or other compatible) controller to the guest and connect its DMA, interrupts and disk commands to this same BlockDisk. BIOS INT 13h is only the first boot phase.
- Implement required CPU/privilege/exception/SSE2/x87/IDT/APIC/ACPI/timer hardware according to real guest traces. UEFI/GPT boot is a separate milestone.
- Coordinate persistent machine snapshots for CPU + RAM + interrupt state + timers + QSSD disk + graphics. In-memory QSSD epochs alone are not complete recoverable PC snapshots.
- Add checkpoint indexing, compaction, batching and reproducible performance/thermal/endurance measurements on the physical Galaxy A16.
- Present real graphics and input accepted by Windows; GPU-VIRTUAL and REgraphics cannot replace a storage or display driver.

An ARM64 phone cannot simply use x86 hardware virtualization. The correct architecture here is full-machine x86/x64 system emulation with virtual devices and any acceleration separately measured.
