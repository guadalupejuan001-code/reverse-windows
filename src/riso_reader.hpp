#pragma once
// REVERSE Windows C02: read REISO01 as the original byte-addressed ISO.
// The format remains a container (.riso), NOT a Windows-readable disc image.
// This reader never copies the full ISO to disk or RAM.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <zlib.h>

namespace reverse_windows {
namespace riso_detail {
class Sha256 {
 public:
  Sha256() : h_{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                 0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}} {}
  void update(const std::uint8_t* data, std::size_t length) {
    if (length && !data) throw std::invalid_argument("SHA256: null data");
    if (length > (std::numeric_limits<std::uint64_t>::max() - bits_) / 8)
      throw std::overflow_error("SHA256: input too large");
    bits_ += std::uint64_t(length) * 8;
    for (std::size_t i = 0; i < length; ++i) {
      chunk_[used_++] = data[i];
      if (used_ == 64) { transform(chunk_.data()); used_ = 0; }
    }
  }
  std::array<std::uint8_t,32> finish() {
    const auto bit_count = bits_;
    chunk_[used_++] = 0x80;
    if (used_ > 56) {
      std::fill(chunk_.begin()+used_,chunk_.end(),0);
      transform(chunk_.data()); used_=0;
    }
    std::fill(chunk_.begin()+used_,chunk_.begin()+56,0);
    for (int i=0;i<8;++i) chunk_[56+i] = std::uint8_t(bit_count >> (56-8*i));
    transform(chunk_.data()); used_=0;
    std::array<std::uint8_t,32> result{};
    for (int i=0;i<8;++i) for (int j=0;j<4;++j)
      result[4*i+j] = std::uint8_t(h_[i] >> (24-8*j));
    return result;
  }
  static std::array<std::uint8_t,32> digest(const std::vector<std::uint8_t>& bytes) {
    Sha256 sha; sha.update(bytes.data(), bytes.size());return sha.finish();
  }
 private:
  static std::uint32_t rotate(std::uint32_t x,unsigned n) { return (x >> n) | (x << (32-n)); }
  void transform(const std::uint8_t* p) {
    static constexpr std::uint32_t K[64] = {
      0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
      0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
      0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
      0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
      0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
      0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
      0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
      0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    std::uint32_t w[64]{};
    for(int i=0;i<16;++i)
      w[i]=(std::uint32_t(p[i*4])<<24)|(std::uint32_t(p[i*4+1])<<16)|
            (std::uint32_t(p[i*4+2])<<8)|p[i*4+3];
    for(int i=16;i<64;++i) {
      const auto a=rotate(w[i-15],7)^rotate(w[i-15],18)^(w[i-15]>>3);
      const auto b=rotate(w[i-2],17)^rotate(w[i-2],19)^(w[i-2]>>10);
      w[i]=w[i-16]+a+w[i-7]+b;
    }
    auto a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
    for(int i=0;i<64;++i) {
      const auto s1=rotate(e,6)^rotate(e,11)^rotate(e,25);
      const auto ch=(e&f)^(~e&g);
      const auto t1=h+s1+ch+K[i]+w[i];
      const auto s0=rotate(a,2)^rotate(a,13)^rotate(a,22);
      const auto maj=(a&b)^(a&c)^(b&c);
      const auto t2=s0+maj;
      h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    h_[0]+=a;h_[1]+=b;h_[2]+=c;h_[3]+=d;
    h_[4]+=e;h_[5]+=f;h_[6]+=g;h_[7]+=h;
  }
  std::array<std::uint32_t,8> h_;
  std::array<std::uint8_t,64> chunk_{};
  std::size_t used_=0;
  std::uint64_t bits_=0;
};
} // namespace riso_detail

class RisoReader {
 public:
  static constexpr std::size_t kBlock = 65536;
  explicit RisoReader(const std::string& path) : file_(path,std::ios::binary) {
    if (!file_)throw std::runtime_error("Cannot open compact ISO container");
    file_.seekg(0,std::ios::end);
    const auto end=file_.tellg();
    if (end < 0)throw std::runtime_error("Cannot determine RISO length");
    physical_size_=static_cast<std::uint64_t>(end);
    seek(0);parse();
  }
  RisoReader(const RisoReader&)=delete;
  RisoReader& operator=(const RisoReader&)=delete;

