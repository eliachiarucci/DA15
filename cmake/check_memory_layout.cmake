# Post-build guard: fail the build if the linker script lost the 16-byte
# DFU-magic reservation at the top of RAM (_estack must be 0x20007FF0).
#
# This regresses silently if STM32CubeMX regenerates STM32H503xx_FLASH.ld:
# the firmware still links and runs, but the DFU magic word at 0x20007FF0
# ends up inside the stack and firmware-update entry breaks intermittently.
# Restore the linker script from git if this check fires (RAM = 32K - 16
# plus the .noinit section).

if(NOT NM OR NM STREQUAL "")
    set(NM arm-none-eabi-nm)
endif()

execute_process(
    COMMAND ${NM} ${ELF}
    OUTPUT_VARIABLE symbols
    RESULT_VARIABLE rc
)

if(NOT rc EQUAL 0)
    message(WARNING "check_memory_layout: could not run ${NM}; check skipped")
    return()
endif()

string(REGEX MATCH "([0-9a-fA-F]+)[ \t]+[A-Za-z][ \t]+_estack" _m "${symbols}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "check_memory_layout: _estack symbol not found in ${ELF}")
endif()

string(TOLOWER "${CMAKE_MATCH_1}" estack)
if(NOT estack MATCHES "20007ff0$")
    message(FATAL_ERROR
        "_estack is 0x${estack}, expected 0x20007ff0.\n"
        "The linker script lost the DFU-magic RAM reservation (RAM must be "
        "32K - 16). Did CubeMX regenerate STM32H503xx_FLASH.ld? Restore it "
        "from git (see CLAUDE.md, 'CubeMX Regeneration').")
endif()
