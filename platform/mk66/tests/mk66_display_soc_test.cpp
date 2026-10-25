// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MK66 register bits the display's hardware half writes (display/soc_facts.h) against a table of values, each cited from the
// two fact sources this tree reads: the Teensy core header (the MK66 / KINETISK branch) and
// NXP's MK66F18 device header (`*_MASK`). The image's hardware half cannot run on the host,
// so a wrong bit position (the eDMA clock gate was once written at the Kinetis-L position, bit 8) would be a DMA that never
// comes up: this table is what stops it. Runs on the host.
#include <doctest/doctest.h>

#include <o3sdk/platform/mk66/display/soc_facts.h>

#include <cstddef>
#include <cstdint>

namespace soc = o3sdk::platform::mk66::display::soc;

namespace {

struct Fact {
  const char* name;
  std::uint32_t ours;
  std::uint32_t expected;
  const char* cited;  // where the expected value is read
};

}  // namespace

TEST_CASE("display soc: every SIM clock gate and register bit is the value the fact tables give") {
  const Fact facts[] = {
      {"SIM_SCGC3 SPI2", soc::kScgc3Spi2, 0x00001000u, "kinetis.h, MK66F18.h SIM_SCGC3_SPI2_MASK"},
      {"SIM_SCGC6 DMAMUX", soc::kScgc6Dmamux, 0x00000002u, "kinetis.h, MK66F18.h SIM_SCGC6_DMAMUX_MASK"},
      {"SIM_SCGC7 DMA", soc::kScgc7Dma, 0x00000002u, "kinetis.h (KINETISK; 0x100 is KINETISL), MK66F18.h SIM_SCGC7_DMA_MASK"},
      {"SPI_MCR MSTR", soc::kMcrMstr, 0x80000000u, "MK66F18.h SPI_MCR_MSTR_MASK"},
      {"SPI_MCR MDIS", soc::kMcrMdis, 0x00004000u, "MK66F18.h SPI_MCR_MDIS_MASK"},
      {"SPI_MCR CLR_TXF", soc::kMcrClrTxf, 0x00000800u, "MK66F18.h SPI_MCR_CLR_TXF_MASK"},
      {"SPI_MCR CLR_RXF", soc::kMcrClrRxf, 0x00000400u, "MK66F18.h SPI_MCR_CLR_RXF_MASK"},
      {"SPI_MCR HALT", soc::kMcrHalt, 0x00000001u, "MK66F18.h SPI_MCR_HALT_MASK"},
      {"SPI_MCR PCSIS (all)", soc::kMcrPcsisAll, 0x003F0000u, "MK66F18.h SPI_MCR_PCSIS_MASK"},
      {"SPI_SR TCF", soc::kSrTcf, 0x80000000u, "MK66F18.h SPI_SR_TCF_MASK"},
      {"SPI_SR TXRXS", soc::kSrTxrxs, 0x40000000u, "MK66F18.h SPI_SR_TXRXS_MASK"},
      {"SPI_SR EOQF", soc::kSrEoqf, 0x10000000u, "MK66F18.h SPI_SR_EOQF_MASK"},
      {"SPI_SR TFUF", soc::kSrTfuf, 0x08000000u, "MK66F18.h SPI_SR_TFUF_MASK"},
      {"SPI_SR TFFF", soc::kSrTfff, 0x02000000u, "MK66F18.h SPI_SR_TFFF_MASK"},
      {"SPI_SR RFOF", soc::kSrRfof, 0x00080000u, "MK66F18.h SPI_SR_RFOF_MASK"},
      {"SPI_SR RFDF", soc::kSrRfdf, 0x00020000u, "MK66F18.h SPI_SR_RFDF_MASK"},
      {"SPI_SR RXCTR (mask)", soc::kSrRxctrMask, 0x000000F0u, "MK66F18.h SPI_SR_RXCTR_MASK"},
  };
  for (const Fact& f : facts) {
    CHECK_MESSAGE(f.ours == f.expected, f.name << ": " << f.cited);
  }
}

TEST_CASE("display soc: the eDMA clock gate is not at a neighbour's bit") {
  // SIM_SCGC7 on the MK66: FLEXBUS 1<<0, DMA 1<<1, MPU 1<<2, SDRAMC 1<<3 (kinetis.h). The DMA gate is exactly bit 1,
  // so the SDRAM controller's and FlexBus's gates (which the image's boot code sets) are different bits, and bit 8 (the
  // Kinetis-L position the first version wrote) is not it.
  CHECK(soc::kScgc7Dma == (1u << 1));
  CHECK(soc::kScgc7Dma != 0x1u);  // FLEXBUS
  CHECK(soc::kScgc7Dma != 0x4u);  // MPU
  CHECK(soc::kScgc7Dma != 0x8u);  // SDRAMC
  CHECK(soc::kScgc7Dma != 0x100u);
  CHECK(soc::kScgc7Dma < 0x10u);
  CHECK((soc::kScgc7Dma & (0x1u | 0x4u | 0x8u)) == 0u);
}

TEST_CASE("display soc: a flush never clears the transmit FIFO fill request, and clears everything else that could be stale") {
  CHECK((soc::kSrFlushClear & soc::kSrTfff) == 0u);
  CHECK((soc::kSrFlushClear & soc::kSrTcf) != 0u);
  CHECK((soc::kSrFlushClear & soc::kSrEoqf) != 0u);  // an end-of-queue left by a garbled frame stops the module until cleared
  CHECK((soc::kSrFlushClear & soc::kSrTfuf) != 0u);
  CHECK((soc::kSrFlushClear & soc::kSrRfof) != 0u);
  CHECK((soc::kSrFlushClear & soc::kSrRfdf) != 0u);
  // status bits that are not flags (the FIFO counters, the running state) are never written
  CHECK((soc::kSrFlushClear & soc::kSrRxctrMask) == 0u);
  CHECK((soc::kSrFlushClear & soc::kSrTxrxs) == 0u);
}