  std::uint64_t size() const noexcept { return logical_size_; }
  std::uint64_t compressed_size() const noexcept { return physical_size_; }
  std::size_t blocks() const noexcept { return blocks_.size(); }

  bool read(std::uint64_t offset,std::uint8_t* destination,std::size_t length) {
    if ((length && !destination) || offset > logical_size_ || length > logical_size_-offset)
      return false;
    std::lock_guard<std::mutex> guard(mutex_);
    std::size_t copied=0;
    while (copied < length) {
      const auto index=static_cast<std::size_t>(offset/kBlock);
      if (index>=blocks_.size())throw std::runtime_error("RISO block index invalid");
      if (cached_block_!=index) {
        cache_=decode(blocks_[index]);
        cached_block_=index;
      }
      const std::size_t inner=static_cast<std::size_t>(offset%kBlock);
      if (inner>=cache_.size())throw std::runtime_error("RISO offset crosses partial block");
      const auto portion=std::min(length-copied,cache_.size()-inner);
      std::memcpy(destination+copied,cache_.data()+inner,portion);
      copied+=portion;offset+=portion;
    }
    return true;
  }

  // Optional complete check. The APK verifies this after creating the archive;
  // the emulator can use lazy per-block verification during normal reads.
  bool verify_all() {
    std::lock_guard<std::mutex> guard(mutex_);
    riso_detail::Sha256 sha;
    for(std::size_t i=0;i<blocks_.size();++i){
      const auto data=decode(blocks_[i]);
      sha.update(data.data(),data.size());
    }
    return sha.finish()==full_digest_;
  }

 private:
  enum : std::uint8_t { Raw=1,Deflate=2,Ref=3,Zero=4,Pattern=5,End=127 };
  struct Unique {
    std::uint64_t at=0;
    std::uint32_t length=0,payload=0;
    std::uint8_t type=0;
    std::array<std::uint8_t,32> digest{};
  };
  struct Block {
    std::uint8_t tag=0;
    std::uint32_t length=0,id=0;
    std::array<std::uint8_t,64> pattern{};
    std::uint32_t pattern_size=0;
  };

