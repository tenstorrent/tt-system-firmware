/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/bindesc.h>
#include <tenstorrent/tt_bindesc.h>
#include <zephyr/version.h>

static int tt_boot_banner(void)
{
	static const char *const logo =
#ifdef CONFIG_SHELL_VT100_COLORS
		"\e[38;5;99m"
#endif
		"         .:.                 .:\n"
		"      .:-----:..             :+++-.\n"
		"   .:------------:.          :++++++=:\n"
		" :------------------:..      :+++++++++\n"
		" :----------------------:.   :+++++++++\n"
		" :-------------------------:.:+++++++++\n"
		" :--------:  .:-----------:. :+++++++++\n"
		" :--------:     .:----:.     :+++++++++\n"
		" .:-------:         .        :++++++++-\n"
		"    .:----:                  :++++=:.\n"
		"        .::                  :+=:\n"
		"          .:.               ::\n"
		"          .===-:        .-===-\n"
		"          .=======:. :-======-\n"
		"          .==================-\n"
		"          .==================-\n"
		"           ==================:\n"
		"            :-==========-:.\n"
		"                .:====-.\n"
		"\n"
#ifdef CONFIG_SHELL_VT100_COLORS
		"\e[0"
#endif
		;

	printk(logo);
	printk("*** Booting " CONFIG_BOARD " with Zephyr OS " STRINGIFY(BUILD_VERSION) " ***\n");

#if defined(CONFIG_TT_BINDESC)
	printk("*** APP_GIT_VERSION %s ***\n", BINDESC_GET_STR(app_git_version));
#endif
	if (IS_ENABLED(CONFIG_TT_BOOT_BANNER_SDK_VERSION)) {
		printk("*** SDK_VERSION " ZEPHYR_SDK_VERSION " ***\n");
	}

	return 0;
}

SYS_INIT(tt_boot_banner, POST_KERNEL, 0);
