"""Copy the session interface for an isolated observer without editing the product tree."""
from pathlib import Path
import sys

source = Path(sys.argv[1]) / "src/android_boot_session.hpp"
output = Path(sys.argv[2])
text = source.read_text()
anchor = "  std::uint64_t session_steps() const noexcept"
assert text.count(anchor) == 1
text = text.replace(anchor, "  cosmic_pc::Cpu& diagnostic_cpu() noexcept { return pc_.cpu; }\n"
                    "  unsigned diagnostic_restarts() const noexcept { return restarts_; }\n\n" + anchor, 1)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(text)