  void seek(std::uint64_t where) {
    if (where>physical_size_ ||
        where>static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
      throw std::runtime_error("RISO file seek outside bounds");
    file_.clear();file_.seekg(static_cast<std::streamoff>(where),std::ios::beg);
    if (!file_)throw std::runtime_error("RISO file seek failed");
  }
  std::uint64_t where() {
    const auto p=file_.tellg();
    if (p < 0)throw std::runtime_error("RISO stream position invalid");
    return static_cast<std::uint64_t>(p);
  }
  std::uint8_t byte() {
    const int value=file_.get();
    if (value<0)throw std::runtime_error("RISO truncated record");
    return static_cast<std::uint8_t>(value);
  }
  void exact(std::uint8_t* target,std::size_t size) {
    if (size && (!target || size>physical_size_-where()))
      throw std::runtime_error("RISO record exceeds file size");
    if (!size)return;
    file_.read(reinterpret_cast<char*>(target),static_cast<std::streamsize>(size));
    if (file_.gcount()!=static_cast<std::streamsize>(size))
      throw std::runtime_error("RISO truncated data");
  }
  std::uint32_t u32() {
    std::uint32_t result=0;
    for(int i=0;i<4;++i)result=(result<<8)|byte();
    return result;
  }
  std::uint64_t u64() {
    std::uint64_t result=0;
    for(int i=0;i<8;++i)result=(result<<8)|byte();
    return result;
  }
  void parse() {
    static constexpr std::uint8_t magic[8]={'R','E','I','S','O','0','1','\n'};
    std::array<std::uint8_t,8> signature{};
    exact(signature.data(),signature.size());
    if (!std::equal(signature.begin(),signature.end(),magic))
      throw std::runtime_error("REISO01 signature invalid");
    if (u32()!=kBlock)throw std::runtime_error("Unsupported RISO block size/version");
    bool last_partial=false;
    for(;;){
      const auto tag=byte();
      if(tag==End){
        const auto original=u64();
        const auto encoded_blocks=u64();
        const auto unique_count=u32();
        exact(full_digest_.data(),full_digest_.size());
        if(encoded_blocks!=blocks_.size() || unique_count!=uniques_.size() ||
           original!=logical_size_ || where()!=physical_size_)
          throw std::runtime_error("RISO footer/length/count mismatch");
        break;
      }
      if(last_partial)throw std::runtime_error("RISO non-final short block");
      Block block;block.tag=tag;
      if(tag==Zero){
        block.length=u32();
      }else if(tag==Pattern){
        block.length=u32();block.pattern_size=u32();
        if(block.pattern_size<1 || block.pattern_size>64 ||
           block.pattern_size>block.length)
          throw std::runtime_error("RISO pattern metadata invalid");
        exact(block.pattern.data(),block.pattern_size);
      }else if(tag==Ref){
        block.id=u32();
        if(block.id>=uniques_.size())throw std::runtime_error("RISO forward/invalid reference");
        block.length=uniques_[block.id].length;
      }else if(tag==Raw || tag==Deflate){
        Unique u;u.type=tag;u.length=u32();u.payload=u32();
        if(u.length<1 || u.length>kBlock || u.payload<1 || u.payload>kBlock+512 ||
           (tag==Raw && u.length!=u.payload))
          throw std::runtime_error("RISO compressed block bounds invalid");
        exact(u.digest.data(),u.digest.size());
        u.at=where();
        if(u.payload>physical_size_-u.at)throw std::runtime_error("RISO truncated compressed block");
        seek(u.at+u.payload);
        if(uniques_.size()>=static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
          throw std::runtime_error("RISO unique table exceeds encoding");
        block.id=static_cast<std::uint32_t>(uniques_.size());
        block.length=u.length;uniques_.push_back(u);
      }else throw std::runtime_error("RISO unknown block type");
      if(block.length<1 || block.length>kBlock)
        throw std::runtime_error("RISO block exceeds 64 KiB or empty");
      if(logical_size_>std::numeric_limits<std::uint64_t>::max()-block.length)
        throw std::runtime_error("RISO original size overflow");
      logical_size_+=block.length;
      last_partial=block.length<kBlock;
      blocks_.push_back(block);
    }
    // Do not claim every byte is checked until verify_all() has run. 
  }
  std::vector<std::uint8_t> unique(std::uint32_t id) {
    if(id>=uniques_.size())throw std::runtime_error("RISO invalid unique ID");
    const Unique& u=uniques_[id];
    seek(u.at);
    std::vector<std::uint8_t> payload(u.payload);
    exact(payload.data(),payload.size());
    std::vector<std::uint8_t> decoded(u.length);
    if(u.type==Raw){
      if(payload.size()!=decoded.size())throw std::runtime_error("RISO raw size mismatch");
      decoded=std::move(payload);
    }else{
      uLongf decoded_size=static_cast<uLongf>(decoded.size());
      const int z=uncompress(decoded.data(),&decoded_size,
                             payload.data(),static_cast<uLong>(payload.size()));
      if(z!=Z_OK || decoded_size!=decoded.size())
        throw std::runtime_error("RISO zlib decompression failed or wrong size");
    }
    if(riso_detail::Sha256::digest(decoded)!=u.digest)
      throw std::runtime_error("RISO SHA256 block mismatch");
    return decoded;
  }
  std::vector<std::uint8_t> decode(const Block& block) {
    if(block.tag==Zero)return std::vector<std::uint8_t>(block.length,0);
    if(block.tag==Pattern) {
      std::vector<std::uint8_t> answer(block.length);
      for(std::size_t i=0;i<answer.size();++i)
        answer[i]=block.pattern[i%block.pattern_size];
      return answer;
    }
    return unique(block.id);
  }

  std::ifstream file_;
  std::uint64_t physical_size_=0,logical_size_=0;
  std::vector<Block> blocks_;
  std::vector<Unique> uniques_;
  std::array<std::uint8_t,32> full_digest_{};
  std::vector<std::uint8_t> cache_;
  std::size_t cached_block_=std::numeric_limits<std::size_t>::max();
  std::mutex mutex_;
};
} // namespace reverse_windows
