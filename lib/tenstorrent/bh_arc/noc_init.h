/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NOC_INIT_H_INCLUDED
#define NOC_INIT_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#define NO_BAD_GDDR UINT8_MAX

/** @brief NOC2AXI TLB index reserved for ECC accesses, on both NOC rings.
 *
 * Every user takes @ref NocEccTlbLock. ECC counter and ECC_STATUS reads go over
 * @ref NOC_ECC_RING; chip-wide ROUTER_CFG_0 walks use this index on each ring in turn.
 */
#define NOC_ECC_TLB  15
#define NOC_ECC_RING 0

/** @brief Guards @ref NOC_ECC_TLB.
 *
 * Shared by every ECC NOC access: telemetry, the shell ECC commands, and the
 * ROUTER_CFG_0 walks in noc_init. Those deliberately avoid TLB 0, which the rest
 * of noc_init and reset.c program with no lock. Defined here rather than in
 * noc_ecc.c so the recovery library can use it. Innermost lock: never take the
 * ECC state lock while holding this one.
 */
void NocEccTlbLock(void);
void NocEccTlbUnlock(void);

int32_t set_tensix_enable(bool enable);

int NocInit(void);
void NocInitSingleTile(uint8_t noc0_x, uint8_t noc0_y);

/** @brief True after @ref NocInit has turned header check on.
 *
 * Sample this and @ref NocTensixRoutersUp only while @ref NocEccStateLock is held.
 */
bool NocEccEnabled(void);

/** @brief Exclude an ECC remote access from fabric state changes.
 *
 * The shell runs reset handlers on its own thread while telemetry scans elsewhere.
 * Hold this across a whole ECC walk, and take it to publish @ref NocTensixRoutersUp.
 *
 * Also serializes the things a walk depends on staying put: NOC translation
 * reprogramming (@ref InitNocTranslation, @ref ClearNocTranslation) and tile clock
 * gating (@ref set_tensix_enable, @ref SetSingleTileClockGate). A walk samples
 * @ref IsNocTranslationEnabled once and checks the gate bit before each L1 read;
 * neither may change under it. Lock order: this lock, then the NOC ECC TLB lock.
 */
void NocEccStateLock(void);
void NocEccStateUnlock(void);

/** @brief Drop NOC header check and mark Tensix routers down.
 *
 * Leaves ARC translation off.
 *
 * The chip-wide walk goes through @ref NOC_ECC_TLB under @ref NocEccTlbLock, so it
 * is serialized against every other ECC TLB user and has no TLB 0 footprint.
 *
 * @return true if check was dropped and the caller must pair a
 *         @ref NocEccRestoreCheck. false if nothing was done: ECC is off, or the
 *         routers are already down from a chip-wide reset that has not been
 *         followed by REINIT_TENSIX. In that case the caller must not call
 *         @ref NocEccRestoreCheck, which would turn check on against unencoded
 *         routers. Branch on this rather than re-reading ECC state.
 */
bool NocEccQuiesceCheck(void);

/** @brief Turn header check back on and mark Tensix routers up.
 *
 * Caller has ARC translation off; this restores it before publishing routers up,
 * so a scan that starts afterwards addresses tiles in the coordinate system
 * @ref IsNocTranslationEnabled reports. No-op if ECC is off. Only call after a
 * @ref NocEccQuiesceCheck that returned true.
 */
void NocEccRestoreCheck(void);

/** @brief Drop NOC header check before a chip-wide Tensix reset.
 *
 * Call while the routers are still up. @ref NocInit turns check back on after
 * it has reprogrammed every router. Leaves ARC translation as it found it.
 */
void NocPrepareForTensixReset(void);

/** @brief False while a Tensix reset has left routers unencoded.
 *
 * @ref NocPrepareForTensixReset holds this through the following @ref NocInit.
 * @ref NocEccQuiesceCheck holds it until @ref NocEccRestoreCheck. Sample it
 * while @ref NocEccStateLock is held; a bare read does not cover the walk that follows.
 */
bool NocTensixRoutersUp(void);
void InitNocTranslation(unsigned int pcie_instance, uint16_t bad_tensix_cols, uint8_t bad_gddr,
			uint16_t skip_eth);
int InitNocTranslationFromHarvesting(void);
void ProgramNocTranslationSingleTile(uint8_t noc0_x, uint8_t noc0_y);
void ClearNocTranslation(void);
void DisableArcNocTranslation(void);
void EnableArcNocTranslation(void);
void RestoreArcNocTranslation(void);
bool IsNocTranslationEnabled(void);
void NocLogicalToPhysical(uint8_t logical_x, uint8_t logical_y, uint8_t *phys_x, uint8_t *phys_y);
void NocPhysicalToLogical(uint8_t phys_x, uint8_t phys_y, uint8_t *logical_x, uint8_t *logical_y);
void SetSingleTileClockGate(uint8_t noc0_x, uint8_t noc0_y, bool gate);

/* Returns NOC 0 coordinates of an enabled, unharvested tensix core.
 * It's guaranteed to be the same core until translation is enabled, disabled or modified.
 */
void GetEnabledTensix(uint8_t *x, uint8_t *y);

#endif
