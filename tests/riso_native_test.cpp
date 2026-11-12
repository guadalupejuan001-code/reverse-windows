#include "riso_reader.hpp"
#include "pc_machine.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(bool condition,const char* why) {
  if(!condition)throw std::runtime_error(why);
}
int main(int argc,char** argv){
  try {
    if(argc!=3)throw std::invalid_argument("Usage: riso_native_test input.iso input.riso");
    const std::string raw_path=argv[1],riso_path=argv[2];
    std::ifstream original(raw_path,std::ios::binary);
    if(!original)throw std::runtime_error("Missing original cross-language fixture");
    original.seekg(0,std::ios::end);
    const auto original_len=static_cast<std::uint64_t>(original.tellg());
    original.seekg(0);
    reverse_windows::RisoReader reader(riso_path);
    check(reader.size()==original_len,"RISO logical bytes mismatch");
    check(reader.compressed_size()<original_len,"Fixture should be compressible");

    const auto abc=std::vector<std::uint8_t>{'a','b','c'};
    const auto digest=reverse_windows::riso_detail::Sha256::digest(abc);
    const std::array<std::uint8_t,32> expected={
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,
      0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
      0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    check(digest==expected,"Native SHA256 must match NIST known vector");

    std::vector<std::uint8_t> old(7913),fresh(7913);
    std::uint64_t offset=0;
    while(offset<original_len){
      const auto n=static_cast<std::size_t>(
          std::min<std::uint64_t>(old.size(),original_len-offset));
      original.read(reinterpret_cast<char*>(old.data()),static_cast<std::streamsize>(n));
      check(original.gcount()==static_cast<std::streamsize>(n),"Fixture read short");
      check(reader.read(offset,fresh.data(),n),"RISO random byte reader failed");
      check(std::equal(old.begin(),old.begin()+n,fresh.begin()),
            "ISO reconstructed bytes differ at an actual offset");
      offset+=n;
    }
    check(!reader.read(reader.size(),fresh.data(),1),"RISO out-of-bounds accepted");
    check(reader.verify_all(),"RISO entire digest did not match Java codec");

    cosmic_pc::PcMachine pc(2048ULL*1024*1024,256ULL*1024*1024);
    pc.mount([&reader](std::uint64_t offset,std::uint8_t* buffer,std::size_t count){
      return reader.read(offset,buffer,count);
    },reader.size(),"iso");
    check(pc.boot_kind=="iso","RISO failed to expose El Torito ISO metadata");
    pc.run(40);
    check(!pc.cpu.fault && pc.cpu.halted,"Original x86 CPU failed boot from compacted ISO");
    check(pc.cpu.read8(0x9100)==0x34 && pc.cpu.read8(0x9101)==0x12,
          "Guest x86 code did not execute original ISO boot sector");
    std::cout<<"REVERSE_WINDOWS_RISO_NATIVE_BOOT_PASS blocks="<<reader.blocks()
             <<" original_bytes="<<reader.size()
             <<" compressed_bytes="<<reader.compressed_size()
             <<" real_x86_steps="<<pc.cpu.instructions<<"\n";
    return 0;
  }catch(const std::exception& e){
    std::cerr<<"REVERSE_WINDOWS_RISO_NATIVE_FAIL "<<e.what()<<"\n";
    return 1;
  }
}
