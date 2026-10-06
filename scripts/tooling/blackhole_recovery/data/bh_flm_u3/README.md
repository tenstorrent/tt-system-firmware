# Blackhole GLX2 Flash Loader Module

This directory builds the SPI EEPROM flash loader for the GLX2 DMC, which uses
an STM32U375 (STMU3) rather than the STM32G0 used by the existing `bh_flm`.

Build it with:

```sh
./build-flm.sh
```

The resulting `build/spi1_u3.flm` is used with pyOCD target
`stm32u375veix` and `pyocd_config_spi1.py`.