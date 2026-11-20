#pragma once
#include <stdint.h>

// Vendor PCI configuration registers on the Prime RAM bridge, IDE and GPU.
// Reading the low byte of CYCLES latches that function's 64-bit sample.
// Both fields are read-only and little endian. Time between VM runs is excluded.
#define RW_PRIME_COMPONENT_HZ UINT64_C(4500000000000)
#define RW_PRIME_PCI_CLOCK_HZ 0x40u
#define RW_PRIME_PCI_CLOCK_CYCLES 0x48u
