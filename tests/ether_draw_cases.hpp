// Included by the PCI integration test to use its real x86 MOV and doorbell fixture.
class EtherDrawGuest {
 public:
  PcMachine pc{PcMachine::high_ram_address_space(4ULL << 30, true), 64 * MiB};
  PcMotherboardC06 board;
  unsigned ticket = 0;
  static constexpr unsigned output = 0x300000, source = 0x80000;

  explicit EtherDrawGuest(const std::filesystem::path& path)
      : board(prepare(pc), nullptr, {}, 0, path) {
    graphics_boot(pc);
    Guest32 init;
    init.cfg_write(0x10, moved_bar);
    init.cfg_write(4, 6);
    init.run(pc);
    require(pc.cpu.read32(moved_bar + RWGPU_REG_CAPS) & RWGPU_CAP_ETHER_MASK, "mask capability absent");
  }
  void target(unsigned w, unsigned h, unsigned clear_color) {
    pc.cpu.write32(moved_bar + RWGPU_REG_WIDTH, w);
    pc.cpu.write32(moved_bar + RWGPU_REG_HEIGHT, h);
    pc.cpu.write32(moved_bar + RWGPU_REG_CLEAR_RGBA, clear_color);
  }
  void issue(unsigned op, std::uint64_t from, std::uint64_t aux, unsigned bytes, unsigned expected = RWGPU_OK) {
    Guest32 guest;
    guest.direct_command(op, ++ticket, from, aux, bytes);
    guest.run(pc);
    const auto status = pc.cpu.read32(moved_bar + RWGPU_REG_STATUS);
    if (pc.cpu.read32(moved_bar + RWGPU_REG_COMPLETED) != ticket || status != expected)
      throw std::runtime_error("Ether draw ticket " + std::to_string(ticket) + " op " +
          std::to_string(op) + " status " + std::to_string(status) + " expected " + std::to_string(expected));
  }
  void stage(const std::vector<RwGpuVertex>& vertices) {
    const auto bytes = vertices.size() * sizeof(RwGpuVertex);
    pc.cpu.memory.write_block(source, reinterpret_cast<const std::uint8_t*>(vertices.data()), bytes);
    // Same BAR stores used by the Windows buffered PCI transport, including its bounds.
    for (std::size_t i = 0; i < bytes; i += 4)
      pc.cpu.write32(moved_bar + RWGPU_STAGE_OFFSET + i, pc.cpu.read32(source + i));
  }
  std::vector<std::uint8_t> pixels(unsigned bytes) {
    issue(RWGPU_READ_COLOR_RAM, 0, output, bytes);
    std::vector<std::uint8_t> image(bytes);
    pc.cpu.memory.read_block(output, image.data(), image.size());
    return image;
  }
  std::uint64_t counter(const char* field) const {
    const auto json = gpu_stats(*board.attached_gpu());
    const auto key = std::string("\"") + field + "\":";
    const auto at = json.find(key);
    if (at == std::string::npos) throw std::runtime_error("missing GPU counter " + key);
    return std::stoull(json.substr(at + key.size()));
  }
 private:
  static PcMachine& prepare(PcMachine& pc) {
    pc.boot_graphics_demo(width, height);
    pc.enable_boot_acceleration();
    return pc;
  }
};

std::vector<RwGpuVertex> layered_mesh() {
  std::vector<RwGpuVertex> mesh;
  for (unsigned group = 0; group < 8; ++group) {
    const float x = -.75f + .5f * (group % 4), y = -.5f + float(group / 4);
    for (unsigned layer = 0; layer < 8; ++layer) {
      const auto z = layer ? .75f : .25f;
      const unsigned color = layer ? green : red;
      mesh.insert(mesh.end(), {{x-.20f,y-.25f,z,1,color}, {x+.20f,y-.25f,z,1,color},
                               {x,y+.25f,z,1,color}});
    }
  }
  return mesh;
}

