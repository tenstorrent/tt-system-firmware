# 19.18.0

## Migration Guide

> This is a working draft for the up-coming 19.18.0 release.

This document lists recommended and required changes for those migrating from the previous v19.17.0 firmware release to the new 19.18.0 firmware release.

### Blackhole: PCIe DMA messages retired

`TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER` (0x9B) and `TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER` (0x9C) now reply "not recognized" (0xFF), and `struct pcie_dma_transfer_rqst` is removed from `msgqueue.h`. HDMA channel 0 belongs to the host driver (tt-kmd). Host code that needs DMA should use tt-kmd, which checks bit `hdma_ch0_host_owned` of `TAG_FW_CAPABILITIES_0`.
