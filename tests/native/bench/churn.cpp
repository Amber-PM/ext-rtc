// create/connect/close churn: resources must return to baseline
#include "../../../src/channel/channel.h"
#include "../../../src/core/dispatcher.h"
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <thread>
#include <rtc/global.hpp>
#include <unistd.h>
using namespace amber::rtc;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;
template<class F> void wait(F f, const char* why){ auto end = clk::now() + 12s; while(!f()){ if(clk::now() > end){ throw std::runtime_error(why); } std::this_thread::sleep_for(1ms); } }
size_t fds(){ size_t n = 0; if(auto* d = opendir("/proc/self/fd")){ while(readdir(d)){ ++n; } closedir(d); } return n; }
long field(const char* key){ std::ifstream f("/proc/self/status"); std::string l; while(std::getline(f, l)){ if(l.rfind(key, 0) == 0){ return std::stol(l.substr(strlen(key) + 1)); } } return -1; }
void cycle(unsigned i){
	Options o; o.pending = 2;
	auto a = Peer::create(o), b = Peer::create(o);
	auto ca = a->create_channel("c", {});
	wait([&]{ a->snapshot(); std::lock_guard l(a->mutex); return a->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather a");
	a->snapshot();
	b->remote(*a->localDescription, true);
	wait([&]{ b->snapshot(); std::lock_guard l(b->mutex); return b->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather b");
	b->snapshot();
	a->remote(*b->localDescription, false);
	std::shared_ptr<Channel> cb;
	try{ wait([&]{ for(auto c : b->poll_channels()){ cb = c; } return cb && ca->is_open() && cb->is_open(); }, "open"); }catch(...){
		auto st = [](Peer& x){ std::lock_guard l(x.mutex); return std::to_string(int(x.state)) + "/g" + std::to_string(int(x.gathering)) + "/f" + (x.failure ? std::to_string(int(*x.failure)) : "-"); };
		std::cerr << "STALL cycle " << i << " a=" << st(*a) << " b=" << st(*b) << " ca_open=" << ca->is_open() << " cb=" << bool(cb) << std::endl; throw; }
	for(int k = 0; k < 4; ++k){ ca->send(std::string(1000, 'x')); }
	if(i % 3 == 0){ a->close(); b->close(); } else if(i % 3 == 1){ b->close(); a->close(); }
	// i % 3 == 2: rely on destructors only
}
int main(int argc, char** argv){
	if(getenv("RTC_LOG")){ ::rtc::InitLogger(::rtc::LogLevel::Info, [](::rtc::LogLevel l, std::string m){ std::cerr << "LOG[" << int(l) << "] " << m << std::endl; }); }
	unsigned cycles = argc > 1 ? std::stoul(argv[1]) : 1000;
	try{
		for(unsigned i = 0; i < 50; ++i){ cycle(i); }
		wait([]{ return active_peers() == 0 && managed_buffer_bytes() == 0; }, "warmup cleanup");
		const auto fd0 = fds(); const auto th0 = field("Threads"); const auto rss0 = field("VmRSS");
		std::cout << "baseline fds=" << fd0 << " threads=" << th0 << " rssKB=" << rss0 << std::endl;
		auto t0 = clk::now();
		for(unsigned i = 0; i < cycles; ++i){
			cycle(i);
			if((i + 1) % (cycles / 4) == 0){ std::cout << "cycle " << i + 1 << " fds=" << fds() << " threads=" << field("Threads") << " rssKB=" << field("VmRSS") << " active=" << active_peers() << std::endl; }
		}
		std::cout << "cycles/s=" << cycles / std::chrono::duration<double>(clk::now() - t0).count() << std::endl;
		wait([]{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }, "final cleanup");
		const auto fd1 = fds(); const auto th1 = field("Threads"); const auto rss1 = field("VmRSS");
		std::cout << "final fds=" << fd1 << " threads=" << th1 << " rssKB=" << rss1 << std::endl;
		if(fd1 > fd0 + 4 || th1 > th0 + 4 || rss1 > rss0 + 20000){ std::cerr << "FAIL resource growth\n"; return 1; }
		std::cout << "PASS churn" << std::endl;
		std::cout.flush(); _exit(0);
	}catch(const std::exception& e){ std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
