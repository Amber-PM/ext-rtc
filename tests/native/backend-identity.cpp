/*
 *
 *  _____ __  _______   ____ _____ ____
 * | ____|\ \/ /_   _| |  _ \_   _/ ___|
 * |  _|   \  /  | |   | |_) || || |
 * | |___  /  \  | |   |  _ < | || |___
 * |_____|/_/\_\ |_|   |_| \_\|_| \____|
 *
 * Copyright (c) 2026 Amber-PM contributors
 * SPDX-License-Identifier: MIT
 *
 * https://github.com/Amber-PM/ext-rtc
 *
 */

#include "../../src/backend/backend_info.h"
#include <iostream>

int main(){
	if(!amber::rtc::backend_probe()){
		std::cerr << "incompatible loaded libdatachannel identity or message lifetime export\n";
		return 1;
	}
	std::cout << amber::rtc::backend_identity() << '\n';
	return 0;
}
