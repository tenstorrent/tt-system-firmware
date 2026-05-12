set(BOARD_REVISIONS
    "p100a" "p150a" "p150b" "p150c" "p300a" "p300b" "p300c" "galaxy" "galaxy_2" "galaxy_revc" "orion_slt")
if(NOT BOARD_REVISION IN_LIST BOARD_REVISIONS)
message(FATAL_ERROR "${BOARD_REVISION} is not a valid revision for tt_blackhole. Accepted revisions: ${BOARD_REVISIONS}")
endif()
