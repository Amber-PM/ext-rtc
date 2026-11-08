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

// saturation test, real connected peers with frozen remotes (SIGSTOP) against a fast one
// usage: rtc-backpressure SCENARIO FROZEN LATE FAST IDLE SIZE SECONDS [report]
//   saturate: FROZEN frozen peers, FAST fast peers, IDLE connected peers that never send
//   grow: FROZEN frozen peers fill first, then LATE more frozen peers and FAST fast peers join
//   shrink: like saturate, then every frozen peer is closed and their send footprint must come back
// the same binary is the remote side when started with --remote COUNT

#include "../../src/channel/channel.h"
#include "../../src/core/dispatcher.h"
#include <rtc/rtc.hpp>
#include <atomic>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
using namespace amber::rtc;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {
std::string hex(const std::string& input){
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	for(unsigned char c : input){ result += digits[c >> 4]; result += digits[c & 15]; }
	return result;
}

std::string unhex(const std::string& input){
	auto nibble = [](char c){ return c <= '9' ? c - '0' : c - 'a' + 10; };
	std::string result;
	for(size_t i = 0; i + 1 < input.size(); i += 2){ result += static_cast<char>(nibble(input[i]) * 16 + nibble(input[i + 1])); }
	return result;
}

template<class F> void wait_for(F f, std::chrono::seconds limit, const char* why){
	const auto end = clk::now() + limit;
	while(!f()){
		if(clk::now() > end){ throw std::runtime_error(why); }
		std::this_thread::sleep_for(2ms);
	}
}

int remote_main(size_t count){
	int result = 0;
	try{
		rtc::Configuration config;
		config.disableAutoNegotiation = true;
		struct Remote{ std::shared_ptr<rtc::PeerConnection> pc; std::shared_ptr<rtc::DataChannel> channel; };
		std::vector<Remote> remotes(count);
		std::atomic<size_t> messages{0}, bytes{0}, closed{0};
		for(auto& r : remotes){
			r.pc = std::make_shared<rtc::PeerConnection>(config);
			r.channel = r.pc->createDataChannel("x");
			r.channel->onMessage([&](rtc::message_variant payload){
				std::visit([&](const auto& data){ ++messages; bytes += data.size(); }, payload);
			});
			r.channel->onClosed([&]{ ++closed; });
			r.pc->setLocalDescription(rtc::Description::Type::Offer);
		}
		for(auto& r : remotes){
			wait_for([&]{ return r.pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete; }, 60s, "gather");
		}
		for(size_t i = 0; i < count; ++i){
			std::cout << "OFFER " << i << ' ' << hex(std::string(*remotes[i].pc->localDescription())) << std::endl;
		}
		for(size_t i = 0; i < count; ++i){
			std::string line, tag;
			size_t index;
			std::getline(std::cin, line);
			std::istringstream in(line);
			std::string answer;
			if(!(in >> tag >> index >> answer) || tag != "ANSWER" || index >= count){ throw std::runtime_error("bad answer"); }
			remotes[index].pc->setRemoteDescription(rtc::Description(unhex(answer), rtc::Description::Type::Answer));
		}
		for(auto& r : remotes){ wait_for([&]{ return r.channel->isOpen(); }, 60s, "open"); }
		std::cout << "OPEN" << std::endl;
		std::string line;
		while(std::getline(std::cin, line)){
			std::istringstream in(line);
			std::string command;
			in >> command;
			if(command == "STATS"){
				std::cout << "STATS " << messages << ' ' << bytes << ' ' << closed << std::endl;
			} else if(command == "SEND"){
				size_t n, size;
				in >> n >> size;
				std::string payload(size, 'r');
				size_t failed = 0;
				for(auto& r : remotes){
					for(size_t i = 0; i < n; ++i){
						try{ r.channel->send(reinterpret_cast<const std::byte*>(payload.data()), payload.size()); } catch(const std::exception&){ ++failed; }
					}
				}
				std::cout << "SENT " << failed << std::endl;
			} else if(command == "QUIT"){
				break;
			}
		}
		for(auto& r : remotes){ r.pc->resetCallbacks(); r.pc->close(); }
	} catch(const std::exception& e){
		std::cerr << "remote failed: " << e.what() << std::endl;
		result = 1;
	}
	auto done = rtc::Cleanup();
	if(done.wait_for(10s) != std::future_status::ready){ std::_Exit(2); }
	return result;
}

struct Helper{
	pid_t pid = -1;
	int in = -1, out = -1;
	std::string pending;
	size_t count = 0;

	void write(const std::string& text){
		const char* p = text.data();
		size_t left = text.size();
		while(left){
			auto n = ::write(in, p, left);
			if(n <= 0){ throw std::runtime_error("helper pipe closed"); }
			p += n;
			left -= static_cast<size_t>(n);
		}
	}
	void retire(){
		if(pid < 0){ return; }
		kill(pid, SIGKILL);
		int status;
		waitpid(pid, &status, 0);
		pid = -1;
		close(in);
		close(out);
	}
	std::string line(std::chrono::seconds limit){
		const auto end = clk::now() + limit;
		for(;;){
			if(auto at = pending.find('\n'); at != std::string::npos){
				auto result = pending.substr(0, at);
				pending.erase(0, at + 1);
				return result;
			}
			pollfd pfd{out, POLLIN, 0};
			if(poll(&pfd, 1, 100) > 0){
				char buffer[65536];
				auto n = ::read(out, buffer, sizeof(buffer));
				if(n <= 0){ throw std::runtime_error("helper exited"); }
				pending.append(buffer, static_cast<size_t>(n));
			}
			if(clk::now() > end){ throw std::runtime_error("helper timeout"); }
		}
	}
};

Helper spawn_helper(const char* self, size_t count){
	int toChild[2], fromChild[2];
	if(pipe(toChild) || pipe(fromChild)){ throw std::runtime_error("pipe"); }
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, toChild[0], 0);
	posix_spawn_file_actions_adddup2(&actions, fromChild[1], 1);
	posix_spawn_file_actions_addclose(&actions, toChild[1]);
	posix_spawn_file_actions_addclose(&actions, fromChild[0]);
	const std::string number = std::to_string(count);
	char* argv[] = {const_cast<char*>(self), const_cast<char*>("--remote"), const_cast<char*>(number.c_str()), nullptr};
	Helper helper;
	if(posix_spawn(&helper.pid, self, &actions, nullptr, argv, environ)){ throw std::runtime_error("spawn"); }
	posix_spawn_file_actions_destroy(&actions);
	close(toChild[0]);
	close(fromChild[1]);
	helper.in = toChild[1];
	helper.out = fromChild[0];
	helper.count = count;
	return helper;
}

