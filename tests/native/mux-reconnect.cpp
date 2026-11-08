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

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>
using namespace amber::rtc;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }
template<class F> void wait(F ready, const char* why){
	auto end = Clock::now() + 30s;
	while(!ready()){
		if(Clock::now() > end){ throw std::runtime_error(why); }
		std::this_thread::sleep_for(1ms);
	}
}
void gathered(const std::shared_ptr<Peer>& peer){
	wait([&]{ peer->snapshot(); std::lock_guard lock(peer->mutex); return peer->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gathering");
	peer->snapshot();
}
Options plain(){
	Options options;
	options.config.bindAddress = "127.0.0.1";
	return options;
}
Options mux(std::optional<std::string> bind, uint16_t port){
	Options options;
	options.config.bindAddress = std::move(bind);
	options.config.portRangeBegin = options.config.portRangeEnd = port;
	options.config.enableIceUdpMux = true;
	return options;
}
double ms(Clock::time_point from){ return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

// a player leaves and the next one joins at once, so the last mux peer's socket is still closing
struct Result{ std::vector<double> create; unsigned refused = 0; double worstOther = 0; };

Result cycle(std::optional<std::string> bind, uint16_t port, unsigned rounds, bool otherPort){
	Result result;
	// an unrelated mux port must not be delayed by this port closing
	std::shared_ptr<Peer> other;
	const uint16_t second = static_cast<uint16_t>(port + 1);
	if(otherPort){ other = Peer::create(mux(bind, second)); }
	for(unsigned round = 0; round < rounds; ++round){
		const auto started = Clock::now();
		std::shared_ptr<Peer> answer;
		try{ answer = Peer::create(mux(bind, port)); }
		catch(const Error& e){ if(e.code != 1){ throw; } ++result.refused; continue; }
		result.create.push_back(ms(started));
		auto offer = Peer::create(plain());
		auto local = offer->create_channel("live", {});
		gathered(offer);
		answer->remote(*offer->localDescription, true);
		gathered(answer);
		offer->remote(*answer->localDescription, false);
		wait([&]{ for(auto& c : answer->poll_channels()){ (void)c; } return local->is_open(); }, "channel open");
		std::thread probe;
		double probeMs = 0;
		if(otherPort){
			probe = std::thread([&]{
				std::this_thread::sleep_for(2ms);
				const auto t = Clock::now();
				try{ Peer::create(mux(bind, second))->close(); }catch(const Error&){}
				probeMs = ms(t);
			});
		}
		offer->close();
		answer->close();
		if(probe.joinable()){ probe.join(); result.worstOther = (std::max)(result.worstOther, probeMs); }
	}
	if(other){ other->close(); }
	return result;
}
void report(const char* name, Result& r){
	std::sort(r.create.begin(), r.create.end());
	const double p50 = r.create.empty() ? 0 : r.create[r.create.size() / 2];
	const double max = r.create.empty() ? 0 : r.create.back();
	std::cout << name << " creates=" << r.create.size() << " refused=" << r.refused << " p50=" << p50 << "ms max=" << max
			  << "ms otherPortCreateMax=" << r.worstOther << "ms\n";
}

// every player drops at once and the first one back must not be refused while sockets close
void mass(std::optional<std::string> bind, uint16_t port, unsigned pairs){
	std::vector<std::shared_ptr<Peer>> offers, answers;
	std::vector<std::shared_ptr<Channel>> locals;
	for(unsigned i = 0; i < pairs; ++i){
		auto offer = Peer::create(plain());
		auto answer = Peer::create(std::getenv("MASS_PLAIN") ? plain() : mux(bind, port));
		locals.push_back(offer->create_channel("live", {}));
		offers.push_back(offer);
		answers.push_back(answer);
	}
	for(unsigned i = 0; i < pairs; ++i){ gathered(offers[i]); answers[i]->remote(*offers[i]->localDescription, true); }
	for(unsigned i = 0; i < pairs; ++i){ gathered(answers[i]); offers[i]->remote(*answers[i]->localDescription, false); }
	const auto connecting = Clock::now();
	for(unsigned i = 0; i < pairs; ++i){
		try{ wait([&]{ for(auto& c : answers[i]->poll_channels()){ (void)c; } return locals[i]->is_open(); }, "mass channel open"); }
		catch(...){
			unsigned open = 0;
			for(unsigned j = 0; j < pairs; ++j){ open += locals[j]->is_open(); }
			std::lock_guard a(answers[i]->mutex), o(offers[i]->mutex);
			std::cerr << "pair " << i << " stuck: answer state=" << static_cast<int>(answers[i]->state) << " offer state=" << static_cast<int>(offers[i]->state)
					  << " offerFailure=" << (offers[i]->failure ? static_cast<int>(*offers[i]->failure) : -1) << " answerFailure=" << (answers[i]->failure ? static_cast<int>(*answers[i]->failure) : -1) << " gather=" << static_cast<int>(offers[i]->gathering) << "/" << static_cast<int>(answers[i]->gathering) << " open=" << open << "/" << pairs << " active=" << active_peers() << "\n";
			throw;
		}
	}
	std::cout << "mass pairs=" << pairs << " connected in " << ms(connecting) << "ms\n";
	const auto closing = Clock::now();
	for(auto& answer : answers){ answer->close(); }
	for(auto& offer : offers){ offer->close(); }
	const double closeMs = ms(closing);
	answers.clear(); offers.clear(); locals.clear();
	const auto started = Clock::now();
	unsigned refused = 0;
	std::shared_ptr<Peer> next;
	while(!next){
		try{ next = Peer::create(mux(bind, port)); }
		catch(const Error& e){ if(e.code != 1){ throw; } ++refused; std::this_thread::sleep_for(5ms); if(ms(started) > 5000){ break; } }
	}
	std::cout << "mass close=" << closeMs << "ms first-create-after=" << ms(started) << "ms refused=" << refused << (next ? " ok" : " NEVER") << "\n";
	check(next && refused == 0, "first reconnect after mass drop was refused");
	next->close();
}
int main(int argc, char** argv){
	try{
		uint16_t port = 0;
		for(uint16_t candidate = 33000; candidate < 33400 && !port; candidate += 4){
			try{ Peer::create(mux("127.0.0.1", candidate))->close(); port = candidate; }
			catch(const Error& e){ if(e.code != 1){ throw; } }
		}
		check(port != 0, "no free mux port");
		std::this_thread::sleep_for(1500ms);
		auto explicitBind = cycle("127.0.0.1", port, 40, true);
		report("explicit-bind", explicitBind);
		std::this_thread::sleep_for(1500ms);
		auto wildcard = cycle(std::nullopt, static_cast<uint16_t>(port + 2), 40, true);
		report("wildcard", wildcard);
		// reconnecting through the same port must never be refused or take close to a second
		check(explicitBind.refused == 0 && wildcard.refused == 0, "reconnect refused");
		check(explicitBind.create.back() < 1000 && wildcard.create.back() < 1000, "reconnect stalled");
		// a peer on another port is created in 0.15 to 0.30 ms while this port closes, a lock shared by all ports shows as hundreds of ms
		check(explicitBind.worstOther < 50 && wildcard.worstOther < 50, "closing a mux port delayed a peer on another port");
		if(argc > 1){ mass(std::nullopt, static_cast<uint16_t>(port + 3), static_cast<unsigned>(std::stoul(argv[1]))); }
		stop_dispatcher();
		std::cout << "PASS mux reconnect\n";
	}catch(const std::exception& e){ std::cerr << e.what() << '\n'; return 1; }
}
