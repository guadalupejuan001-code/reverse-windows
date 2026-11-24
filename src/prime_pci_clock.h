#pragma once
#include <stdint.h>

#define RW_PRIME_DDR5_CLOCK_HZ UINT64_C(4800000000)
#define RW_PRIME_DDR5_TRANSFERS_PER_SECOND (UINT64_C(2) * RW_PRIME_DDR5_CLOCK_HZ)
#define RW_PRIME_IDE_CLOCK_HZ UINT64_C(33333333)
#define RW_PRIME_GPU_CLOCK_HZ UINT64_C(4500000000000)

// Vendor PCI configuration registers on the Prime RAM bridge, IDE and GPU.
// Reading the low byte of CYCLES latches that function's 64-bit sample.
// Both fields are read-only and little endian. Time between VM runs is excluded.
#define RW_PRIME_PCI_CLOCK_HZ 0x40u
#define RW_PRIME_PCI_CLOCK_CYCLES 0x48u