struct Member{
	std::shared_ptr<Peer> peer;
	std::shared_ptr<Channel> channel;
	size_t helper = 0;
	char role = 'f';
};

struct World{
	const char* self;
	std::vector<Helper> helpers;
	std::vector<Member> members;

	// connects count peers hosted by helpers of at most 25 remotes, role f fast, z frozen, i idle
	void join(size_t count, char role){
		for(size_t left = count; left;){
			const size_t chunk = (std::min)(left, size_t{25});
			// the pinned backend loses a handshake now and then even without ext-rtc, a failed chunk is built again
			for(int attempt = 0;; ++attempt){
				try{
					connect_chunk(chunk, role);
					break;
				} catch(const std::exception& error){
					if(attempt == 2){ throw; }
					std::cerr << "retrying chunk after: " << error.what() << std::endl;
				}
			}
			left -= chunk;
		}
	}
	void connect_chunk(size_t count, char role){
		const size_t h = helpers.size();
		helpers.push_back(spawn_helper(self, count));
		auto& helper = helpers[h];
		std::vector<std::shared_ptr<Peer>> peers(helper.count);
		std::vector<Member> chunk;
		try{
			for(size_t i = 0; i < helper.count; ++i){
				std::istringstream in(helper.line(60s));
				std::string tag, offer;
				size_t index;
				in >> tag >> index >> offer;
				peers[index] = Peer::create({});
				peers[index]->remote(unhex(offer), true);
			}
			for(size_t i = 0; i < helper.count; ++i){
				wait_for([&]{
					peers[i]->snapshot();
					std::lock_guard lock(peers[i]->mutex);
					return peers[i]->gathering == ::rtc::PeerConnection::GatheringState::Complete;
				}, 30s, "answer gathering");
				peers[i]->snapshot();
				helper.write("ANSWER " + std::to_string(i) + ' ' + hex(*peers[i]->localDescription) + "\n");
			}
			if(helper.line(60s) != "OPEN"){ throw std::runtime_error("helper did not open"); }
			for(auto& peer : peers){
				Member member;
				member.peer = peer;
				member.helper = h;
				member.role = role;
				wait_for([&]{
					for(auto c : peer->poll_channels()){ member.channel = c; }
					return member.channel && member.channel->is_open();
				}, 30s, "channel open");
				chunk.push_back(std::move(member));
			}
		} catch(...){
			for(auto& peer : peers){ if(peer){ peer->close_safely(); } }
			helper.retire();
			throw;
		}
		for(auto& member : chunk){ members.push_back(std::move(member)); }
	}
	void freeze(char role, int signal){
		for(size_t h = 0; h < helpers.size(); ++h){
			for(auto& m : members){
				if(m.helper == h && m.role == role){ kill(helpers[h].pid, signal); break; }
			}
		}
	}
	~World(){
		for(auto& m : members){ if(m.peer){ m.peer->close_safely(); } }
		for(auto& h : helpers){ h.retire(); }
	}
};

