#pragma once
// C03: persistent QSSD-backed block device for a real x86 guest.
// Uses the pinned qssd::StateBus for immutable committed disk epochs.
// Records contain literal guest bytes (or exact zero/pattern/reference encodings).
// No simulated quantum hardware or arbitrary compression promises.
#include "block_disk.hpp"
#include "qssd/state_bus.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <sys/file.h>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace reverse_windows {
class QssdDisk final : public BlockDisk {
 public:
  enum class Access { ReadWrite, ReadOnly };
  static constexpr std::uint64_t kSectorSize = 512, kBlockSize = 4096;
  static constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  static constexpr std::uint64_t kMaxLogicalBytes = 8ULL << 50;
  static constexpr std::size_t kMaxTransactionBlocks = 128;
  static constexpr std::size_t kMaxTransactionBytes = kMaxTransactionBlocks * kBlockSize;
  static constexpr std::size_t kMaxBulkTransactionBlocks = 2048;
  static constexpr std::size_t kMaxBulkTransactionBytes = kMaxBulkTransactionBlocks * kBlockSize;
  struct Stats {
    std::uint64_t logical_bytes, physical_bytes, commits, raw_blocks;
    std::uint64_t zero_blocks, pattern_blocks, deduplicated_blocks, qssd_serial;
  };
  struct RecoveryStats {
    bool used_checkpoint=false;
    std::uint64_t replayed_records=0, checkpoint_blocks=0;
    std::uint64_t verified_prefix_bytes=0;
  };
  struct CompactResult {
    std::uint64_t before_bytes=0, after_bytes=0, live_blocks=0;
    std::uint64_t old_commits=0, compact_commits=0, generation=0;
    bool replaced=false, checkpoint_saved=false;
  };
  struct CacheStats {
    std::uint64_t hits=0, misses=0, resident_blocks=0, resident_bytes=0;
  };
  struct IoStats {
    std::uint64_t read_calls,read_bytes,write_calls,write_bytes,sync_calls;
    std::uint64_t read_ns,write_ns,sync_ns;
  };
  struct XorSummary {
    std::uint64_t revision=0,word=0,nonzero_blocks=0;
    unsigned parity=0;
  };


 private:
  // Immutable 16-way radix nodes allow COW block-index updates in O(log16 blocks).
  // Old qSSD epoch views retain the exact old block map without copying a disk.
  enum Kind : std::uint32_t { Raw=1, Zero=2, Pattern=3, Reference=4, Commit=5 };
  struct Ref {
    std::uint64_t offset=0,word_xor=0;
    Kind kind=Zero;
  };
  struct Node {
    std::array<std::shared_ptr<const Node>, 16> children{};
    Ref ref{};
    std::uint64_t word_xor=0,nonzero_blocks=0;
  };
  using NodePtr=std::shared_ptr<const Node>;
  struct Epoch {
    NodePtr root;
    std::uint64_t commits=0, raw=0, zeros=0, patterns=0, refs=0, end=kHeader;
  };
  struct Record {
    Kind kind;
    std::uint64_t block, digest, reference, offset, next;
    std::vector<std::uint8_t> payload;
  };

  static constexpr std::size_t kHeader=64, kRecHeader=40, kRecFooter=16;
  static constexpr std::size_t kRawRecordBytes=kRecHeader+kBlockSize+kRecFooter;
  static constexpr std::size_t kIndexHeader=112, kIndexEntry=16;
  static constexpr std::uint64_t kFNVStart=14695981039346656037ULL;
  static constexpr std::uint64_t kFNVPrime=1099511628211ULL;
  static constexpr std::size_t kDigestLanes=4;

  static constexpr std::size_t kDedupWindow=4096;
  static constexpr std::size_t kReadCacheBlocks=128; // 512 KiB bounded data
  int fd_=-1;
  const Access access_;
  std::filesystem::path path_;
  std::uint64_t capacity_=0, committed_end_=kHeader, generation_=0;
  unsigned index_levels_=8;
  RecoveryStats recovery_{};
  std::mutex writer_;
  // Readers can observe immutable qSSD epochs while the journal appends.
  // Maintenance is offline, so no live-fd replacement is performed.