void verify_ether_draws(const std::filesystem::path& path) {
  EtherDrawGuest guest(path);
  auto& cpu = guest.pc.cpu;
  const auto mesh = layered_mesh();
  const unsigned bytes = mesh.size() * sizeof(RwGpuVertex);
  constexpr unsigned img_bytes = 64 * 32 * 4;
  constexpr std::uint64_t mask_offset = RWGPU_LOGICAL_BYTES - 8;
  const auto stage = moved_bar + RWGPU_STAGE_OFFSET;
  cpu.write32(stage, 0x01010101); cpu.write32(stage + 4, 0x01010101);
  guest.issue(RWGPU_QUANTUM_STORE, mask_offset, 0, 8);
  guest.stage(mesh);
  guest.target(64, 32, clear);
  guest.issue(RWGPU_DRAW, 0, 0, bytes);
  const auto all = guest.pixels(img_bytes);
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset, 0, bytes);
  require(guest.pixels(img_bytes) == all && guest.counter("ether_mask_rejected_triangles") == 56 &&
          guest.counter("backend_vertex_bytes") == (64 + 8) * 3 * sizeof(RwGpuVertex),
          "mask disturbed order/depth, or submitted omitted triangles");
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset, 0, bytes);
  require(guest.counter("replayed_frames") == 1 && guest.counter("submitted_triangles") == 0 &&
          guest.counter("render_and_readback_ns") == 0 &&
          gpu_stats(*guest.board.attached_gpu()).find("\"gpu_query_ns\":null") != std::string::npos,
          "exact replay left a stale backend receipt");
  guest.issue(RWGPU_QUANTUM_H, mask_offset * 8, 0, 0);
  guest.issue(RWGPU_QUANTUM_CX, mask_offset * 8, mask_offset * 8 + 1, 0);
  const auto epoch = guest.pc.quantum_session->epoch(), draws = guest.counter("draws");
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset, 0, bytes, RWGPU_QUANTUM_UNAVAILABLE);
  require(guest.pc.quantum_session->epoch() == epoch && guest.counter("draws") == draws &&
          guest.pixels(img_bytes) == all, "nondeterministic predicate measured or published a frame");
  guest.issue(RWGPU_QUANTUM_CX, mask_offset * 8, mask_offset * 8 + 1, 0);
  guest.issue(RWGPU_QUANTUM_H, mask_offset * 8, 0, 0);
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset, 0, bytes);
  require(guest.pixels(img_bytes) == all && guest.counter("replayed_frames") == 2,
          "inverse Bell gates failed to restore the selected image");
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset + 1, 0, bytes, RWGPU_INVALID);
  guest.issue(RWGPU_DRAW_ETHER_MASK, mask_offset, 0, bytes - 1, RWGPU_INVALID);

  // The last valid byte in the second window selects only triangle 2048.
  std::vector<RwGpuVertex> large;
  large.reserve(2049 * 3);
  for (unsigned i = 0; i < 2049; ++i) large.insert(large.end(), mesh.begin(), mesh.begin() + 3);
  constexpr std::uint64_t wide = 0x1000;
  cpu.write32(stage, 1);
  guest.issue(RWGPU_QUANTUM_STORE, wide + 256, 0, 1);
  guest.stage(large);
  guest.issue(RWGPU_DRAW_ETHER_MASK, wide, 0, large.size() * sizeof(RwGpuVertex));
  const auto one = guest.pixels(img_bytes);
  guest.issue(RWGPU_DRAW, 0, 0, 3 * sizeof(RwGpuVertex));
  require(guest.pixels(img_bytes) == one && guest.counter("ether_mask_rejected_triangles") == 3*56+2048,
          "multi-window mask selection failed");

  const auto initial = guest.counter("backend_frames");
  guest.target(32, 64, clear);
  guest.issue(RWGPU_DRAW, 0, 0, 3 * sizeof(RwGpuVertex));
  guest.target(32, 64, 0x11112233);
  guest.issue(RWGPU_DRAW, 0, 0, 3 * sizeof(RwGpuVertex));
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_COLOR_ALPHA, 0, 3 * sizeof(RwGpuVertex));
  auto changed = mesh;
  changed[0].rgba = blue;
  guest.stage(changed);
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_COLOR_ALPHA, 0, 3 * sizeof(RwGpuVertex));
  require(guest.counter("backend_frames") == initial + 4, "different dimensions, clear, alpha or vertices replayed");
  cpu.write32(moved_bar + RWGPU_REG_ENGINE_RESET, 1);
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_COLOR_ALPHA, 0, 3 * sizeof(RwGpuVertex));
  guest.board.set_gpu_cache_enabled(false);
  guest.board.set_gpu_cache_enabled(true);
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_COLOR_ALPHA, 0, 3 * sizeof(RwGpuVertex));
  require(guest.counter("backend_frames") == initial + 6, "reset or pressure retained the replay entry");
  guest.issue(RWGPU_DRAW_ETHER_MASK, RWGPU_LOGICAL_BYTES, 0, 0);
  const auto empty = guest.pixels(img_bytes);
  guest.issue(RWGPU_DRAW_ETHER_MASK, 0, 0, 3*sizeof(RwGpuVertex));
  require(empty == guest.pixels(img_bytes), "zero mask differs from an empty draw");

  // Both the one-ticket DMA command and the boot framebuffer must overwrite guest edits on a hit.
  guest.target(32, 64, clear);
  guest.issue(RWGPU_DRAW_RAM, guest.source, RWGPU_DRAW_COLOR_ALPHA, 3 * sizeof(RwGpuVertex));
  const auto dma_pixels = guest.pixels(img_bytes);
  cpu.write32(guest.output, ~cpu.read32(guest.output));
  const auto before = guest.counter("replayed_frames");
  guest.issue(RWGPU_DRAW_READBACK, guest.source, guest.output, 3 * sizeof(RwGpuVertex));
  std::vector<std::uint8_t> output(img_bytes);
  cpu.memory.read_block(guest.output, output.data(), output.size());
  require(output == dma_pixels && guest.counter("alpha_draw_ticket") == guest.ticket &&
          guest.counter("frame_dma_completions") == 1, "DMA replay lost data or completion");
  guest.target(width, height, clear);
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_PRESENT_VBE, 0, 0);
  const auto fb = guest.pc.video_info().physical_address;
  const auto pixel = cpu.read32(fb);
  cpu.write32(fb, ~pixel);
  guest.issue(RWGPU_DRAW, RWGPU_DRAW_PRESENT_VBE, 0, 0);
  require(cpu.read32(fb) == pixel && guest.counter("replayed_frames") == before + 2,
          "replay did not restore guest-written framebuffer");
  std::cout << "PRIME_ETHER_DRAW_PASS changes=4 reset=1 pressure=1 bell_inverse=1 multiwindow=257 dma=1 vbe=1\n";
}