// share of its solo rate that a healthy peer must keep, see the calibration note in CMakeLists.txt
constexpr double progressShare = 0.25;

double rss_mib(){
	std::ifstream in("/proc/self/statm");
	size_t total = 0, resident = 0;
	in >> total >> resident;
	return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / 1048576.0;
}

struct Totals{
	size_t accepted = 0, refused = 0, budget = 0, dead = 0, bytes = 0, killed = 0;
};

struct Run{
	Totals fast, frozen, ref;
	double rssStart = 0, rssPeak = 0;
	size_t setupBudget = 0, maxManaged = 0, maxBudget = 0, maxFootprint = 0, footprintAfterClose = 0, received = 0, remoteMessages = 0, remoteFailed = 0;
};

// one round of sends for every member, fast ones also drain what the remote wrote
void pump(World& world, Run& run, const std::string& payload, const std::string& healthyPayload, bool frozenAlive){
	for(auto& m : world.members){
		if(m.role == 'i'){ continue; }
		if(m.role == 'z' && !frozenAlive){ continue; }
		auto& t = m.role == 'f' ? run.fast : m.role == 'r' ? run.ref : run.frozen;
		const std::string& frame = m.role == 'z' ? payload : healthyPayload;
		for(int j = 0; j < 4; ++j){
			try{
				if(m.channel->send(frame)){ ++t.accepted; t.bytes += frame.size(); } else{ ++t.refused; break; }
			} catch(const Error& e){
				if(e.code == 10){ ++t.budget; } else{ ++t.dead; }
				break;
			}
		}
		if(m.role == 'f' || m.role == 'r'){
			try{
				while(auto size = m.channel->read_size()){
					std::string sink(*size, '\0');
					if(!m.channel->copy_front(sink.data(), *size, true)){ break; }
					++run.received;
				}
			} catch(const Error&){ ++t.killed; }
		}
	}
	run.rssPeak = (std::max)(run.rssPeak, rss_mib());
	run.maxManaged = (std::max)(run.maxManaged, managed_buffer_bytes());
	run.maxBudget = (std::max)(run.maxBudget, budget_bytes());
	run.maxFootprint = (std::max)(run.maxFootprint, send_footprint());
}

