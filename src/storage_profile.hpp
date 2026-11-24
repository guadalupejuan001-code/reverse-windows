#pragma once

#include "bios_hard_disk.hpp"
#include "c07_ata.hpp"
#include "qssd_disk.hpp"
#include <ostream>

namespace reverse_windows {
inline void write_qssd_parallel_io(std::ostream& out,QssdDisk::ParallelIoStats io) {
  out << "{\"read_runs\":" << io.read_runs << ",\"write_transactions\":" << io.write_transactions
      << ",\"read_bytes\":" << io.read_bytes << ",\"write_bytes\":" << io.write_bytes
      << ",\"peak_workers\":" << io.peak_workers << '}';
}

inline void write_storage_profile(std::ostream& out, const cosmic_pc::PcMachine& pc, const BiosHardDisk* bios,
                                  const AtaC07* ata, const QssdDisk* disk) {
  const auto b=bios?bios->transfers():BiosHardDisk::Transfers{};
  const auto a=ata?ata->backend():AtaC07::Backend{};
  const auto io=disk?disk->io_stats():QssdDisk::IoStats{};
  const auto aggregate=disk?disk->xor_summary():QssdDisk::XorSummary{};
  const auto& media=pc.boot_media_reads();
  out << "{\"bios_int13\":{\"reads\":" << b.reads << ",\"writes\":" << b.writes
      << ",\"read_bytes\":" << b.read_bytes << ",\"written_bytes\":" << b.written_bytes
      << ",\"bulk_copies\":" << b.bulk_copies << ",\"guest_ram_ns\":" << b.guest_ram.count()
      << ",\"backend_read_ns\":" << b.backend_read.count()
      << ",\"backend_write_ns\":" << b.backend_write.count()
      << "},\"bios_boot_media\":{\"reads\":" << pc.disk_reads << ",\"read_bytes\":" << media.bytes
      << ",\"bulk_copies\":" << media.bulk_copies << ",\"backend_read_ns\":" << media.backend.count()
      << ",\"guest_ram_ns\":" << media.guest_ram.count()
      << "},\"ata_backend\":{\"reads\":" << a.reads << ",\"writes\":" << a.writes
      << ",\"flushes\":" << a.flushes << ",\"read_ns\":" << a.read_time.count()
      << ",\"write_ns\":" << a.write_time.count() << ",\"flush_ns\":" << a.flush_time.count()
      << ",\"batched_pio_bytes\":" << (ata?ata->counters().batched_pio_bytes:0)
      << "},\"qssd_journal\":{\"pread_calls\":" << io.read_calls
      << ",\"pread_bytes\":" << io.read_bytes << ",\"pread_ns\":" << io.read_ns
      << ",\"pwrite_calls\":" << io.write_calls << ",\"pwrite_bytes\":" << io.write_bytes
      << ",\"pwrite_ns\":" << io.write_ns << ",\"fsync_calls\":" << io.sync_calls
      << ",\"fsync_ns\":" << io.sync_ns
      << ",\"serial_read_run_limit_blocks\":" << QssdDisk::kMaxSerialReadRunBlocks
      << ",\"verified_read_run_limit_blocks\":" << QssdDisk::kMaxVerifiedReadRunBlocks
      << ",\"peak_raw_read_blocks\":" << (disk ? disk->peak_raw_read_blocks() : 0)
      << "},\"qssd_xor64\":{\"revision\":" << aggregate.revision
      << ",\"value\":\"" << cosmic_pc::PcMachine::hex64(aggregate.word) << "\""
      << ",\"bit_parity\":" << aggregate.parity
      << ",\"nonzero_blocks\":" << aggregate.nonzero_blocks << "},\"qssd_parallel_io\":";
  write_qssd_parallel_io(out,disk?disk->parallel_io_stats():QssdDisk::ParallelIoStats{});
  out << '}';
}
}  // namespace reverse_windows