void measure_ether_draws(const std::filesystem::path& path) {
  EtherDrawGuest guest(path);
  const auto mesh = layered_mesh();
  const unsigned geometry = mesh.size() * sizeof(RwGpuVertex);
  constexpr unsigned w = 128, h = 96, frames = 24, pixels = w*h*4;
  constexpr auto ether = PcMachine::kEtherMemoryPhysicalBase;
  guest.pc.cpu.write32(ether, 0x01010101);
  guest.pc.cpu.write32(ether + 4, 0x01010101);
  guest.stage(mesh);
  for (const bool dynamic : {false, true}) {
    std::array<std::vector<long long>, 2> times;
    std::vector<std::vector<std::uint8_t>> reference;
    for (unsigned trial = 0; trial < 7; ++trial) {
      for (unsigned order = 0; order < 2; ++order) {
        const unsigned enabled = (trial + order) % 2;
        guest.board.set_gpu_cache_enabled(false);
        guest.board.set_gpu_cache_enabled(enabled);
        guest.target(w, h, 0);
        guest.issue(RWGPU_DRAW, 0, 0, geometry); // Same warmed GLES context, outside the timed region.
        const auto renders = guest.counter("backend_frames"), uploads = guest.counter("backend_vertex_bytes");
        const auto readbacks = guest.counter("backend_readback_bytes"), replays = guest.counter("replayed_frames");
        long long ns = 0;
        for (unsigned frame = 0; frame < frames; ++frame) {
          guest.target(w, h, clear + (dynamic ? frame : 0));
          const auto t = std::chrono::steady_clock::now();
          guest.issue(dynamic && enabled ? RWGPU_DRAW_ETHER_MASK : RWGPU_DRAW, 0, 0, geometry);
          ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t).count();
          const auto image = guest.pixels(pixels);
          if (!enabled && trial == 0) reference.push_back(image);
          else require(image == reference.at(frame), "A/B output differed for the same frame");
        }
        times[enabled].push_back(ns);
        const unsigned expected = enabled && !dynamic ? 1 : frames;
        require(guest.counter("backend_frames") - renders == expected &&
                guest.counter("backend_readback_bytes") - readbacks == expected * pixels &&
                guest.counter("backend_vertex_bytes") - uploads == expected * (dynamic && enabled ? geometry/8 : geometry) &&
                guest.counter("replayed_frames") - replays == frames - expected, "A/B backend work differs from expected");
      }
    }
    for (auto& values : times) std::sort(values.begin(), values.end());
    std::cout << "PRIME_ETHER_DRAW_BENCH workload=" << (dynamic ? "changing_clear_mask" : "identical_replay")
              << " trials=7 frames=" << frames << " exact_pixels_per_trial=" << w*h*frames
              << " control_ns=" << times[0][3] << " optimized_ns=" << times[1][3]
              << " backend_frames=" << frames << "/" << (dynamic ? frames : 1)
              << " vertex_bytes=" << frames*geometry << "/" << (dynamic ? frames*geometry/8 : geometry)
              << " readback_bytes=" << frames*pixels << "/" << (dynamic ? frames*pixels : pixels) << '\n';
  }
  std::cout << "PRIME_ETHER_DRAW_RENDERER " << gpu_stats(*guest.board.attached_gpu()) << '\n';
}