size_t remote_bytes(Helper& helper){
	helper.write("STATS\n");
	std::istringstream in(helper.line(30s));
	std::string tag;
	size_t messages, bytes, closed;
	in >> tag >> messages >> bytes >> closed;
	return bytes;
}

void check(bool value, const std::string& why){
	if(!value){ throw std::runtime_error(why); }
}
}

int main(int argc, char** argv){
	if(argc == 3 && std::string(argv[1]) == "--remote"){ return remote_main(std::stoul(argv[2])); }
	try{
		const std::string scenario = argc > 1 ? argv[1] : "saturate";
		const size_t frozen = argc > 2 ? std::stoul(argv[2]) : 40;
		const size_t late = argc > 3 ? std::stoul(argv[3]) : 0;
		const size_t fast = argc > 4 ? std::stoul(argv[4]) : 4;
		const size_t idle = argc > 5 ? std::stoul(argv[5]) : 0;
		const size_t size = argc > 6 ? std::stoul(argv[6]) : 262144;
		const size_t seconds = argc > 7 ? std::stoul(argv[7]) : 6;
		const std::string mode = argc > 8 ? argv[8] : "";
		const size_t reference = argc > 9 ? std::stoul(argv[9]) : 0;
		const bool report = mode == "report";
		const size_t healthySize = argc > 10 ? std::stoul(argv[10]) : size;
		const std::string payload(size, 's'), healthyPayload(healthySize, 's');
		const size_t baseline = managed_buffer_bytes();
		Run run;
		std::vector<size_t> newcomerTimeline, establishedTimeline;
		double firstRate = 0, secondRate = 0, referenceRate = 0;
		size_t fastBytesDelivered = 0;
		{
			World world{argv[0], {}, {}};
			if(scenario == "grow"){
				world.join(frozen, 'z');
				world.freeze('z', SIGSTOP);
				const auto end = clk::now() + std::chrono::seconds(seconds / 2);
				while(clk::now() < end){ pump(world, run, payload, healthyPayload, true); std::this_thread::sleep_for(500us); }
				world.join(late, 'z');
				world.freeze('z', SIGSTOP);
				if(fast){ world.join(fast, 'f'); }
			} else{
				// reference peers alone give the rate that a healthy peer reaches on this machine
				if(reference){
					world.join(reference, 'r');
					const auto refStart = clk::now();
					while(clk::now() < refStart + 3s){ pump(world, run, payload, healthyPayload, true); std::this_thread::sleep_for(500us); }
					referenceRate = static_cast<double>(run.ref.accepted) / 3.0 / static_cast<double>(reference);
					run.ref = {};
				}
				world.join(frozen, 'z');
				if(fast){ world.join(fast, 'f'); }
				if(idle){ world.join(idle, 'i'); }
				world.freeze('z', SIGSTOP);
			}
			run.setupBudget = budget_bytes();
			run.rssStart = rss_mib();
			size_t fastHelper = 0;
			for(auto& m : world.members){ if(m.role == 'f'){ fastHelper = m.helper; break; } }
			const size_t before = fast ? remote_bytes(world.helpers[fastHelper]) : 0;
			const auto start = clk::now();
			const auto end = start + std::chrono::seconds(seconds);
			// fast remotes also write back so the receive side competes for the same budget
			auto nextWrite = start;
			const auto half = start + std::chrono::seconds(seconds) / 2;
			size_t halfBytes = 0;
			bool halved = false;
			size_t lastFast = 0, lastRef = 0;
			auto nextSecond = start + 1s;
			while(clk::now() < end){
				if(clk::now() >= nextSecond){
					newcomerTimeline.push_back(run.fast.accepted - lastFast);
					establishedTimeline.push_back(run.ref.accepted - lastRef);
					lastFast = run.fast.accepted;
					lastRef = run.ref.accepted;
					nextSecond += 1s;
				}
				pump(world, run, payload, healthyPayload, true);
				// the fast remotes keep writing so the receive side competes for the same budget
				if(fast && clk::now() >= nextWrite){
					world.helpers[fastHelper].write("SEND 2 16384\n");
					std::istringstream sent(world.helpers[fastHelper].line(30s));
					std::string tag;
					size_t failed = 0;
					sent >> tag >> failed;
					run.remoteMessages += 2 * fast - failed;
					run.remoteFailed += failed;
					nextWrite += 250ms;
				}
				if(!halved && clk::now() >= half){
					halved = true;
					halfBytes = fast ? remote_bytes(world.helpers[fastHelper]) - before : 0;
					if(scenario == "shrink"){
						for(auto& m : world.members){ if(m.role == 'z'){ m.peer->close_safely(); } }
					}
				}
				std::this_thread::sleep_for(500us);
			}
			run.footprintAfterClose = send_footprint();
			const double span = static_cast<double>(seconds) / 2;
			if(fast){
				fastBytesDelivered = remote_bytes(world.helpers[fastHelper]) - before;
				firstRate = static_cast<double>(halfBytes) / span / 1048576 / static_cast<double>(fast);
				secondRate = static_cast<double>(fastBytesDelivered - halfBytes) / span / 1048576 / static_cast<double>(fast);
			}
			for(auto& m : world.members){
				if(m.role == 'f' && (m.channel->is_closed() || m.channel->receiveError)){ ++run.fast.killed; }
				if(m.role == 'r' && (m.channel->is_closed() || m.channel->receiveError)){ ++run.ref.killed; }
			}
		}
		// budgets must drain once every peer is closed
		wait_for([&]{ return active_peers() == 0 && managed_buffer_bytes() == baseline && send_footprint() == 0; }, 20s, "managed budget or send footprint did not return to its baseline");
		std::cout << "scenario=" << scenario << " frozen=" << frozen << " late=" << late << " fast=" << fast << " idle=" << idle
			<< " size=" << size << " seconds=" << seconds << "\n"
			<< "fast accepted=" << run.fast.accepted << " refused=" << run.fast.refused << " budgetErr=" << run.fast.budget
			<< " dead=" << run.fast.dead << " killed=" << run.fast.killed << " delivered_mb=" << fastBytesDelivered / 1048576
			<< " mb_s_per_peer_first_half=" << firstRate << " second_half=" << secondRate << " received_from_remote=" << run.received << "/" << run.remoteMessages << " remote_send_failed=" << run.remoteFailed << "\n"
			<< "frozen accepted=" << run.frozen.accepted << " refused=" << run.frozen.refused << " budgetErr=" << run.frozen.budget
			<< " dead=" << run.frozen.dead << "\n"
			<< "setupBudgetMiB=" << run.setupBudget / 1048576.0 << " maxManagedMiB=" << run.maxManaged / 1048576.0 << " maxBudgetMiB=" << run.maxBudget / 1048576.0 << " maxSendFootprintMiB=" << run.maxFootprint / 1048576.0 << " limitMiB=" << bufferLimit / 1048576 << std::endl;
		// the frozen peers hold their memory until the backend gives up on them by itself, so the window ends well before that
		const size_t windowStart = 2, windowEnd = 8;
		auto windowRate = [&](const std::vector<size_t>& timeline, size_t peers, size_t from, size_t to){
			if(!peers || timeline.size() < to){ return 0.0; }
			size_t total = 0;
			for(size_t i = from; i < to; ++i){ total += timeline[i]; }
			return static_cast<double>(total) / static_cast<double>(to - from) / static_cast<double>(peers);
		};
		// large frames of newcomers wait until the backend declares the frozen peers dead, about 13 s, their memory comes back then
		const double lateFastRate = windowRate(newcomerTimeline, fast, 16, 19);
		const double tailFastRate = windowRate(newcomerTimeline, fast, windowStart, windowEnd), tailRefRate = windowRate(establishedTimeline, reference, windowStart, windowEnd);
		std::cout << "reference_msgs_s_per_peer=" << referenceRate << " window_newcomer_msgs_s_per_peer=" << tailFastRate
			<< " window_established_msgs_s_per_peer=" << tailRefRate << std::endl;
		std::cout << "rss_start_mib=" << run.rssStart << " rss_peak_mib=" << run.rssPeak << " managed_peak_mib=" << run.maxManaged / 1048576.0
			<< " backend_outside_budget_mib=" << run.rssPeak - run.rssStart - run.maxManaged / 1048576.0 << std::endl;
		std::cout << "newcomer msgs per second:";
		for(auto n : newcomerTimeline){ std::cout << ' ' << n; }
		std::cout << "\nestablished msgs per second:";
		for(auto n : establishedTimeline){ std::cout << ' ' << n; }
		std::cout << std::endl;
		// the backend keeps a 1 MiB send buffer per association outside the managed budget, a frozen peer fills it, plus per association state
		// measured 0.9 MiB of growth per frozen peer, the limit leaves 1.6 times that
		const double peersTotal = static_cast<double>(frozen + late + fast + idle + reference);
		const double rssLimit = run.maxManaged / 1048576.0 + peersTotal * 1.25 + 64.0;
		std::cout << "rss_growth_mib=" << run.rssPeak - run.rssStart << " rss_limit_mib=" << rssLimit << std::endl;
		if(report){ return 0; }
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
		// a sanitizer keeps freed memory in quarantine and shadow pages, the resident set says nothing about the library there
		std::cout << "resident set check skipped under a sanitizer" << std::endl;
#else
		check(run.rssPeak - run.rssStart <= rssLimit, "process memory grew past the managed budget plus one SCTP send buffer per peer");
#endif
		// sends stop before the last part of the budget, which stays for receives and new peers
		check(run.maxBudget <= bufferLimit - bufferLimit / 16, "queued sends took the budget needed by receives");
		check(run.maxFootprint <= sendBudget, "send footprint exceeded its cap");
		check(run.fast.killed == 0, "an innocent fast channel was killed");
		check(run.fast.budget == 0, "fast peers saw BUFFER_BUDGET");
		check(run.fast.dead == 0, "fast peers lost their channel");
		if(fast){
			check(run.received + 2 * fast >= run.remoteMessages, "data written by the fast remotes was lost");
			// the windows below are the real progress checks of saturate, this one only rules out a dead run
			check(run.fast.accepted >= 20 * fast, "fast peers made no progress");
			// grow measured 1.2k accepted before the quota work, 33k after the first quota and 86k to 90k now, 2000 per peer is 8k for four
			if(scenario == "grow"){ check(run.fast.accepted >= 2000 * fast, "fast peers fell back to the starvation of the unfixed quota"); }
			// closing the frozen peers must give their share back
			if(scenario == "shrink"){ check(run.footprintAfterClose <= sendBudget / 4, "closed peers kept their send footprint"); }
		}
		if(reference){
			// healthy peers must keep a fixed fraction of what they reach alone, and only stalled peers may be given up
			check(referenceRate > 0, "no reference rate");
			if(healthySize <= 16384){ check(tailFastRate >= progressShare * referenceRate, "newcomer peers with small frames starved while frozen peers held the pool"); }
			else if(seconds >= 19){ check(lateFastRate >= progressShare * referenceRate, "the memory of the dead frozen peers was not given back to the newcomers"); }
			check(tailRefRate >= progressShare * referenceRate, "established peers starved while frozen peers held the pool");
			check(run.ref.killed == 0 && run.ref.dead == 0 && run.ref.budget == 0, "a reference peer lost its channel or saw BUFFER_BUDGET");
		}
		std::cout << "PASS fast peers keep progress and the budget drains while frozen peers saturate" << std::endl;
	} catch(const std::exception& e){
		std::cerr << "FAIL " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
