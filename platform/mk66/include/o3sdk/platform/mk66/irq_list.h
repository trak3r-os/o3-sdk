// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The MK66's 100 device interrupts in vector-table order, once: the vector table, the weak default handlers and the
// `Irq` enumeration are all generated from this list, so a line cannot be in one and missing from another. Numbers and
// names are the device's (the IRQn_Type of NXP's MK66F18 device header, a fact table; vector 16 + n). A handler is `<name>_IRQHandler`, `extern "C"`: defining one
// in any file replaces the weak default, which records a crash with the interrupt's number (src/startup.cpp).
//
// "Reserved" lines exist in the table but have no peripheral behind them; they are named so that a spurious one shows up
// in the crash record by name.

#include <cstdint>

#define O3SDK_MK66_IRQ_LIST(X) \
    X(0, DMA0_DMA16) X(1, DMA1_DMA17) X(2, DMA2_DMA18) X(3, DMA3_DMA19) \
    X(4, DMA4_DMA20) X(5, DMA5_DMA21) X(6, DMA6_DMA22) X(7, DMA7_DMA23) \
    X(8, DMA8_DMA24) X(9, DMA9_DMA25) X(10, DMA10_DMA26) X(11, DMA11_DMA27) \
    X(12, DMA12_DMA28) X(13, DMA13_DMA29) X(14, DMA14_DMA30) X(15, DMA15_DMA31) \
    X(16, DMA_Error) X(17, MCM) X(18, FTFE) X(19, Read_Collision) \
    X(20, LVD_LVW) X(21, LLWU) X(22, WDOG_EWM) X(23, RNG) \
    X(24, I2C0) X(25, I2C1) X(26, SPI0) X(27, SPI1) \
    X(28, I2S0_Tx) X(29, I2S0_Rx) X(30, Reserved46) X(31, UART0_RX_TX) \
    X(32, UART0_ERR) X(33, UART1_RX_TX) X(34, UART1_ERR) X(35, UART2_RX_TX) \
    X(36, UART2_ERR) X(37, UART3_RX_TX) X(38, UART3_ERR) X(39, ADC0) \
    X(40, CMP0) X(41, CMP1) X(42, FTM0) X(43, FTM1) \
    X(44, FTM2) X(45, CMT) X(46, RTC) X(47, RTC_Seconds) \
    X(48, PIT0) X(49, PIT1) X(50, PIT2) X(51, PIT3) \
    X(52, PDB0) X(53, USB0) X(54, USBDCD) X(55, Reserved71) \
    X(56, DAC0) X(57, MCG) X(58, LPTMR0) X(59, PORTA) \
    X(60, PORTB) X(61, PORTC) X(62, PORTD) X(63, PORTE) \
    X(64, SWI) X(65, SPI2) X(66, UART4_RX_TX) X(67, UART4_ERR) \
    X(68, Reserved84) X(69, Reserved85) X(70, CMP2) X(71, FTM3) \
    X(72, DAC1) X(73, ADC1) X(74, I2C2) X(75, CAN0_ORed_Message_buffer) \
    X(76, CAN0_Bus_Off) X(77, CAN0_Error) X(78, CAN0_Tx_Warning) X(79, CAN0_Rx_Warning) \
    X(80, CAN0_Wake_Up) X(81, SDHC) X(82, ENET_1588_Timer) X(83, ENET_Transmit) \
    X(84, ENET_Receive) X(85, ENET_Error) X(86, LPUART0) X(87, TSI0) \
    X(88, TPM1) X(89, TPM2) X(90, USBHSDCD) X(91, I2C3) \
    X(92, CMP3) X(93, USBHS) X(94, CAN1_ORed_Message_buffer) X(95, CAN1_Bus_Off) \
    X(96, CAN1_Error) X(97, CAN1_Tx_Warning) X(98, CAN1_Rx_Warning) X(99, CAN1_Wake_Up)

namespace o3sdk::platform::mk66 {

inline constexpr unsigned kIrqCount = 100;

enum class Irq : std::uint8_t {
#define O3SDK_MK66_IRQ_ENUM(number, name) name = number,
  O3SDK_MK66_IRQ_LIST(O3SDK_MK66_IRQ_ENUM)
#undef O3SDK_MK66_IRQ_ENUM
};

namespace detail {
inline constexpr std::uint8_t kIrqNumbers[] = {
#define O3SDK_MK66_IRQ_NUMBER(number, name) number,
    O3SDK_MK66_IRQ_LIST(O3SDK_MK66_IRQ_NUMBER)
#undef O3SDK_MK66_IRQ_NUMBER
};
constexpr bool irq_numbers_sequential() noexcept {
  for (unsigned i = 0; i < sizeof(kIrqNumbers); ++i) {
    if (kIrqNumbers[i] != i) {
      return false;
    }
  }
  return true;
}
}  // namespace detail

static_assert(sizeof(detail::kIrqNumbers) == kIrqCount, "100 device interrupts");
static_assert(detail::irq_numbers_sequential(), "the list is in vector order, 0..99, none skipped or repeated");

}  // namespace o3sdk::platform::mk66
