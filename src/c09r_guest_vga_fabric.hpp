#pragma once
// C09R: use the user's GPU-VIRTUAL and qSSD graphics StateBus with one
// canonical Mundo X PcMachine. This is an exact VGA HOST PRESENTATION delta,
// not a fictitious Windows GPU, WDDM driver, or second guest machine.
#include "pc_machine.hpp"
#include "trabajo_fisico.hpp"
#include "qssd/graphics_work_state.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace reverse_windows {
class GuestVgaFabricC09R final {
 public:
  struct Cell {
    std::uint16_t index;
    std::uint8_t character,attribute;
  };
  struct Frame {
    std::uint64_t revision=0,qssd_serial=0,changed_cells=0;
    std::uint64_t full_frame_bytes=4000,transferred_bytes=0;
    std::uint64_t gpu_planned_cells=0,reused_objects=0;
    bool full_snapshot=false,exact_mirror=false;
    std::vector<Cell> updates;
    std::array<std::uint8_t,4000> complete{};
  };

 private:
  cosmic_pc::PcMachine& machine_;
  xgpu::PhysicalWorkAuthority gpu_;
  qssd::GraphicsWorkMemory<xgpu::PhysicalWorkPlan> work_{4};
  std::array<std::uint8_t,4000> previous_{},mirror_{};
  bool initialized_=false;
  std::uint64_t revision_=0;

 public:
  explicit GuestVgaFabricC09R(cosmic_pc::PcMachine& machine)
      :machine_(machine){}
  Frame capture() {
    Frame result;
    machine_.copy_vga_text(result.complete.data(),result.complete.size());
    std::vector<xgpu::PhysicalCandidate> candidates;
    candidates.reserve(2000);
    for(unsigned cell=0;cell<2000;++cell) {
      const auto at=cell*2;
      const auto changed=!initialized_ ||
          previous_[at]!=result.complete[at] ||
          previous_[at+1]!=result.complete[at+1];
      if(!changed)continue;
      xgpu::PhysicalCandidate candidate;
      candidate.id=cell;
      candidate.transform_revision=1; // fixed exact grid location
      candidate.material_revision=(std::uint64_t(result.complete[at])<<8)|
                                    result.complete[at+1];
      candidate.mesh=0;
      candidate.camera_visible=true; // erased glyphs also need redraw
      candidate.camera_diameter_px=1;
      candidates.push_back(candidate);
    }
    // GPU-VIRTUAL, not a duplicate visibility planner, authorizes which
    // exact guest VGA cells are transported for actual host presentation.
    auto plan=gpu_.plan(candidates,{6},++revision_,true,true,true,0.5f,0.5f);
    result.revision=revision_;
    result.gpu_planned_cells=plan.camera_indices.size();
    result.reused_objects=plan.reused_objects;
    result.changed_cells=candidates.size();
    if(plan.camera_indices.size()!=candidates.size())
      throw std::runtime_error("GPU-VIRTUAL rejected changed guest VGA cells");
    for(auto idx:plan.camera_indices){
      if(idx>=candidates.size())throw std::runtime_error("GPU plan index outside VGA");
      const auto cell=candidates[idx].id;
      if(cell>=2000)throw std::runtime_error("GPU plan cell outside VGA");
      const auto at=std::size_t(cell)*2;
      result.updates.push_back(Cell{std::uint16_t(cell),
                        result.complete[at],result.complete[at+1]});
    }
    // 2-byte index + 2-byte exact VGA char/attribute per changed cell.
    // A full 4KB snapshot is cheaper for initial/large updates.
    result.full_snapshot=!initialized_||result.updates.size()*4>=4000;
    if(result.full_snapshot){
      result.transferred_bytes=4000;
      mirror_=result.complete;
    }else{
      result.transferred_bytes=result.updates.size()*4;
      for(const auto& cell:result.updates){
        mirror_[cell.index*2]=cell.character;
        mirror_[cell.index*2+1]=cell.attribute;
      }
    }
    result.exact_mirror=mirror_==result.complete;
    if(!result.exact_mirror)
      throw std::runtime_error("GPU-VIRTUAL emitted a non-equivalent guest framebuffer delta");
    previous_=result.complete;
    initialized_=true;
    // Publish completed *exact* real VGA plan in original qSSD StateBus.
    const auto completed=plan.revision;
    const auto view=work_.publish(std::move(plan),revision_,completed);
    result.qssd_serial=view->serial;
    return result;
  }
  auto state()const{return work_.snapshot();}
};
} // namespace reverse_windows
