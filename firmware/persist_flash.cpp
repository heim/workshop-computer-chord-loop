// persist_flash.cpp - flash-backed storage of the settings blob.
//
// Firmware-only (not compiled into host tests). Uses the SDK's
// flash_safe_execute(), which parks the other core and disables interrupts for
// the erase/program so the audio ISR on core0 never runs code from flash while
// XIP is off. core0 must have called flash_safe_execute_core_init() at startup
// (see main.cpp).

#include "persist.h"

#include <cstring>
#include "pico/flash.h"
#include "hardware/flash.h"

namespace chordloop {

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024) // 2 MB fallback
#endif

// Last 4 KB sector of the program-card flash.
static constexpr uint32_t kFlashTargetOffset = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;

// Program length must be a multiple of FLASH_PAGE_SIZE (256). Pad the blob up.
static constexpr uint32_t kProgramLen =
    ((kPersistBlobSize + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

// Runs with the other core parked + IRQs disabled: RAM + flash ROM routines only.
static void __not_in_flash_func(doFlashWrite)(void *param) {
    const uint8_t *buf = static_cast<const uint8_t *>(param);
    flash_range_erase(kFlashTargetOffset, FLASH_SECTOR_SIZE);
    flash_range_program(kFlashTargetOffset, buf, kProgramLen);
}

bool savePersistToFlash(const PersistState &st) {
    static uint8_t staging[kProgramLen];
    memset(staging, 0xFF, sizeof(staging));
    if (serializePersist(st, staging, sizeof(staging)) == 0) return false;

    int rc = flash_safe_execute(doFlashWrite, staging, 1000);
    return rc == PICO_OK;
}

bool loadPersistFromFlash(PersistState &st) {
    const uint8_t *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + kFlashTargetOffset);
    return deserializePersist(flash, kPersistBlobSize, st);
}

} // namespace chordloop
