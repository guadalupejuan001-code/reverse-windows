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
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
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
  static constexpr std::uint64_t kSectorSize = 512, kBlockSize = 4096;
  static constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
  struct Stats {
    std::uint64_t logical_bytes, physical_bytes, commits, raw_blocks;
    std::uint64_t zero_blocks, pattern_blocks, deduplicated_blocks, qssd_serial;
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
  static constexpr std::size_t kDedupWindow=4096;
  int fd_=-1;
  std::uint64_t capacity_=0, committed_end_=kHeader;
  std::mutex writer_;
  qssd::StateBus<Epoch> states_{Epoch{}, 32};
  std::deque<std::pair<std::uint64_t,std::uint64_t>> dedup_fifo_;
  std::unordered_map<std::uint64_t,std::vector<std::uint64_t>> dedup_;

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
  static std::uint64_t digest(const std::uint8_t* p,std::size_t n) {
    std::uint64_t h=14695981039346656037ULL;
    for(std::size_t i=0;i<n;++i){h^=p[i];h*=1099511628211ULL;}
    return h;
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
  std::uint64_t append(std::uint64_t at,Kind kind,std::uint64_t block,
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
    write_all(fd_,at,bytes.data(),bytes.size());
    return at+bytes.size();
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
    if(capacity_<kGiB||capacity_>16384*kGiB||capacity_%kBlockSize)
      throw std::runtime_error("QSSD volume logical size invalid");
    auto state=Epoch{};
    std::uint64_t pos=kHeader, last_commit=kHeader;
    std::vector<Change> pending;
    while(pos<sz){
      auto record=record_at(pos,sz);
      if(!record)break; // interrupted tail is never a published transaction
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
    if(sz!=last_commit){
      if(::ftruncate(fd_,static_cast<off_t>(last_commit))!=0||::fsync(fd_)!=0)
        throw std::runtime_error("QSSD cannot discard incomplete transaction tail");
    }
    committed_end_=last_commit;
    state.end=last_commit;
    if(state.commits)states_.publish(std::move(state),"replay");
  }

 public:
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
  explicit QssdDisk(const std::filesystem::path& path) {
    check_path(path);
    fd_=::open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd_<0)throw std::system_error(errno,std::generic_category(),"QSSD open");
    try{
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
          const auto data=decode(ref.offset,view->value->end);
          std::memcpy(target+done,data.data()+start,n);
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
    std::lock_guard<std::mutex> hold(writer_);
    return fd_ >= 0 && ::fsync(fd_) == 0;
  }

  bool write(std::uint64_t lba,std::uint16_t count,const std::uint8_t* source) override {
    if(!source||!count||count>127||lba>sectors()||count>sectors()-lba)return false;
    std::lock_guard<std::mutex> hold(writer_);
    try{
      const auto old=states_.snapshot();
      Epoch next=*old->value;
      const auto original_end=committed_end_;
      std::uint64_t at=original_end;
      std::vector<Change> changes;
      std::vector<std::pair<std::uint64_t,std::uint64_t>> new_raw;
      const std::uint64_t first=lba*kSectorSize, total=std::uint64_t(count)*kSectorSize;
      std::uint64_t done=0;
      while(done<total){
        const auto block=(first+done)/kBlockSize;
        const auto start=static_cast<std::size_t>((first+done)%kBlockSize);
        const auto n=std::min<std::uint64_t>(total-done,kBlockSize-start);
        auto ref=locate(next.root,block);
        auto data=ref.offset?decode(ref.offset,original_end):
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
