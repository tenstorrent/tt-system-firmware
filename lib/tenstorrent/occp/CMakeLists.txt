# Copyright (c) 2026 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_OCCP occp.c)
zephyr_library_sources_ifdef(CONFIG_OCCP_BACKEND_I3C occp_i3c.c)
zephyr_library_sources_ifdef(CONFIG_OCCP_BACKEND_FAKE occp_fake.c)
