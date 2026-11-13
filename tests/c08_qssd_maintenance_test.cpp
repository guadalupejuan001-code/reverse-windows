#include "qssd_disk.hpp"
#include "c07_ata.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <unistd.h>

using reverse_windows::QssdDisk;
namespace {
std::uint64_t checks=0;
void need(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class Fn>void rejects(Fn&& fn,const char* why){
  bool refused=false;try{fn();}catch(const std::exception&){refused=true;}
  need(refused,why);
}
std::array<std::uint8_t,4096> bytes(unsigned seed){
  std::array<std::uint8_t,4096> data{};
  std::uint32_t s=0x9e3779b9U+17U*seed;
  for(auto& v:data){s^=s<<13;s^=s>>17;s^=s<<5;v=std::uint8_t(s);}
  return data;
}
void write_xor(const std::string& path,std::uint64_t at){
  const int fd=::open(path.c_str(),O_RDWR|O_CLOEXEC);
  need(fd>=0,"cannot open corruption fixture");
  std::uint8_t b{};
  need(::pread(fd,&b,1,at)==1,"cannot read corruption byte");
  b^=0x40U;
  need(::pwrite(fd,&b,1,at)==1,"cannot tamper corruption byte");
  ::close(fd);
}
void verify(const std::string& path){
  constexpr std::uint64_t first=512,distant=230000,cleared=800;
  QssdDisk::create(path,1);
  const auto far=bytes(37);
  std::array<std::uint8_t,4096> recent{},readback{},zero{};
  {
    QssdDisk disk(path);
    for(unsigned n=0;n<48;++n){
      recent=bytes(100+n);
      need(disk.write(first,8,recent.data()),"overwriting QSSD block failed");
    }
    need(disk.write(distant,8,far.data()),"distant block commit failed");
    need(disk.write(cleared,8,recent.data()),"zero fixture write failed");
    need(disk.write(cleared,8,zero.data()),"zero fixture clear failed");
    need(disk.stats().commits==51,"base commit count wrong");
    need(disk.checkpoint(),"atomic index checkpoint failed");
  }
  {
    QssdDisk disk(path);
    const auto r=disk.recovery_stats();
    need(r.used_checkpoint&&r.replayed_records==0&&r.checkpoint_blocks==3,
         "checkpoint did not restore three live radix block mappings");
    need(disk.read(first,8,readback.data())&&readback==recent,"checkpoint first block mismatch");
    need(disk.read(distant,8,readback.data())&&readback==far,"checkpoint distant block mismatch");
    need(disk.read(cleared,8,readback.data())&&readback==zero,"checkpoint zero block mismatch");
    for(unsigned n=0;n<12;++n){
      recent=bytes(500+n);
      need(disk.write(first,8,recent.data()),"post-index transaction failed");
    }
  }
  {
    QssdDisk disk(path);
    const auto r=disk.recovery_stats();
    need(r.used_checkpoint&&r.replayed_records==24,
         "checkpoint tail replay must validate 12 records plus 12 COMMITs");
    need(disk.stats().commits==63,"checkpoint did not preserve cumulative commit count");
    need(disk.read(first,8,readback.data())&&readback==recent,
         "tail replay did not recover last committed guest bytes");
    need(disk.checkpoint(),"second atomic checkpoint write failed");
  }
  const auto before=std::filesystem::file_size(path);
  const auto index_bytes=std::filesystem::file_size(path+".rqix");
  need(before>100000&&index_bytes>=112+48,"checkpoint fixture too small");

  // Corrupt the INDEX only: use full original journal and recover exact data.
  write_xor(path+".rqix",96);
  {
    QssdDisk disk(path);
    need(!disk.recovery_stats().used_checkpoint&&
         disk.recovery_stats().replayed_records>=126,
         "invalid checkpoint was trusted instead of validated full replay");
    need(disk.read(first,8,readback.data())&&readback==recent,
         "sidecar corruption modified committed data");
    need(disk.checkpoint(),"failed to regenerate damaged disposable index");
  }
  // Even a historical record replaced 40 times must be verified before
  // trusting the checkpoint. A byte flip makes constructor fail closed.
  const auto damaged=path+".damaged.rqvd";
  std::filesystem::copy_file(path,damaged);
  std::filesystem::copy_file(path+".rqix",damaged+".rqix");
  write_xor(damaged,64+40+64);
  rejects([&]{QssdDisk broken(damaged);},
          "checkpoint concealed corruption in overwritten committed record");
  std::filesystem::remove(damaged);
  std::filesystem::remove(damaged+".rqix");

  rejects([&]{(void)QssdDisk::compact(path,true);},
          "pre-rename compact fault injection was not triggered");
  need(std::filesystem::file_size(path)==before,
       "crash before rename changed original valid journal");
  {
    QssdDisk disk(path);
    need(disk.read(first,8,readback.data())&&readback==recent,
         "interrupted compact damaged original guest data");
  }
  const auto info=QssdDisk::compact(path);
  need(info.replaced&&info.checkpoint_saved&&info.generation==1,
       "verified compact not atomically published and indexed");
  need(info.before_bytes==before&&info.after_bytes<before/4,
       "compactor did not reclaim historical/overwritten records");
  need(info.live_blocks==2,"compactor should omit zero-only leaf");
  {
    QssdDisk disk(path);
    need(disk.generation()==1&&disk.recovery_stats().used_checkpoint,
         "new-generation compacted volume index did not open");
    need(disk.read(first,8,readback.data())&&readback==recent,
         "compaction changed latest guest data");
    need(disk.read(distant,8,readback.data())&&readback==far,
         "compaction changed distant guest data");
    need(disk.read(cleared,8,readback.data())&&readback==zero,
         "compaction resurrected old data after a zero overwrite");
    // The SAME BlockDisk must remain usable behind the existing C07 ATA.
    reverse_windows::AtaC07 ata(disk,[]{});
    need(ata.output(0x1f6,1,0xe0)&&ata.output(0x1f2,1,1)&&
         ata.output(0x1f3,1,std::uint8_t(first))&&
         ata.output(0x1f4,1,std::uint8_t(first>>8))&&
         ata.output(0x1f5,1,std::uint8_t(first>>16))&&
         ata.output(0x1f7,1,0x20),
         "C07 ATA read failed after compaction");
    for(unsigned word=0;word<256;++word)
      need(ata.input(0x1f0,2).has_value(),"ATA PIO after compact failed");
    need(ata.counters().read_sectors==1,"ATA did not read from compacted disk");
  }
  // Interrupted guest append: truncate incomplete bytes, keep validated image.
  {
    const int fd=::open(path.c_str(),O_WRONLY|O_APPEND|O_CLOEXEC);
    need(fd>=0,"cannot append crash tail");
    constexpr char partial[]="QBLKpart";
    need(::write(fd,partial,sizeof(partial)-1)==ssize_t(sizeof(partial)-1),
         "cannot create interrupted transaction fixture");
    ::close(fd);
    QssdDisk disk(path);
    need(disk.recovery_stats().used_checkpoint&&
         std::filesystem::file_size(path)==info.after_bytes,
         "committed checkpoint + crash tail not safely recovered");
    need(disk.read(first,8,readback.data())&&readback==recent,
         "crash recovery lost last guest sector");
  }
  std::cout<<"C08_ATOMIC_COMPACTION_PASS old_bytes="<<before
           <<" new_bytes="<<info.after_bytes
           <<" sparse_live_blocks="<<info.live_blocks<<"\n";
  std::cout<<"C08_CHECKPOINT_CRASH_REPLAY_PASS index_bytes="<<index_bytes
           <<" replay_tail=24 prefix_verified=true\n";
}
}
int main(){
  const auto root=std::filesystem::temp_directory_path()/
      ("qssd-c08-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto path=root.string()+".rqvd";
  try{
    verify(path);
    std::filesystem::remove(path);std::filesystem::remove(path+".rqix");
    std::cout<<"REVERSE_WINDOWS_C08_MAINTENANCE_PASS checks="<<checks
             <<" windows_booted=false\n";
    return 0;
  }catch(const std::exception& e){
    std::cerr<<"REVERSE_WINDOWS_C08_MAINTENANCE_FAIL "<<e.what()<<"\n";
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    std::filesystem::remove(path+".rqix",ignored);
    return 1;
  }
}