  qssd::StateBus<Epoch> states_{Epoch{}, 32};
  std::deque<std::pair<std::uint64_t,std::uint64_t>> dedup_fifo_;
  std::unordered_map<std::uint64_t,std::vector<std::uint64_t>> dedup_;
  using BlockData=std::array<std::uint8_t,kBlockSize>;
  mutable std::mutex cache_mutex_;
  mutable std::unordered_map<std::uint64_t,std::shared_ptr<const BlockData>> read_cache_;
  mutable std::deque<std::uint64_t> read_fifo_;
  mutable std::atomic<std::uint64_t> cache_hits_{0},cache_misses_{0};
  struct IoCounters {
    std::atomic<std::uint64_t> calls{0},bytes{0},ns{0};
    void add(std::size_t length,std::chrono::steady_clock::time_point started) noexcept {
      calls.fetch_add(1,std::memory_order_relaxed);
      bytes.fetch_add(length,std::memory_order_relaxed);
      ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-started).count(),std::memory_order_relaxed);
    }
  };
  mutable IoCounters physical_reads_,physical_writes_,physical_syncs_;


  static std::uint32_t u32(const std::uint8_t* p) {
    std::uint32_t v=0;
    for(unsigned i=0;i<4;++i) v|=std::uint32_t(p[i])<<(i*8);
    return v;
  }
  static std::uint64_t u64(const std::uint8_t* p) {
    std::uint64_t v=0;
    for(unsigned i=0;i<8;++i) v|=std::uint64_t(p[i])<<(i*8);
    return v;
  }
  static void put32(std::uint8_t* p, std::uint32_t v) {
    for(unsigned i=0;i<4;++i)p[i]=std::uint8_t(v>>(i*8));
  }
  static void put64(std::uint8_t* p, std::uint64_t v) {
    for(unsigned i=0;i<8;++i)p[i]=std::uint8_t(v>>(i*8));
  }
  static std::uint64_t digest_update(std::uint64_t h,const std::uint8_t* p,std::size_t n) {
    for(std::size_t i=0;i<n;++i){h^=p[i];h*=kFNVPrime;}
    return h;
  }
  static std::uint64_t digest(const std::uint8_t* p,std::size_t n) {
    return digest_update(kFNVStart,p,n);
  }
  static std::array<std::uint64_t,kDigestLanes> digest_four(
      const std::array<const std::uint8_t*,kDigestLanes>& data,std::size_t size) noexcept {
    // Four independent streams hide the multiplication latency while keeping
    // the identical on-disk FNV-1a digest of each record.
    auto a=kFNVStart,b=kFNVStart,c=kFNVStart,d=kFNVStart;
    for(std::size_t i=0;i<size;++i){
      a=(a^data[0][i])*kFNVPrime;
      b=(b^data[1][i])*kFNVPrime;
      c=(c^data[2][i])*kFNVPrime;
      d=(d^data[3][i])*kFNVPrime;
    }
    return {a,b,c,d};
  }
  static std::uint64_t fd_digest(int fd,std::uint64_t end,IoCounters* measurement=nullptr) {
    std::array<std::uint8_t,65536> page{};
    auto h=kFNVStart;
    for(std::uint64_t pos=0;pos<end;) {
      const auto chunk=std::size_t(std::min<std::uint64_t>(end-pos,page.size()));
      read_all(fd,pos,page.data(),chunk,measurement);
      h=digest_update(h,page.data(),chunk);
      pos+=chunk;
    }
    return h;
  }
  static void fsync_parent(const std::filesystem::path& path) {
    auto parent=path.parent_path();
    if(parent.empty())parent=".";
    int dir=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir<0)throw std::system_error(errno,std::generic_category(),"QSSD open parent for sync");
    const bool ok=::fsync(dir)==0;
    const int error=errno;
    ::close(dir);
    if(!ok)throw std::system_error(error,std::generic_category(),"QSSD directory sync");
  }
  static std::filesystem::path temp_name(const std::filesystem::path& path,const char* suffix) {
    static std::atomic<unsigned long long> counter{0};
    return path.string()+suffix+"."+std::to_string(::getpid())+"."+
           std::to_string(counter.fetch_add(1,std::memory_order_relaxed));
  }

  static void check_path(const std::filesystem::path& p) {
    if(p.empty()||p.extension()!=".rqvd")
      throw std::invalid_argument("QSSD disk must be an explicit .rqvd regular file");
  }
  static void read_all(int fd,std::uint64_t off,void* ptr,std::size_t size,
                       IoCounters* measurement=nullptr) {
    auto* out=static_cast<std::uint8_t*>(ptr);
    for(std::size_t done=0;done<size;) {
      const auto started=measurement?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
      const auto n=::pread(fd,out+done,size-done,static_cast<off_t>(off+done));
      const auto error=errno;
      if(measurement)measurement->add(n>0?std::size_t(n):0,started);
      if(n<0&&error==EINTR)continue;
      if(n<=0)throw std::runtime_error("QSSD truncated or unreadable block record");
      done+=static_cast<std::size_t>(n);
    }
  }
  static void write_all(int fd,std::uint64_t off,const void* ptr,std::size_t size,
                        IoCounters* measurement=nullptr) {
    const auto* in=static_cast<const std::uint8_t*>(ptr);
    for(std::size_t done=0;done<size;) {
      const auto started=measurement?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
      const auto n=::pwrite(fd,in+done,size-done,static_cast<off_t>(off+done));
      const auto error=errno;
      if(measurement)measurement->add(n>0?std::size_t(n):0,started);
      if(n<0&&error==EINTR)continue;
      if(n<=0)throw std::system_error(n<0?error:EIO,std::generic_category(),"QSSD write");
      done+=static_cast<std::size_t>(n);
    }
  }
  int sync_volume() noexcept {
    const auto started=std::chrono::steady_clock::now();
    const auto result=::fsync(fd_);
    const auto error=errno;
    physical_syncs_.add(0,started);
    errno=error;
    return result;
  }
  std::uint64_t file_size() const {
    struct stat info{};
    if(::fstat(fd_,&info)!=0||info.st_size<0)throw std::runtime_error("QSSD stat failed");
    return static_cast<std::uint64_t>(info.st_size);
  }
  Ref locate(const NodePtr& root,std::uint64_t block) const {
    // The epoch retains the entire immutable tree for the duration of a lookup.
    const auto* node=root.get();
    for(unsigned level=index_levels_;level>0;--level){
      if(!node)return {};
      node=node->children[(block>>((level-1)*4))&15].get();
    }
    return node?node->ref:Ref{};
  }
  template<class Visitor>
  static void each_live(NodePtr node,unsigned level,std::uint64_t prefix,Visitor& visit) {
    if(!node)return;
    if(level==0) {
      if(node->ref.offset)visit(prefix,node->ref.offset);
      return;
    }
    const auto shift=(level-1U)*4U;
    for(unsigned index=0;index<16;++index) {
      if(node->children[index])
        each_live(node->children[index],level-1,
                  prefix|(std::uint64_t(index)<<shift),visit);
    }
  }
  static NodePtr replace_contiguous(const NodePtr& node,std::uint64_t block,unsigned level,
                                    const Ref* refs,std::size_t count) {
    auto next=std::make_shared<Node>(node?*node:Node{});
    if(!level){
      next->ref=*refs;
      next->word_xor=refs->word_xor;
      next->nonzero_blocks=refs->kind!=Zero;
      return next;
    }
    const auto shift=(level-1)*4;
    const auto stride=std::uint64_t{1}<<shift;
    while(count){
      const auto slot=(block>>shift)&15U;
      const auto run=std::min<std::uint64_t>(count,stride-(block&(stride-1)));
      auto& child=next->children[slot];
      if(child){
        next->word_xor^=child->word_xor;
        next->nonzero_blocks-=child->nonzero_blocks;
      }
      child=replace_contiguous(child,block,level-1,refs,std::size_t(run));
      next->word_xor^=child->word_xor;
      next->nonzero_blocks+=child->nonzero_blocks;
      block+=run;refs+=run;count-=std::size_t(run);
    }
    return next;
  }
  static NodePtr replace(const NodePtr& node,std::uint64_t block,unsigned level,Ref ref) {
    return replace_contiguous(node,block,level,&ref,1);
  }
  static std::uint64_t xor_words(const std::uint8_t* data) noexcept {
    std::uint64_t aggregate=0;
    for(std::size_t offset=0;offset<kBlockSize;offset+=8){
      std::uint64_t word;
      std::memcpy(&word,data+offset,sizeof(word));
      aggregate^=word;
    }
    return u64(reinterpret_cast<const std::uint8_t*>(&aggregate));
  }
  static std::uint64_t xor_rule(const BlockData& data,std::size_t period) noexcept {
    if(!period)return xor_words(data.data());
    // Each XOR lane repeats after period bytes even with a non-aligned motif.
    // Pairs of complete repetitions cancel. Only odd repetitions and the tail contribute.
    std::uint8_t lanes[8]{};
    constexpr auto words=kBlockSize/8;
    const auto count=((words/period)&1U?period:0)+words%period;
    for(std::size_t i=0;i<count;++i)
      for(unsigned byte=0;byte<8;++byte)lanes[byte]^=data[(i*8+byte)%period];
    return u64(lanes);
  }

  static std::size_t record_payload_size(const std::uint8_t* header) {
    if(std::memcmp(header,"QBLK",4)!=0)throw std::runtime_error("QSSD record magic corruption");
    const auto kind=u32(header+4), len=u32(header+16);
    if(kind<Raw||kind>Commit||len>kBlockSize)
      throw std::runtime_error("QSSD unknown record type or oversize payload");
    if((kind==Raw&&len!=kBlockSize)||(kind==Zero&&len!=0)||
       (kind==Pattern&&(len<1||len>32))||
       ((kind==Reference||kind==Commit)&&len!=0))
      throw std::runtime_error("QSSD invalid record length");
    return len;
  }
  static void verify_record_footer(const std::uint8_t* record,std::size_t len) {
    const auto* footer=record+kRecHeader+len;
    if(std::memcmp(footer,"QEND",4)!=0||u32(footer+4)!=kRecHeader+len+kRecFooter||
       u64(footer+8)!=digest(record,kRecHeader+len))
      throw std::runtime_error("QSSD journal checksum corruption");
  }
  // A missing record means incomplete tail. Malformed complete records throw.
  static std::optional<Record> parse_record_at(const std::uint8_t* bytes,
                                               std::uint64_t offset,std::size_t available) {
    if(available<kRecHeader)return std::nullopt;
    const auto len=record_payload_size(bytes);
    const auto total=kRecHeader+len+kRecFooter;
    if(available<total)return std::nullopt;
    verify_record_footer(bytes,len);
    return Record{Kind(u32(bytes+4)),u64(bytes+8),u64(bytes+24),u64(bytes+32),offset,offset+total,
                  {bytes+kRecHeader,bytes+kRecHeader+len}};
  }
  std::optional<Record> record_at(std::uint64_t offset,std::uint64_t limit) const {
    if(offset>limit||limit-offset<kRecHeader)return {};
    std::array<std::uint8_t,kRecHeader> h{};
    read_all(fd_,offset,h.data(),h.size(),&physical_reads_);
    const auto total=kRecHeader+record_payload_size(h.data())+kRecFooter;
    if(total>limit-offset)return {};
    std::array<std::uint8_t,kRawRecordBytes> all;
    std::copy(h.begin(),h.end(),all.data());
    read_all(fd_,offset+kRecHeader,all.data()+kRecHeader,total-kRecHeader,&physical_reads_);
    return parse_record_at(all.data(),offset,total);
  }
  std::array<std::uint8_t,kBlockSize> decode(std::uint64_t offset,std::uint64_t limit) const {
    const auto record=record_at(offset,limit);
    if(!record)throw std::runtime_error("QSSD missing stored data block");
    return decode(*record,limit);
  }
  static void expand_payload(BlockData& data,const std::uint8_t* payload,std::size_t size) {
    std::memcpy(data.data(),payload,size);
    for(auto filled=size;filled<data.size();){
      const auto count=std::min(filled,data.size()-filled);
      std::memcpy(data.data()+filled,data.data(),count);
      filled+=count;
    }
  }
  std::array<std::uint8_t,kBlockSize> decode(const Record& record,std::uint64_t limit) const {
    if(record.kind==Commit)throw std::runtime_error("QSSD missing stored data block");
    std::array<std::uint8_t,kBlockSize> data{};
    if(record.kind==Raw||record.kind==Pattern) {
      expand_payload(data,record.payload.data(),record.payload.size());
    } else if(record.kind==Reference) {
      if(record.reference<kHeader||record.reference>=record.offset)
        throw std::runtime_error("QSSD forward/dangling content reference");
      auto raw=record_at(record.reference,limit);
      if(!raw||raw->kind!=Raw)throw std::runtime_error("QSSD REF must point directly to RAW");
      std::copy(raw->payload.begin(),raw->payload.end(),data.begin());
    }
    if(digest(data.data(),data.size())!=record.digest)
      throw std::runtime_error("QSSD decoded block digest differs");
    return data;
  }
  static std::size_t append_record(std::vector<std::uint8_t>& bytes,Kind kind,std::uint64_t block,
                                   std::uint64_t hash,std::uint64_t reference,
                                   const std::uint8_t* payload,std::size_t size) {
    const auto position=bytes.size();
    const auto length=kRecHeader+size+kRecFooter;
    bytes.resize(position+length);
    auto* record=bytes.data()+position;
    std::memcpy(record,"QBLK",4);
    put32(record+4,kind);
    put64(record+8,block);
    put32(record+16,static_cast<std::uint32_t>(size));
    put64(record+24,hash);
    put64(record+32,reference);
    if(size)std::memcpy(record+kRecHeader,payload,size);
    auto* end=record+kRecHeader+size;
    std::memcpy(end,"QEND",4);
    put32(end+4,static_cast<std::uint32_t>(length));
    return position;
  }
  static void encode_record(std::vector<std::uint8_t>& bytes,Kind kind,std::uint64_t block,
                            std::uint64_t hash,std::uint64_t reference,
                            const std::uint8_t* payload,std::size_t size) {
    const auto position=append_record(bytes,kind,block,hash,reference,payload,size);
    const auto length=kRecHeader+size;
    auto* record=bytes.data()+position;
    put64(record+length+8,digest(record,length));
  }
  static void finish_raw_records(std::vector<std::uint8_t>& journal,
                                 const std::vector<std::size_t>& positions) noexcept {
    constexpr auto digested=kRecHeader+kBlockSize;
    std::size_t i=0;
    for(;positions.size()-i>=kDigestLanes;i+=kDigestLanes){
      std::array<const std::uint8_t*,kDigestLanes> records{};
      for(std::size_t lane=0;lane<kDigestLanes;++lane)
        records[lane]=journal.data()+positions[i+lane];
      const auto sums=digest_four(records,digested);
      for(std::size_t lane=0;lane<kDigestLanes;++lane)
        put64(journal.data()+positions[i+lane]+digested+8,sums[lane]);
    }
    for(;i<positions.size();++i){
      auto* record=journal.data()+positions[i];
      put64(record+digested+8,digest(record,digested));
    }
  }
  static std::uint64_t append_to(int fd,std::uint64_t at,Kind kind,std::uint64_t block,
                                 std::uint64_t hash,std::uint64_t reference,
                                 const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes;
    encode_record(bytes,kind,block,hash,reference,payload.data(),payload.size());
    write_all(fd,at,bytes.data(),bytes.size());
    return at+bytes.size();
  }
  static std::size_t rule_period(const BlockData& data) noexcept {
    const auto* start=data.data();
    for(unsigned period=1;period<=32;++period)
      if(std::memcmp(start,start+period,data.size()-period)==0)return period;
    return 0;
  }
  void cache_insert_locked(std::uint64_t off,std::shared_ptr<const BlockData> data) const {
    if(read_cache_.find(off)!=read_cache_.end())return;
    if(read_fifo_.size()>=kReadCacheBlocks) {
      const auto old=read_fifo_.front();read_fifo_.pop_front();
      read_cache_.erase(old);
    }
    read_fifo_.push_back(off);
    try{read_cache_.emplace(off,std::move(data));}
    catch(...){read_fifo_.pop_back();throw;}
  }
  void cache_insert(std::uint64_t off,std::shared_ptr<const BlockData> data) const {
    std::lock_guard<std::mutex> guard(cache_mutex_);
    cache_insert_locked(off,std::move(data));
  }
  bool read_cached_raw_run(std::uint64_t offset,std::size_t count,std::uint8_t* target) const {
    std::lock_guard<std::mutex> guard(cache_mutex_);
    std::array<const BlockData*,kMaxTransactionBlocks> pages;
    for(std::size_t i=0;i<count;++i){
      const auto found=read_cache_.find(offset+i*kRawRecordBytes);
      if(found==read_cache_.end())return false;
      pages[i]=found->second.get();
    }
    for(std::size_t i=0;i<count;++i)
      std::memcpy(target+i*kBlockSize,pages[i]->data(),kBlockSize);
    cache_hits_.fetch_add(count,std::memory_order_relaxed);
    return true;
  }
  static void verify_raw_batch(const std::array<const std::uint8_t*,kDigestLanes>& records,
                               std::size_t count) {
    constexpr auto digested=kRecHeader+kBlockSize;
    std::array<const std::uint8_t*,kDigestLanes> payloads{};
    for(std::size_t lane=0;lane<count;++lane){
      const auto* record=records[lane];
      const auto* tail=record+digested;
      if(std::memcmp(record,"QBLK",4)!=0||u32(record+4)!=Raw||u32(record+16)!=kBlockSize||
         std::memcmp(tail,"QEND",4)!=0||u32(tail+4)!=kRawRecordBytes)
        throw std::runtime_error("QSSD invalid RAW record");
      payloads[lane]=record+kRecHeader;
    }
    std::array<std::uint64_t,kDigestLanes> encoded{},data{};
    if(count==kDigestLanes){
      encoded=digest_four(records,digested);
      data=digest_four(payloads,kBlockSize);
    }else{
      for(std::size_t lane=0;lane<count;++lane){
        encoded[lane]=digest(records[lane],digested);
        data[lane]=digest(payloads[lane],kBlockSize);
      }
    }
    for(std::size_t lane=0;lane<count;++lane)
      if(encoded[lane]!=u64(records[lane]+digested+8)||data[lane]!=u64(records[lane]+24))
        throw std::runtime_error("QSSD RAW record checksum corruption");
  }
  void read_raw_run(std::uint64_t offset,std::uint64_t block,std::size_t count,
                    std::uint64_t limit,std::vector<std::uint8_t>& buffer,
                    std::uint8_t* target) const {
    const auto size=count*kRawRecordBytes;
    if(offset>limit||size>limit-offset)
      throw std::runtime_error("QSSD truncated contiguous RAW records");
    if(read_cached_raw_run(offset,count,target))return;
    buffer.resize(size);
    read_all(fd_,offset,buffer.data(),size,&physical_reads_);

    for(std::size_t i=0;i<count;){
      std::array<const std::uint8_t*,kDigestLanes> records{};
      const auto batch=std::min(kDigestLanes,count-i);
      for(std::size_t lane=0;lane<batch;++lane){
        const auto* record=buffer.data()+(i+lane)*kRawRecordBytes;
        if(u64(record+8)!=block+i+lane)
          throw std::runtime_error("QSSD RAW record block address differs");
        records[lane]=record;
      }
      verify_raw_batch(records,batch);
      i+=batch;
    }
    // Copy only after the raw run has passed both checksum checks.
    for(std::size_t i=0;i<count;++i)
      std::memcpy(target+i*kBlockSize,buffer.data()+i*kRawRecordBytes+kRecHeader,kBlockSize);
    cache_misses_.fetch_add(count,std::memory_order_relaxed);
  }

  std::shared_ptr<const BlockData> cached_decode(std::uint64_t off,
                                                  std::uint64_t limit) const {
    {
      std::lock_guard<std::mutex> guard(cache_mutex_);
      const auto found=read_cache_.find(off);
      if(found!=read_cache_.end()){
        cache_hits_.fetch_add(1,std::memory_order_relaxed);
        return found->second;
      }
    }
    // The digest of the record is checked on first decode and refs only
    // name immutable append-only offsets. A published offset is never reused.
    auto decoded=std::make_shared<const BlockData>(decode(off,limit));
    cache_misses_.fetch_add(1,std::memory_order_relaxed);
    cache_insert(off,decoded);
    return decoded;
  }

  void remember(std::uint64_t hash,std::uint64_t off) {
    dedup_[hash].push_back(off);
    dedup_fifo_.emplace_back(hash,off);
    if(dedup_fifo_.size()>kDedupWindow) {
      const auto old=dedup_fifo_.front();dedup_fifo_.pop_front();
      auto it=dedup_.find(old.first);
      if(it!=dedup_.end()){
        auto& v=it->second;
        v.erase(std::remove(v.begin(),v.end(),old.second),v.end());
        if(v.empty())dedup_.erase(it);
      }
    }
  }
  // A disposable sidecar index. It is never trusted without matching the
  // generation, full committed journal prefix hash, index checksum, and every
  // referenced live block. Corrupt sidecar -> full journal replay.
  std::optional<Epoch> read_checkpoint(std::uint64_t volume_size) {
    const auto path=path_.string()+".rqix";
    const int index=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(index<0)return {};
    std::optional<Epoch> loaded;
    try {
      struct stat stat{};
      if(::fstat(index,&stat)!=0||!S_ISREG(stat.st_mode)||
         stat.st_size<static_cast<off_t>(kIndexHeader))
        throw std::runtime_error("QSSD index unavailable/incomplete");
      std::array<std::uint8_t,kIndexHeader> head{};
      read_all(index,0,head.data(),head.size());
      const auto cutoff=u64(head.data()+32);
      const auto count=u64(head.data()+80);
      if(std::memcmp(head.data(),"RQIX0001",8)!=0||
         u64(head.data()+8)!=1||u64(head.data()+16)!=capacity_||
         u64(head.data()+24)!=generation_||
         cutoff<kHeader||cutoff>volume_size||
         count>capacity_/kBlockSize||
         count>(std::uint64_t(stat.st_size)-kIndexHeader)/kIndexEntry||
         std::uint64_t(stat.st_size)!=kIndexHeader+count*kIndexEntry)
        throw std::runtime_error("QSSD checkpoint does not match the volume");
      if(cutoff>kHeader) {
        if(cutoff<kHeader+kRecHeader+kRecFooter)
          throw std::runtime_error("QSSD checkpoint has no transaction");
        const auto end_record=record_at(cutoff-kRecHeader-kRecFooter,cutoff);
        if(!end_record||end_record->kind!=Commit||
           end_record->next!=cutoff||end_record->block!=u64(head.data()+40))
          throw std::runtime_error("QSSD checkpoint is not anchored at a valid commit");
      }else if(u64(head.data()+40)!=0||count!=0) {
        throw std::runtime_error("QSSD empty checkpoint contains committed state");
      }
      // The streaming digest scans ALL bytes prior to cutoff. Unlike checking
      // only active refs, this rejects overwritten-but-corrupt journal history.
      if(fd_digest(fd_,cutoff,&physical_reads_)!=u64(head.data()+88))
        throw std::runtime_error("QSSD checkpoint prefix journal digest mismatch");
      recovery_.verified_prefix_bytes=cutoff;
      const auto expected_index_hash=u64(head.data()+96);
      put64(head.data()+96,0);
      auto checksum=digest(head.data(),head.size());
      Epoch state{};
      state.end=cutoff;
      state.commits=u64(head.data()+40);
      state.raw=u64(head.data()+48);
      state.zeros=u64(head.data()+56);
      state.patterns=u64(head.data()+64);
      state.refs=u64(head.data()+72);
      std::uint64_t previous=0;
      for(std::uint64_t i=0;i<count;++i) {
        std::array<std::uint8_t,kIndexEntry> entry{};
        read_all(index,kIndexHeader+i*kIndexEntry,entry.data(),entry.size());
        checksum=digest_update(checksum,entry.data(),entry.size());
        const auto block=u64(entry.data()),off=u64(entry.data()+8);
        if(block>=capacity_/kBlockSize||(i&&block<=previous)||
           off<kHeader||off>=cutoff)
          throw std::runtime_error("QSSD checkpoint references invalid guest block");
        const auto rec=record_at(off,cutoff);
        if(!rec||rec->kind==Commit||rec->block!=block)
          throw std::runtime_error("QSSD checkpoint references non-block record");
        const auto data=decode(*rec,cutoff);
        const auto word=rec->kind==Zero?0:xor_rule(data,rec->kind==Pattern?rec->payload.size():0);
        state.root=replace(state.root,block,index_levels_,Ref{off,word,rec->kind});
        if(rec->kind==Raw)remember(rec->digest,off);
        previous=block;
      }
      if(checksum!=expected_index_hash)
        throw std::runtime_error("QSSD checkpoint index checksum mismatch");
      recovery_.checkpoint_blocks=count;
      recovery_.used_checkpoint=true;
      loaded=std::move(state);
    }catch(const std::exception&){
      dedup_.clear();dedup_fifo_.clear();
      recovery_.used_checkpoint=false;
      recovery_.checkpoint_blocks=0;
      recovery_.verified_prefix_bytes=0;
    }
    ::close(index);
    return loaded;
  }
  bool save_checkpoint_locked() {
    // Caller owns writer_ and has a fully fsynced journal transaction.
    const auto view=states_.snapshot();
    const auto& state=*view->value;
    const auto target=std::filesystem::path(path_.string()+".rqix");
    const auto temp=temp_name(target,".tmp");
    int out=::open(temp.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(out<0)return false;
    bool published=false;
    try {
      std::array<std::uint8_t,kIndexHeader> header{};
      std::memcpy(header.data(),"RQIX0001",8);
      put64(header.data()+8,1);
      put64(header.data()+16,capacity_);
      put64(header.data()+24,generation_);
      put64(header.data()+32,state.end);
      put64(header.data()+40,state.commits);
      put64(header.data()+48,state.raw);
      put64(header.data()+56,state.zeros);
      put64(header.data()+64,state.patterns);
      put64(header.data()+72,state.refs);
      put64(header.data()+88,fd_digest(fd_,state.end,&physical_reads_));
      write_all(out,0,header.data(),header.size());
      std::uint64_t count=0;
      auto append_entry=[&](std::uint64_t block,std::uint64_t offset){
        if(count>capacity_/kBlockSize)
          throw std::runtime_error("QSSD checkpoint block count invalid");
        std::array<std::uint8_t,kIndexEntry> entry{};
        put64(entry.data(),block);
        put64(entry.data()+8,offset);
        write_all(out,kIndexHeader+count*kIndexEntry,entry.data(),entry.size());
        ++count;
      };
      each_live(state.root,index_levels_,0,append_entry);
      put64(header.data()+80,count);
      // checksum over header with index_hash=0 and all ordered entries
      write_all(out,0,header.data(),header.size());
      const auto hash=fd_digest(out,kIndexHeader+count*kIndexEntry);
      put64(header.data()+96,hash);
      write_all(out,0,header.data(),header.size());
      if(::fsync(out)!=0)
        throw std::system_error(errno,std::generic_category(),"QSSD index fsync");
      ::close(out);out=-1;
      if(::rename(temp.c_str(),target.c_str())!=0)
        throw std::system_error(errno,std::generic_category(),"QSSD index atomic publish");
      published=true;
      fsync_parent(target);
      return true;
    }catch(const std::exception&){
      if(out>=0)::close(out);
      if(!published)(void)::unlink(temp.c_str());
      return false; // a failed index is a performance miss, never data loss
    }
  }

  void replay() {
    const auto sz=file_size();
    if(sz<kHeader)throw std::runtime_error("QSSD truncated volume header");
    std::array<std::uint8_t,kHeader> head{};
    read_all(fd_,0,head.data(),head.size(),&physical_reads_);
    if(std::memcmp(head.data(),"RQVD0001",8)!=0||
       u64(head.data()+8)!=1||u64(head.data()+24)!=kBlockSize||
       u64(head.data()+32)!=digest(head.data(),32))
      throw std::runtime_error("QSSD invalid volume header");
    capacity_=u64(head.data()+16);
    generation_=u64(head.data()+40); // old C03 images have zero generation
    if(generation_ && u64(head.data()+48)!=digest(head.data(),48))
      throw std::runtime_error("QSSD compacted generation/header checksum mismatch");
    if(capacity_<kGiB||capacity_>kMaxLogicalBytes||capacity_%kBlockSize)
      throw std::runtime_error("QSSD volume logical size invalid");
    // Journal and checkpoint entries already store 64-bit block numbers.
    // Keep the established depth for disks up to 16 TiB and add levels as needed.
    const auto last_block=capacity_/kBlockSize-1;
    for(auto extra=last_block>>(index_levels_*4);extra;extra>>=4)++index_levels_;
    recovery_={};
    dedup_fifo_.clear();dedup_.clear();
    const auto checkpoint=read_checkpoint(sz);
    auto state=checkpoint?*checkpoint:Epoch{};
    std::uint64_t pos=state.end, last_commit=state.end;
    struct ReplayChange { std::uint64_t block,digest; Ref ref; };
    std::vector<ReplayChange> pending;
    pending.reserve(kMaxTransactionBlocks);
    std::vector<Ref> refs;
    refs.reserve(kMaxTransactionBlocks);
    constexpr std::size_t chunk_bytes=1024U*1024U;
    std::vector<std::uint8_t> buffer;
    std::uint64_t buffer_from=pos, buffer_end=pos;
    while(pos<sz){
      if(buffer_end<sz&&buffer_end-pos<kDigestLanes*kRawRecordBytes){
        const auto bytes=std::size_t(std::min<std::uint64_t>(sz-pos,chunk_bytes));
        buffer.resize(bytes);
        read_all(fd_,pos,buffer.data(),bytes,&physical_reads_);
        buffer_from=pos;buffer_end=pos+bytes;
      }
      if(pending.size()+kDigestLanes<=kMaxBulkTransactionBlocks&&
         kDigestLanes*kRawRecordBytes<=buffer_end-pos){
        std::array<const std::uint8_t*,kDigestLanes> headers{};
        const auto* base=buffer.data()+std::size_t(pos-buffer_from);
        bool raw=true;
        for(std::size_t lane=0;lane<kDigestLanes;++lane){
          const auto* header=base+lane*kRawRecordBytes;
          if(u32(header+4)!=Raw||u32(header+16)!=kBlockSize||
             u64(header+8)>=capacity_/kBlockSize){
            raw=false;
            break;
          }
          headers[lane]=header;
        }
        if(raw){
          verify_raw_batch(headers,kDigestLanes);
          for(const auto* header:headers){
            pending.push_back({u64(header+8),u64(header+24),
                               {pos,xor_words(header+kRecHeader),Raw}});
            pos+=kRawRecordBytes;
          }
          recovery_.replayed_records+=kDigestLanes;
          continue;
        }
      }
      auto record=parse_record_at(buffer.data()+std::size_t(pos-buffer_from),pos,
                                  std::size_t(buffer_end-pos));
      if(!record)break; // interrupted tail is never a published transaction
      ++recovery_.replayed_records;
      if(record->kind==Commit) {
        if(pending.empty()||record->block!=state.commits+1||
           record->reference!=pending.size())
          throw std::runtime_error("QSSD transaction commit mismatch");
        for(std::size_t i=0;i<pending.size();){
          refs.clear();
          const auto first=pending[i].block;
          do {
            refs.push_back(pending[i++].ref);
          } while(i<pending.size()&&pending[i].block==first+refs.size());
          state.root=replace_contiguous(state.root,first,index_levels_,refs.data(),refs.size());
        }
        for(const auto& change:pending) {
          switch(change.ref.kind){
            case Raw: ++state.raw;remember(change.digest,change.ref.offset);break;
            case Zero: ++state.zeros;break;
            case Pattern: ++state.patterns;break;
            case Reference: ++state.refs;break;
            default:break;
          }
        }
        ++state.commits;
        pending.clear();
        last_commit=record->next;
      } else {
        if(record->block>=capacity_/kBlockSize||pending.size()>=kMaxBulkTransactionBlocks)
          throw std::runtime_error("QSSD journal record address/count invalid");
        const auto data=decode(*record,sz);
        const auto word=record->kind==Zero?0:xor_rule(data,record->kind==Pattern?record->payload.size():0);
        pending.push_back({record->block,record->digest,{pos,word,record->kind}});
      }
      pos=record->next;
    }
    if(sz!=last_commit && access_==Access::ReadWrite){
      if(::ftruncate(fd_,static_cast<off_t>(last_commit))!=0||sync_volume()!=0)
        throw std::runtime_error("QSSD cannot discard incomplete transaction tail");
    }
    committed_end_=last_commit;
    state.end=last_commit;
    if(state.commits)states_.publish(std::move(state),"replay");
  }

 public:
  // Offline maintenance. The original descriptor stays open and locked until
  // the independently verified new .rqvd has been fsynced and atomically
  // renamed on the SAME filesystem. Only live guest blocks are retained.
  // The temporary image has a new generation; stale .rqix indexes cannot
  // claim its address space. Optional fault injection is strictly for tests.
  static CompactResult compact(const std::filesystem::path& path,
                               bool inject_abort_before_rename=false) {
    check_path(path);
    CompactResult result{};
    const auto temp=std::filesystem::path(temp_name(path,".c08-staging").string()+".rqvd");
    bool renamed=false;
    try {
      {
        QssdDisk original(path);
        const auto original_view=original.states_.snapshot();
        const auto root=original_view->value->root;
        const auto old_end=original_view->value->end;
        result.before_bytes=old_end;
        result.old_commits=original_view->value->commits;
        if(original.generation_==UINT64_MAX)
          throw std::runtime_error("QSSD generation exhausted");
        result.generation=original.generation_+1;
        int out=::open(temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(out<0)throw std::system_error(errno,std::generic_category(),"QSSD compact temp create");
        try {
          std::array<std::uint8_t,kHeader> head{};
          read_all(original.fd_,0,head.data(),head.size());
          put64(head.data()+40,result.generation);
          put64(head.data()+48,digest(head.data(),48));
          write_all(out,0,head.data(),head.size());
          std::uint64_t position=kHeader, committed=0;
          unsigned staged=0;
          auto finish_batch=[&](){
            if(!staged)return;
            position=append_to(out,position,Commit,committed+1,0,staged,{});
            ++committed;staged=0;
          };
          auto transfer=[&](std::uint64_t block,std::uint64_t offset){
            const auto data=original.decode(offset,old_end);
            // An all-zero block is indistinguishable from an absent block and
            // can be safely removed from the compacted sparse map.
            const auto period=rule_period(data);
            if(period==1&&!data.front())return;
            const auto kind=period?Pattern:Raw;
            const std::vector<std::uint8_t> payload(data.begin(),
                data.begin()+(period?period:data.size()));
            position=append_to(out,position,kind,block,digest(data.data(),data.size()),0,payload);
            ++result.live_blocks;
            if(++staged==kMaxTransactionBlocks)finish_batch();
          };
          each_live(root,original.index_levels_,0,transfer);
          finish_batch();
          result.compact_commits=committed;
          result.after_bytes=position;
          if(::fsync(out)!=0)
            throw std::system_error(errno,std::generic_category(),"QSSD compact file fsync");
          ::close(out);out=-1;
        }catch(...){
          ::close(out);
          throw;
        }
        // Independently re-open, replay every new commit and compare the
        // complete decoded bytes of every previously live block before rename.
        {
          QssdDisk candidate(temp);
          if(candidate.capacity_!=original.capacity_ ||
             candidate.generation_!=result.generation ||
             candidate.file_size()!=result.after_bytes)
            throw std::runtime_error("QSSD compact candidate metadata mismatch");
          std::array<std::uint8_t,kBlockSize> actual{};
          auto verify=[&](std::uint64_t block,std::uint64_t offset) {
            const auto expected=original.decode(offset,old_end);
            if(!candidate.read_bytes(block*kBlockSize,actual.data(),actual.size()) ||
               actual!=expected)
              throw std::runtime_error("QSSD compact candidate differs from guest block state");
          };
          each_live(root,original.index_levels_,0,verify);
        }
        if(inject_abort_before_rename)
          throw std::runtime_error("C08 injected crash point: before atomic compact rename");
        // Detect unexpected replacement of the original path since open.
        struct stat handle{},name{};
        if(::fstat(original.fd_,&handle)!=0||
           ::lstat(path.c_str(),&name)!=0||
           handle.st_dev!=name.st_dev||handle.st_ino!=name.st_ino)
          throw std::runtime_error("QSSD source image changed during offline compact");
        if(::rename(temp.c_str(),path.c_str())!=0)
          throw std::system_error(errno,std::generic_category(),"QSSD compact rename");
        renamed=true;
        fsync_parent(path); // durable name publication; old/new are each valid
        result.replaced=true;
      }
      // The obsolete checkpoint has a DIFFERENT generation. Generating a new
      // checkpoint is optional: failure cannot invalidate a verified image.
      QssdDisk after(path);
      result.checkpoint_saved=after.checkpoint();
      return result;
    }catch(...){
      if(!renamed)(void)::unlink(temp.c_str());
      throw;
    }
  }

  static void create(const std::filesystem::path& path,std::uint64_t gib) {
    if(gib<1||gib>kMaxLogicalBytes/kGiB)
      throw std::invalid_argument("QSSD supported logical capacity is 1 GiB .. 8 PiB");
    create_bytes(path,gib*kGiB);
  }
  static void create_bytes(const std::filesystem::path& path,std::uint64_t bytes) {
    check_path(path);
    if(bytes<kGiB||bytes>kMaxLogicalBytes||bytes%kBlockSize)
      throw std::invalid_argument("QSSD capacity must be block aligned and within 1 GiB .. 8 PiB");
    const auto name=path.string();
    int fd=::open(name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"QSSD create without overwrite");
    bool complete=false;
    try{
      std::array<std::uint8_t,kHeader> head{};
      std::memcpy(head.data(),"RQVD0001",8);
      put64(head.data()+8,1);
      put64(head.data()+16,bytes);
      put64(head.data()+24,kBlockSize);
      put64(head.data()+32,digest(head.data(),32));
      write_all(fd,0,head.data(),head.size());
      if(::fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"QSSD create sync");
      fsync_parent(path);
      complete=true;
    }catch(...){::close(fd);::unlink(name.c_str());throw;}
    ::close(fd);
    (void)complete;
  }
  explicit QssdDisk(const std::filesystem::path& path,Access access=Access::ReadWrite)
      : access_(access),path_(path) {
    check_path(path);
    const bool writable=access_==Access::ReadWrite;
    fd_=::open(path.c_str(),(writable?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW);
    if(fd_<0)throw std::system_error(errno,std::generic_category(),"QSSD open");
    try{
      if(::flock(fd_,(writable?LOCK_EX:LOCK_SH)|LOCK_NB)!=0)
        throw std::runtime_error("QSSD volume already mounted for writing or maintenance");
      struct stat st{};
      if(::fstat(fd_,&st)!=0||!S_ISREG(st.st_mode))
        throw std::runtime_error("QSSD volume is not a regular file");
      replay();
    }catch(...){::close(fd_);fd_=-1;throw;}
  }
  ~QssdDisk() override {if(fd_>=0)::close(fd_);}
  QssdDisk(const QssdDisk&)=delete;
  QssdDisk& operator=(const QssdDisk&)=delete;
  std::uint64_t bytes() const noexcept override {return capacity_;}
  std::uint64_t sectors() const noexcept override {return capacity_/kSectorSize;}
  Stats stats() const {
    const auto v=states_.snapshot();
    return {capacity_,v->value->end,v->value->commits,v->value->raw,
            v->value->zeros,v->value->patterns,v->value->refs,v->serial};
  }
  RecoveryStats recovery_stats() const {return recovery_;}
  // Exact XOR of little-endian 64-bit words across the current logical disk.
  // Unwritten blocks are zero. One immutable StateBus lease supplies both
  // the revision and its root result, regardless of concurrent appends.
  XorSummary xor_summary() const noexcept {
    const auto view=states_.snapshot();
    const auto& root=view->value->root;
    if(!root)return {view->serial,0,0,0};
    auto word=root->word_xor;
    auto parity=0U;
    for(;word;word&=word-1)parity^=1U;
    return {view->serial,root->word_xor,root->nonzero_blocks,parity};
  }
  IoStats io_stats() const noexcept {
    constexpr auto relaxed=std::memory_order_relaxed;
    return {physical_reads_.calls.load(relaxed),physical_reads_.bytes.load(relaxed),
            physical_writes_.calls.load(relaxed),physical_writes_.bytes.load(relaxed),
            physical_syncs_.calls.load(relaxed),physical_reads_.ns.load(relaxed),
            physical_writes_.ns.load(relaxed),physical_syncs_.ns.load(relaxed)};
  }
  std::uint64_t generation() const noexcept {return generation_;}
  CacheStats cache_stats() const {
    std::lock_guard<std::mutex> hold(cache_mutex_);
    const auto blocks=std::uint64_t(read_cache_.size());
    return {cache_hits_.load(std::memory_order_relaxed),
            cache_misses_.load(std::memory_order_relaxed),blocks,
            blocks*kBlockSize};
  }
  bool checkpoint() {
    if(access_==Access::ReadOnly)return false;
    std::lock_guard<std::mutex> hold(writer_);
    return save_checkpoint_locked();
  }

  bool read_bytes(std::uint64_t offset,std::uint8_t* target,std::size_t size) override {
    if((!target&&size)||offset>capacity_||size>capacity_-offset)return false;
    try{
      const auto view=states_.snapshot(); // one coherent qSSD version for this request
      std::vector<std::uint8_t> raw_buffer;
      std::size_t done=0;
      while(done<size){
        const auto block=(offset+done)/kBlockSize;
        const auto start=static_cast<std::size_t>((offset+done)%kBlockSize);
        const auto n=std::min<std::size_t>(size-done,kBlockSize-start);
        const auto ref=locate(view->value->root,block);
        if(start==0&&ref.kind==Raw&&(size-done)/kBlockSize>=kDigestLanes){
          const auto max_count=std::min(kMaxTransactionBlocks,(size-done)/kBlockSize);
          std::size_t count=1;
          for(;count<max_count;++count){
            const auto next=locate(view->value->root,block+count);
            if(next.kind!=Raw||next.offset!=ref.offset+count*kRawRecordBytes)break;
          }
          if(count>=kDigestLanes){
            read_raw_run(ref.offset,block,count,view->value->end,raw_buffer,target+done);
            done+=count*kBlockSize;
            continue;
          }
        }
        if(!ref.offset)std::memset(target+done,0,n);
        else {
          const auto data=cached_decode(ref.offset,view->value->end);
          std::memcpy(target+done,data->data()+start,n);
        }
        done+=n;
      }
      return true;
    }catch(const std::exception&){return false;}
  }
  bool read(std::uint64_t lba,std::uint16_t count,std::uint8_t* target) override {
    if(!target||!count||lba>sectors()||count>sectors()-lba)return false;
    return read_bytes(lba*kSectorSize,target,std::size_t(count)*kSectorSize);
  }
  bool flush() override {
    if(access_==Access::ReadOnly)return false;
    std::lock_guard<std::mutex> hold(writer_);
    return fd_ >= 0 && sync_volume() == 0;
  }

  bool write(std::uint64_t lba,std::uint16_t count,const std::uint8_t* source) override {
    if(!count||count>127||lba>sectors()||count>sectors()-lba)return false;
    return write_bytes(lba*kSectorSize,std::size_t(count)*kSectorSize,source);
  }

  bool write_bytes(std::uint64_t first,std::size_t total,const std::uint8_t* source) {
    if(access_==Access::ReadOnly||first>capacity_||total>capacity_-first||
       total>kMaxBulkTransactionBytes-first%kBlockSize||(!source&&total))return false;
    if(!total)return true;
    std::lock_guard<std::mutex> hold(writer_);
    bool appending=false;
    try{
      const auto old=states_.snapshot();
      Epoch next=*old->value;
      const auto original_end=committed_end_;
      struct RawCandidate {std::uint64_t hash,offset,word_xor;};
      std::vector<RawCandidate> new_raw;
      std::unordered_multimap<std::uint64_t,std::size_t> staged_hashes;
      std::vector<std::size_t> cache_positions;
      const auto block_count=(first%kBlockSize+total+kBlockSize-1)/kBlockSize;
      new_raw.reserve(block_count);
      staged_hashes.reserve(block_count);
      cache_positions.reserve(block_count);
      std::vector<Ref> replacements;
      replacements.reserve(block_count);
      std::vector<std::size_t> raw_positions;
      raw_positions.reserve(block_count);
      std::vector<std::uint8_t> journal;
      journal.reserve(block_count*(kRecHeader+kBlockSize+kRecFooter)+kRecHeader+kRecFooter);
      std::size_t done=0,digested_lane=kDigestLanes;
      std::array<std::uint64_t,kDigestLanes> content_digests{};
      while(done<total){
        const auto block=(first+done)/kBlockSize;
        const auto start=static_cast<std::size_t>((first+done)%kBlockSize);
        const auto n=std::min<std::size_t>(total-done,kBlockSize-start);
        BlockData data;
        auto hash=std::uint64_t{0};
        if(n==kBlockSize){
          std::memcpy(data.data(),source+done,n);
          if(digested_lane==kDigestLanes&&total-done>=kDigestLanes*kBlockSize){
            std::array<const std::uint8_t*,kDigestLanes> blocks{};
            for(std::size_t lane=0;lane<kDigestLanes;++lane)
              blocks[lane]=source+done+lane*kBlockSize;
            content_digests=digest_four(blocks,kBlockSize);
            digested_lane=0;
          }
          hash=digested_lane<kDigestLanes?content_digests[digested_lane++]:digest(data.data(),n);
        }else{
          const auto ref=locate(old->value->root,block);
          if(ref.offset)data=*cached_decode(ref.offset,original_end);
          else data.fill(0);
          std::memcpy(data.data()+start,source+done,n);
          hash=digest(data.data(),data.size());
        }
        const auto period=rule_period(data);
        Kind kind=Raw;
        std::uint64_t backref=0;
        std::uint64_t word_xor=0;
        std::size_t payload=data.size();
        if(period){
          kind=period==1&&!data.front()?Zero:Pattern;
          payload=kind==Zero?0:period;
          word_xor=xor_rule(data,period);
        }else{
          const auto matching=staged_hashes.equal_range(hash);
          for(auto entry=matching.first;entry!=matching.second;++entry){
            const auto& staged=new_raw[entry->second];
            const auto* bytes=journal.data()+std::size_t(staged.offset-original_end)+kRecHeader;
            if(std::memcmp(bytes,data.data(),data.size())!=0||(backref&&staged.offset>=backref))
              continue;
            backref=staged.offset;
            word_xor=staged.word_xor;
          }
          if(!backref){
            word_xor=xor_words(data.data());
            const auto it=dedup_.find(hash);
            if(it!=dedup_.end())for(const auto candidate:it->second){
              if(*cached_decode(candidate,original_end)==data){backref=candidate;break;}
            }
          }
          if(backref){kind=Reference;payload=0;}
        }
        const auto position=journal.size();
        const auto record_offset=original_end+position;
        if(kind==Raw)
          raw_positions.push_back(append_record(journal,kind,block,hash,backref,data.data(),payload));
        else
          encode_record(journal,kind,block,hash,backref,data.data(),payload);
        if(kind==Raw){
          staged_hashes.emplace(hash,new_raw.size());
          new_raw.push_back({hash,record_offset,word_xor});
          ++next.raw;
        }else if(kind==Zero)++next.zeros;
        else if(kind==Pattern)++next.patterns;
        else ++next.refs;
        if(kind==Raw||kind==Pattern)cache_positions.push_back(position);
        replacements.push_back({record_offset,word_xor,kind});
        done+=n;
      }
      finish_raw_records(journal,raw_positions);
      next.root=replace_contiguous(next.root,first/kBlockSize,index_levels_,
                                   replacements.data(),replacements.size());
      ++next.commits;
      encode_record(journal,Commit,next.commits,0,block_count,nullptr,0);
      const auto end=original_end+journal.size();
      next.end=end;
      appending=true;
      write_all(fd_,original_end,journal.data(),journal.size(),&physical_writes_);
      if(sync_volume()!=0)throw std::system_error(errno,std::generic_category(),"QSSD transaction fsync");
      states_.publish(std::move(next),"guest-sector-commit",old);
      committed_end_=end;
      // Dedup index is a disposable optimization; its allocation must never
      // turn a committed guest write into an acknowledged failure or rollback.
      try { for(const auto& entry:new_raw)remember(entry.hash,entry.offset); }
      catch(const std::exception&) {dedup_.clear();dedup_fifo_.clear();}
      // Discardable cache mutations cannot invalidate an already fsynced
      // and published guest transaction.
      try {
        std::lock_guard<std::mutex> guard(cache_mutex_);
        const auto keep=std::min(cache_positions.size(),kReadCacheBlocks);
        for(auto i=cache_positions.size()-keep;i<cache_positions.size();++i){
          const auto position=cache_positions[i];
          const auto* record=journal.data()+position;
          auto block=std::make_shared<BlockData>();
          expand_payload(*block,record+kRecHeader,u32(record+16));
          cache_insert_locked(original_end+position,std::move(block));
        }
      }
      catch(const std::exception&){}
      return true;
    }catch(const std::exception&){
      // Discard any uncommitted append. A failed write is never acknowledged.
      if(appending){
        (void)::ftruncate(fd_,static_cast<off_t>(committed_end_));
        (void)sync_volume();
      }
      return false;
    }
  }
};
} // namespace reverse_windows
