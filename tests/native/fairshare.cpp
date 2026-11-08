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

// cheap unit checks of the send admission rules, the saturation behaviour lives in backpressure.cpp

#include "../../src/peer/peer.h"
#include <iostream>
#include <vector>

using namespace amber::rtc;

namespace {
void check(bool value, const char* why){
	if(!value){ throw std::runtime_error(why); }
}
}

int main(){
	try{
		check(sendGeneral < sendBudget && sendBudget < bufferLimit, "receives need headroom above the send budget");
		check(send_footprint() == 0, "send footprint must start empty");
		check(send_cost(0) > 2 && send_cost(1000) >= 2000, "owned copy and backend escrow are both charged");
		std::vector<std::shared_ptr<Peer>> peers;
		for(size_t i = 0; i < 128; ++i){ peers.push_back(Peer::create({})); }
		// peers that never queued anything still count, shares only decide admission above the floor
		const size_t general = sendGeneral;
		const size_t big = send_cost(262144);
		check(big > general / (std::max)(peers.size(), sharePeers), "test message must exceed the share");
		// with the pool still empty, a queue above its share is refused for being above it and a queue inside its share is not
		const size_t share = general / (std::max)(peers.size(), sharePeers);
		check(!reserve_send(big, big), "a queue above its share was admitted while the pool was empty");
		check(send_footprint() == 0, "a refusal must not reserve anything");
		check(reserve_send(share / 4, share / 4) && send_footprint() == share / 4, "a queue inside its share was refused while the pool was empty");
		release_send(share / 4);
		// an empty queue may always start one oversized message, until the region of peers without proof is spent
		size_t bypassed = 0;
		while(reserve_send(0, big)){ ++bypassed; check(bypassed < 1000, "bypass is unbounded"); }
		check(bypassed >= 1 && send_footprint() <= general - provenHeadroom, "peers without proof escaped their region of the general pool");
		// peers that proved a drain get the last part, and only them
		check(!reserve_send(0, big, false) && reserve_send(0, big, true), "the reserve is not kept for proven peers");
		size_t proven = 1;
		while(reserve_send(0, big, true)){ ++proven; check(proven < 1000, "proven bypass is unbounded"); }
		check(send_footprint() <= general && send_footprint() > general - provenHeadroom, "proven peers did not use the reserve");
		check(!reserve_send(big, big, true), "a proven queue above its share was admitted");
		// the reserve still serves peers that stay inside the floor
		size_t floors = 0;
		while(reserve_send(0, sendFloor)){ ++floors; }
		check(floors >= 16, "the reserve did not serve small first messages");
		check(send_footprint() <= sendBudget, "send footprint exceeded the hard cap");
		check(reserve_send(0, sendBudget - send_footprint()) && send_footprint() == sendBudget && !reserve_send(0, 1), "hard cap is not enforced");
		release_send(send_footprint());
		check(send_footprint() == 0, "release left a footprint behind");
		// a message that never fits is only admitted alone
		check(reserve_send(0, sendBudget + 1) && !reserve_send(0, 1) && !reserve_send(0, sendBudget + 1), "oversize message rules");
		release_send(sendBudget + 1);
		check(send_footprint() == 0, "oversize release");
		for(auto& peer : peers){ peer->close(); }
		check(active_peers() == 0, "peers leaked");
		std::cout << "PASS send admission keeps a hard cap, a bounded bypass and a reserve for small queues\n";
	} catch(const std::exception& e){
		std::cerr << e.what() << '\n';
		return 1;
	}
}
