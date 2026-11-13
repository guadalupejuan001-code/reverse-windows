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


 private:
  // Immutable 16-way radix nodes allow COW block-index updates in O(log16 blocks).
  // Old qSSD epoch views retain the exact old block map without copying a disk.
  struct Ref { std::uint64_t offset=0; };
  struct Node {
    std::array<std::shared_ptr<const Node>, 16> children{};
    Ref ref{};
  };
  using NodePtr=std::shared_ptr<const Node>;
  struct Epoch {
    NodePtr root;
    std::uint64_t commits=0, raw=0, zeros=0, patterns=0, refs=0, end=kHeader;
  };
  enum Kind : std::uint32_t { Raw=1, Zero=2, Pattern=3, Reference=4, Commit=5 };
  struct Record {
    Kind kind;
    std::uint64_t block, digest, reference, offset, next;
    std::vector<std::uint8_t> payload;
  };
  struct Change { std::uint64_t block, offset; Kind kind; std::uint64_t digest; };

  static constexpr std::size_t kHeader=64, kRecHeader=40, kRecFooter=16;
  static constexpr std::size_t kIndexHeader=112, kIndexEntry=16;
  static constexpr std::uint64_t kFNVStart=14695981039346656037ULL;

  static constexpr std::size_t kDedupWindow=4096;
  static constexpr std::size_t kReadCacheBlocks=128; // 512 KiB bounded data
  int fd_=-1;
  const Access access_;
  std::filesystem::path path_;
  std::uint64_t capacity_=0, committed_end_=kHeader, generation_=0;
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
    for(std::size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;}
    return h;
  }
  static std::uint64_t digest(const std::uint8_t* p,std::size_t n) {
    return digest_update(kFNVStart,p,n);
  }
  static std::uint64_t fd_digest(int fd,std::uint64_t end) {
    std::array<std::uint8_t,65536> page{};
    auto h=kFNVStart;
    for(std::uint64_t pos=0;pos<end;) {
      const auto chunk=std::size_t(std::min<std::uint64_t>(end-pos,page.size()));
      read_all(fd,pos,page.data(),chunk);
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
  static void read_all(int fd,std::uint64_t off,void* ptr,std::size_t size) {
    auto* out=static_cast<std::uint8_t*>(ptr);
    for(std::size_t done=0;done<size;) {
      const auto n=::pread(fd,out+done,size-done,static_cast<off_t>(off+done));
      if(n<0&&errno==EINTR)continue;
      if(n<=0)throw std::runtime_error("QSSD truncated or unreadable block record");
      done+=static_cast<std::size_t>(n);
    }
  }
  static void write_all(int fd,std::uint64_t off,const void* ptr,std::size_t size) {
    const auto* in=static_cast<const std::uint8_t*>(ptr);
    for(std::size_t done=0;done<size;) {
      const auto n=::pwrite(fd,in+done,size-done,static_cast<off_t>(off+done));
      if(n<0&&errno==EINTR)continue;
      if(n<=0)throw std::system_error(n<0?errno:EIO,std::generic_category(),"QSSD write");
      done+=static_cast<std::size_t>(n);
    }
  }
  std::uint64_t file_size() const {
    struct stat info{};
    if(::fstat(fd_,&info)!=0||info.st_size<0)throw std::runtime_error("QSSD stat failed");
    return static_cast<std::uint64_t>(info.st_size);
  }
  static Ref locate(NodePtr node,std::uint64_t block) {
    for(int level=7;level>=0;--level){
      if(!node)return {};
      node=node->children[(block>>(level*4))&15];
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
  static NodePtr replace(NodePtr node,std::uint64_t block,int level,Ref ref) {
    auto next=std::make_shared<Node>(node?*node:Node{});
    if(level==0) next->ref=ref;
    else {
      const auto slot=unsigned((block>>((level-1)*4))&15);
      next->children[slot]=replace(next->children[slot],block,level-1,ref);
    }
    return next;
  }

  // nullptr means incomplete tail, never "valid but corrupt".
  std::unique_ptr<Record> record_at(std::uint64_t offset,std::uint64_t limit) const {
    if(offset>limit||limit-offset<kRecHeader)return {};
    std::array<std::uint8_t,kRecHeader> h{};
    read_all(fd_,offset,h.data(),h.size());
    if(std::memcmp(h.data(),"QBLK",4)!=0)throw std::runtime_error("QSSD record magic corruption");
    const auto kind=u32(h.data()+4), len=u32(h.data()+16);
    if(kind<Raw||kind>Commit||len>kBlockSize)
      throw std::runtime_error("QSSD unknown record type or oversize payload");
    if((kind==Raw&&len!=kBlockSize)||(kind==Zero&&len!=0)||
       (kind==Pattern&&(len<1||len>32))||
       ((kind==Reference||kind==Commit)&&len!=0))
      throw std::runtime_error("QSSD invalid record length");
    const auto total=kRecHeader+std::uint64_t(len)+kRecFooter;
    if(total>limit-offset)return {};
    std::vector<std::uint8_t> all(static_cast<std::size_t>(total));
    read_all(fd_,offset,all.data(),all.size());
    const auto* footer=all.data()+kRecHeader+len;
    if(std::memcmp(footer,"QEND",4)!=0||u32(footer+4)!=total||
       u64(footer+8)!=digest(all.data(),kRecHeader+len))
      throw std::runtime_error("QSSD journal checksum corruption");
    auto out=std::make_unique<Record>();
    out->kind=static_cast<Kind>(kind);
    out->block=u64(h.data()+8);
    out->digest=u64(h.data()+24);
    out->reference=u64(h.data()+32);
    out->offset=offset;out->next=offset+total;
    out->payload.assign(all.begin()+kRecHeader,all.begin()+kRecHeader+len);
    return out;
  }
  std::array<std::uint8_t,kBlockSize> decode(std::uint64_t offset,std::uint64_t limit) const {
    auto r=record_at(offset,limit);
    if(!r||r->kind==Commit)throw std::runtime_error("QSSD missing stored data block");
    std::array<std::uint8_t,kBlockSize> data{};
    if(r->kind==Raw) std::copy(r->payload.begin(),r->payload.end(),data.begin());
    else if(r->kind==Pattern) {
      for(std::size_t i=0;i<kBlockSize;++i)data[i]=r->payload[i%r->payload.size()];
    } else if(r->kind==Reference) {
      if(r->reference<kHeader||r->reference>=offset)
        throw std::runtime_error("QSSD forward/dangling content reference");
      auto raw=record_at(r->reference,limit);
      if(!raw||raw->kind!=Raw)throw std::runtime_error("QSSD REF must point directly to RAW");
      std::copy(raw->payload.begin(),raw->payload.end(),data.begin());
    }
    if(digest(data.data(),data.size())!=r->digest)
      throw std::runtime_error("QSSD decoded block digest differs");
    return data;
  }
  static std::uint64_t append_to(int fd,std::uint64_t at,Kind kind,std::uint64_t block,
                       std::uint64_t hash,std::uint64_t reference,
                       const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> bytes(kRecHeader+payload.size()+kRecFooter);
    std::memcpy(bytes.data(),"QBLK",4);
    put32(bytes.data()+4,kind);
    put64(bytes.data()+8,block);
    put32(bytes.data()+16,static_cast<std::uint32_t>(payload.size()));
    put64(bytes.data()+24,hash);
    put64(bytes.data()+32,reference);
    if(!payload.empty())std::copy(payload.begin(),payload.end(),bytes.begin()+kRecHeader);
    auto* end=bytes.data()+kRecHeader+payload.size();
    std::memcpy(end,"QEND",4);
    put32(end+4,static_cast<std::uint32_t>(bytes.size()));
    put64(end+8,digest(bytes.data(),kRecHeader+payload.size()));
    write_all(fd,at,bytes.data(),bytes.size());
    return at+bytes.size();
  }
  void cache_insert(std::uint64_t off,std::shared_ptr<const BlockData> data) const {
    std::lock_guard<std::mutex> guard(cache_mutex_);
    if(read_cache_.find(off)!=read_cache_.end())return;
    if(read_fifo_.size()>=kReadCacheBlocks) {
      const auto old=read_fifo_.front();read_fifo_.pop_front();
      read_cache_.erase(old);
    }
    read_cache_.emplace(off,std::move(data));
    read_fifo_.push_back(off);
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

  std::uint64_t append(std::uint64_t at,Kind kind,std::uint64_t block,
                       std::uint64_t hash,std::uint64_t reference,
                       const std::vector<std::uint8_t>& payload) {
    return append_to(fd_,at,kind,block,hash,reference,payload);
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
      if(fd_digest(fd_,cutoff)!=u64(head.data()+88))
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
        (void)decode(off,cutoff);
        state.root=replace(state.root,block,8,Ref{off});
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
      put64(header.data()+88,fd_digest(fd_,state.end));
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
      each_live(state.root,8,0,append_entry);
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
    read_all(fd_,0,head.data(),head.size());
    if(std::memcmp(head.data(),"RQVD0001",8)!=0||
       u64(head.data()+8)!=1||u64(head.data()+24)!=kBlockSize||
       u64(head.data()+32)!=digest(head.data(),32))
      throw std::runtime_error("QSSD invalid volume header");
    capacity_=u64(head.data()+16);
    generation_=u64(head.data()+40); // old C03 images have zero generation
    if(generation_ && u64(head.data()+48)!=digest(head.data(),48))
      throw std::runtime_error("QSSD compacted generation/header checksum mismatch");
    if(capacity_<kGiB||capacity_>16384*kGiB||capacity_%kBlockSize)
      throw std::runtime_error("QSSD volume logical size invalid");
    recovery_={};
    dedup_fifo_.clear();dedup_.clear();
    const auto checkpoint=read_checkpoint(sz);
    auto state=checkpoint?*checkpoint:Epoch{};
    std::uint64_t pos=state.end, last_commit=state.end;
    std::vector<Change> pending;
    while(pos<sz){
      auto record=record_at(pos,sz);
      if(!record)break; // interrupted tail is never a published transaction
      ++recovery_.replayed_records;
      if(record->kind==Commit) {
        if(pending.empty()||record->block!=state.commits+1||
           record->reference!=pending.size())
          throw std::runtime_error("QSSD transaction commit mismatch");
        for(const auto& p:pending) {
          state.root=replace(state.root,p.block,8,Ref{p.offset});
          switch(p.kind){
            case Raw: ++state.raw;remember(p.digest,p.offset);break;
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
        if(record->block>=capacity_/kBlockSize||pending.size()>=128)
          throw std::runtime_error("QSSD journal record address/count invalid");
        (void)decode(pos,sz); // verify actual recoverable bytes before publishing
        pending.push_back({record->block,pos,record->kind,record->digest});
      }
      pos=record->next;
    }
    if(sz!=last_commit && access_==Access::ReadWrite){
      if(::ftruncate(fd_,static_cast<off_t>(last_commit))!=0||::fsync(fd_)!=0)
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
            if(std::all_of(data.begin(),data.end(),[](auto v){return v==0;}))
              return;
            std::vector<std::uint8_t> payload;
            Kind kind=Raw;
            for(unsigned period=1;period<=32;++period) {
              bool repeating=true;
              for(std::size_t i=period;i<kBlockSize;++i)
                if(data[i]!=data[i%period]) {repeating=false;break;}
              if(repeating) {
                kind=Pattern;payload.assign(data.begin(),data.begin()+period);
                break;
              }
            }
            if(kind==Raw)payload.assign(data.begin(),data.end());
            position=append_to(out,position,kind,block,digest(data.data(),data.size()),0,payload);
            ++result.live_blocks;
            if(++staged==128)finish_batch(); // replay bound: <=128 per commit
          };
          each_live(root,8,0,transfer);
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
          each_live(root,8,0,verify);
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
    check_path(path);
    if(gib<1||gib>16384)throw std::invalid_argument("QSSD supported logical capacity is 1 GiB .. 16 TiB");
    const auto name=path.string();
    int fd=::open(name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)throw std::system_error(errno,std::generic_category(),"QSSD create without overwrite");
    bool complete=false;
    try{
      std::array<std::uint8_t,kHeader> head{};
      std::memcpy(head.data(),"RQVD0001",8);
      put64(head.data()+8,1);
      put64(head.data()+16,gib*kGiB);
      put64(head.data()+24,kBlockSize);
      put64(head.data()+32,digest(head.data(),32));
      write_all(fd,0,head.data(),head.size());
      if(::fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"QSSD create sync");
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
      std::size_t done=0;
      while(done<size){
        const auto block=(offset+done)/kBlockSize;
        const auto start=static_cast<std::size_t>((offset+done)%kBlockSize);
        const auto n=std::min<std::size_t>(size-done,kBlockSize-start);
        const auto ref=locate(view->value->root,block);
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
    return fd_ >= 0 && ::fsync(fd_) == 0;
  }

  bool write(std::uint64_t lba,std::uint16_t count,const std::uint8_t* source) override {
    if(access_==Access::ReadOnly||!source||!count||count>127||lba>sectors()||count>sectors()-lba)return false;
    std::lock_guard<std::mutex> hold(writer_);
    try{
      const auto old=states_.snapshot();
      Epoch next=*old->value;
      const auto original_end=committed_end_;
      std::uint64_t at=original_end;
      std::vector<Change> changes;
      std::vector<std::pair<std::uint64_t,std::uint64_t>> new_raw;
      std::vector<std::pair<std::uint64_t,std::shared_ptr<const BlockData>>> prepared_cache;
      const std::uint64_t first=lba*kSectorSize, total=std::uint64_t(count)*kSectorSize;
      std::uint64_t done=0;
      while(done<total){
        const auto block=(first+done)/kBlockSize;
        const auto start=static_cast<std::size_t>((first+done)%kBlockSize);
        const auto n=std::min<std::uint64_t>(total-done,kBlockSize-start);
        auto ref=locate(next.root,block);
        auto data=ref.offset?*cached_decode(ref.offset,original_end):
                    std::array<std::uint8_t,kBlockSize>{};
        std::memcpy(data.data()+start,source+done,static_cast<std::size_t>(n));
        const auto hash=digest(data.data(),data.size());
        Kind kind=Raw;
        std::uint64_t backref=0;
        std::vector<std::uint8_t> payload;
        if(std::all_of(data.begin(),data.end(),[](auto b){return b==0;})) {
          kind=Zero;
        }else{
          for(unsigned period=1;period<=32;++period){
            bool equal=true;
            for(std::size_t i=period;i<kBlockSize;++i)
              if(data[i]!=data[i%period]){equal=false;break;}
            if(equal){
              kind=Pattern;
              payload.assign(data.begin(),data.begin()+period);
              break;
            }
          }
          if(kind==Raw){
            const auto it=dedup_.find(hash);
            if(it!=dedup_.end())for(const auto candidate:it->second){
              if(decode(candidate,original_end)==data){kind=Reference;backref=candidate;break;}
            }
          }
          if(kind==Raw)payload.assign(data.begin(),data.end());
        }
        const auto record_offset=at;
        at=append(at,kind,block,hash,backref,payload);
        changes.push_back({block,record_offset,kind,hash});
        prepared_cache.emplace_back(record_offset,std::make_shared<const BlockData>(data));
        next.root=replace(next.root,block,8,Ref{record_offset});
        if(kind==Raw)new_raw.emplace_back(hash,record_offset);
        done+=n;
      }
      at=append(at,Commit,next.commits+1,0,changes.size(),{});
      if(::fsync(fd_)!=0)throw std::system_error(errno,std::generic_category(),"QSSD transaction fsync");
      for(const auto& c:changes){
        if(c.kind==Raw)++next.raw;
        else if(c.kind==Zero)++next.zeros;
        else if(c.kind==Pattern)++next.patterns;
        else if(c.kind==Reference)++next.refs;
      }
      ++next.commits;
      next.end=at;
      states_.publish(std::move(next),"guest-sector-commit",old);
      committed_end_=at;
      // Dedup index is a disposable optimization; its allocation must never
      // turn a committed guest write into an acknowledged failure or rollback.
      try { for(const auto& entry:new_raw)remember(entry.first,entry.second); }
      catch(const std::exception&) {dedup_.clear();dedup_fifo_.clear();}
      // Discardable cache mutations cannot invalidate an already fsynced
      // and published guest transaction.
      try {for(const auto& entry:prepared_cache)cache_insert(entry.first,entry.second);}
      catch(const std::exception&){}
      return true;
    }catch(const std::exception&){
      // Discard any uncommitted append. A failed write is never acknowledged.
      (void)::ftruncate(fd_,static_cast<off_t>(committed_end_));
      (void)::fsync(fd_);
      return false;
    }
  }
};
} // namespace reverse_windows
