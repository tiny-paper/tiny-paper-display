// Generated from apps/webapp/src/lib/reset.ts of the tiny-paper main repository (MIT): do not edit.
// Reset an ESP32 into its app over Web Serial (USB-Serial-JTAG on the S3/C3, or a USB-UART bridge with the
// classic DTR/RTS auto-reset circuit). Same pulse as esptool.py HardReset for USB-JTAG (RTS high 200 ms, low,
// DTR low throughout), with one step first: both lines low. On the USB-Serial-JTAG, going from DTR+RTS high
// (Chrome's state after port.open) straight to RTS-only high resets the chip into download mode
// (TRMNL X 2026-09-25: rst:0x15 boot:0x0 "waiting for download"). esptool-js 0.7.0 HardReset never raises RTS
// at all (it only sets it low), so its after('hard_reset') left the X in the flasher stub until a watchdog
// reset it ("reset=task_wdt crash=1" on the first boot of every flash).











/** Each step sets both lines in one request (one USB control transfer), then waits. */
export const RESET_STEPS                       = [
  { dtr: false, rts: false, ms: 100 },
  { dtr: false, rts: true, ms: 200 },
  { dtr: false, rts: false, ms: 200 },
];

const sleep = (ms        ) => new Promise      ((r) => setTimeout(r, ms));

export async function resetToApp(port            , wait                                = sleep)                {
  for (const s of RESET_STEPS) {
    await port.setSignals({ dataTerminalReady: s.dtr, requestToSend: s.rts });
    await wait(s.ms);
  }
}

/** The ROM/stub loader (esptool-js ESPLoader) as far as a reset needs it. */




/** RTC_CNTL registers (ESP-IDF 5.5 soc/<chip>/register/soc/rtc_cntl_reg.h, esptool targets/<chip>.py). */
const RTC_REGS                                                                                    = {
  esp32s3: { wdt0: 0x60008098, wdt1: 0x6000809c, wprotect: 0x600080b0, option1: 0x6000812c },
  esp32c3: { wdt0: 0x60008090, wdt1: 0x60008094, wprotect: 0x600080a8, option1: 0x600080f4 },
};
export const RTC_WDT_WKEY = 0x50d83aa1;
/** Enable, stage 0 = system reset (5), system reset length (1 << 8), stage 0 active (2): esptool watchdog_reset. */
export const RTC_WDT_RESET_CFG = ((1 << 31) | (5 << 28) | (1 << 8) | 2) >>> 0;

/**
 * Leave the loader for the app with an RTC watchdog reset (esptool `--after watchdog_reset`): it works on the
 * USB-Serial-JTAG where an RTS pulse from the loader does not (TRMNL X 2026-09-25). First clears
 * RTC_CNTL_FORCE_DOWNLOAD_BOOT (esptool ESP32S3ROM.hard_reset), so the reset boots from flash. `chip` as
 * firmware.ts normChip ("esp32s3"). False for a chip without the registers here: use resetToApp instead.
 */
export async function watchdogReset(l           , chip        , wait                                = sleep)                   {
  const r = RTC_REGS[chip];
  if (!r) return false;
  await l.writeReg(r.option1, 0, 0x1);
  await l.writeReg(r.wprotect, RTC_WDT_WKEY);
  await l.writeReg(r.wdt1, 2000); /* RTC slow clock cycles: ~15 ms */
  await l.writeReg(r.wdt0, RTC_WDT_RESET_CFG);
  try {
    await l.writeReg(r.wprotect, 0);
  } catch {
    /* the chip may already be resetting */
  }
  await wait(500);
  return true;
}
