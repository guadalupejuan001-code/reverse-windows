#include "qssd_disk.hpp"
#include "virtual_disk.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using reverse_windows::QssdDisk;
using reverse_windows::DiskImage;
namespace {
using Clock=std::chrono::steady_clock;
struct Item{std::uint64_t lba;std::array<std::uint8_t,4096> bytes;};
double elapsed(Clock::time_point t){
  return std::chrono::duration<double,std::milli>(Clock::now()-t).count();
}
std::uint64_t physical(const std::string& path){
  struct stat st{};
  if(::stat(path.c_str(),&st)!=0)throw std::runtime_error("stat failed");
  return std::uint64_t(st.st_blocks)*512;
}
std::vector<Item> workload(){
  std::vector<Item> ops;ops.reserve(128);
  std::uint32_t rng=0x8badf00dU;
  for(unsigned i=0;i<128;++i){
    Item op{};
    op.lba=(i%5==0?4096ULL+std::uint64_t(i)*8:512ULL+std::uint64_t(i%16)*8);
    if(i%4==0)op.bytes.fill(0);
    else if(i%4==1)for(unsigned j=0;j<4096;++j)op.bytes[j]=std::uint8_t((j+i)%3);
    else for(auto& b:op.bytes){
      rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;b=std::uint8_t(rng);
    }
    ops.push_back(op);
  }
  return ops;
}
template<class Disk>void verify(Disk& disk,const std::vector<Item>& ops){
  std::array<std::uint8_t,4096> bytes{};
  for(std::size_t i=0;i<ops.size();++i){
    bool superseded=false;
    for(std::size_t j=i+1;j<ops.size();++j)
      if(ops[j].lba==ops[i].lba){superseded=true;break;}
    if(!superseded&&(!disk.read(ops[i].lba,8,bytes.data())||bytes!=ops[i].bytes))
      throw std::runtime_error("benchmark guest bytes mismatch");
  }
}
template<class Disk>double writes(Disk& disk,const std::vector<Item>& ops,int raw_fd=-1){
  auto start=Clock::now();
  for(const auto& op:ops){
    if(!disk.write(op.lba,8,op.bytes.data()))
      throw std::runtime_error("benchmark write failed");
    if(raw_fd>=0&&::fsync(raw_fd)!=0)
      throw std::runtime_error("raw backend fsync failed");
  }
  return elapsed(start);
}
template<class Disk>double reads(Disk& disk,const std::vector<Item>& ops){
  std::array<std::uint8_t,4096> bytes{};
  auto start=Clock::now();
  for(unsigned i=0;i<512;++i)
    if(!disk.read(ops[(i*17)%ops.size()].lba,8,bytes.data()))
      throw std::runtime_error("benchmark read failed");
  return elapsed(start);
}
void benchmark(){
  const auto base=(std::filesystem::temp_directory_path()/
      ("c08-benchmark-"+std::to_string(Clock::now().time_since_epoch().count()))).string();
  const auto raw_file=base+".rvdisk",qssd_file=base+".rqvd";
  try{
    DiskImage::create(raw_file,1);
    QssdDisk::create(qssd_file,1);
    const auto ops=workload();
    double raw_write=0,qssd_write=0,raw_read=0,qssd_read=0;
    double raw_open=0,qssd_full_open=0,qssd_index_open=0;
    double index_build=0,compact_time=0;
    std::uint64_t qssd_journal_before=0,qssd_journal_after=0,index_bytes=0;
    {
      DiskImage raw(raw_file);
      QssdDisk qssd(qssd_file);
      const int fd=::open(raw_file.c_str(),O_RDWR|O_CLOEXEC);
      if(fd<0)throw std::runtime_error("raw fsync descriptor missing");
      try{raw_write=writes(raw,ops,fd);}
      catch(...){::close(fd);throw;}
      ::close(fd);
      qssd_write=writes(qssd,ops);
      verify(raw,ops);verify(qssd,ops);
      raw_read=reads(raw,ops);qssd_read=reads(qssd,ops);
      qssd_journal_before=qssd.stats().physical_bytes;
    }
    {
      const auto start=Clock::now();DiskImage raw(raw_file);
      raw_open=elapsed(start);verify(raw,ops);
    }
    {
      const auto start=Clock::now();QssdDisk disk(qssd_file);
      qssd_full_open=elapsed(start);
      if(disk.recovery_stats().used_checkpoint)
        throw std::runtime_error("unexpected checkpoint in no-index benchmark");
      verify(disk,ops);
      const auto index_start=Clock::now();
      if(!disk.checkpoint())throw std::runtime_error("benchmark checkpoint failed");
      index_build=elapsed(index_start);
      index_bytes=std::filesystem::file_size(qssd_file+".rqix");
    }
    {
      const auto start=Clock::now();QssdDisk disk(qssd_file);
      qssd_index_open=elapsed(start);
      if(!disk.recovery_stats().used_checkpoint)
        throw std::runtime_error("verified checkpoint not loaded");
      verify(disk,ops);
    }
    {
      const auto start=Clock::now();const auto compact=QssdDisk::compact(qssd_file);
      compact_time=elapsed(start);
      if(!compact.replaced||!compact.checkpoint_saved)
        throw std::runtime_error("benchmark compaction/index failed");
      qssd_journal_after=compact.after_bytes;
    }
    {
      QssdDisk disk(qssd_file);
      if(!disk.recovery_stats().used_checkpoint)
        throw std::runtime_error("compacted checkpoint not loaded");
      verify(disk,ops);
    }
    std::cout<<std::fixed<<std::setprecision(3)
      <<"{\"schema\":\"reverse-windows-c08-benchmark-v1\","
      <<"\"host\":\"GitHub-Actions-x86_64-or-native-not-Android\","
      <<"\"workload\":\"128-mixed-4KiB-writes-512-warm-reads\","
      <<"\"raw_fsync_after_every_write\":true,"
      <<"\"qssd_fsync_after_every_commit\":true,"
      <<"\"warm_cache_not_dropped\":true,"
      <<"\"raw_write_ms\":"<<raw_write<<",\"qssd_write_ms\":"<<qssd_write
      <<",\"raw_read_ms\":"<<raw_read<<",\"qssd_read_ms\":"<<qssd_read
      <<",\"raw_open_ms\":"<<raw_open
      <<",\"qssd_full_open_ms\":"<<qssd_full_open
      <<",\"qssd_index_open_ms\":"<<qssd_index_open
      <<",\"checkpoint_build_ms\":"<<index_build
      <<",\"compact_ms\":"<<compact_time
      <<",\"raw_allocated_bytes\":"<<physical(raw_file)
      <<",\"qssd_allocated_bytes\":"<<physical(qssd_file)
      <<",\"qssd_journal_before\":"<<qssd_journal_before
      <<",\"qssd_journal_after\":"<<qssd_journal_after
      <<",\"qssd_sidecar_index_bytes\":"<<index_bytes
      <<",\"windows_booted\":false}\n";
    std::cout<<"REVERSE_WINDOWS_C08_BENCHMARK_PASS performance_not_guaranteed=1\n";
  }catch(...){
    std::error_code ec;std::filesystem::remove(raw_file,ec);
    std::filesystem::remove(qssd_file,ec);
    std::filesystem::remove(qssd_file+".rqix",ec);
    throw;
  }
  std::filesystem::remove(raw_file);
  std::filesystem::remove(qssd_file);
  std::filesystem::remove(qssd_file+".rqix");
}
}
int main(){
  try{benchmark();return 0;}
  catch(const std::exception& e){
    std::cerr<<"REVERSE_WINDOWS_C08_BENCHMARK_FAIL "<<e.what()<<"\n";return 1;
  }
}
