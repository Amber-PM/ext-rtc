// measures dispatcher latency of a victim peer while other peers close, and close durations
#include "../../../src/channel/channel.h"
#include "../../../src/core/dispatcher.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <dirent.h>
#include <fstream>
#include <cstring>
#include <rtc/global.hpp>
#include <unistd.h>
#include <vector>
using namespace amber::rtc;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;
struct Pair{ std::shared_ptr<Peer> a, b; std::shared_ptr<Channel> ca, cb; };
template<class F> void wait(F f, const char* why){ auto end = clk::now() + 15s; while(!f()){ if(clk::now() > end){ throw std::runtime_error(why); } std::this_thread::yield(); } }
Pair connect(){
	Options o; o.pending = 2;
	Pair p; p.a = Peer::create(o); p.b = Peer::create(o);
	p.ca = p.a->create_channel("c", {});
	wait([&]{ p.a->snapshot(); std::lock_guard l(p.a->mutex); return p.a->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather a");
	p.a->snapshot();
	p.b->remote(*p.a->localDescription, true);
	wait([&]{ p.b->snapshot(); std::lock_guard l(p.b->mutex); return p.b->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "gather b");
	p.b->snapshot();
	p.a->remote(*p.b->localDescription, false);
	try{
		wait([&]{ for(auto c : p.b->poll_channels()){ p.cb = c; } return p.cb && p.ca->is_open() && p.cb->is_open(); }, "open");
	}catch(...){
		auto st = [](Peer& x){ std::lock_guard l(x.mutex); return std::to_string(int(x.state)) + "/g" + std::to_string(int(x.gathering)) + "/f" + (x.failure ? std::to_string(int(*x.failure)) : "-"); };
		std::cerr << "STALL a=" << st(*p.a) << " b=" << st(*p.b) << " ca_open=" << p.ca->is_open() << " cb=" << bool(p.cb) << " cb_open=" << (p.cb && p.cb->is_open()) << " a_active=" << p.a->channels[0].use_count() << std::endl;
		throw;
	}
	return p;
}
size_t fdcount(){ size_t n = 0; if(auto* d = opendir("/proc/self/fd")){ while(readdir(d)){ ++n; } closedir(d); } return n; }
long field(const char* key){ std::ifstream f("/proc/self/status"); std::string l; while(std::getline(f, l)){ if(l.rfind(key, 0) == 0){ return std::stol(l.substr(strlen(key) + 1)); } } return -1; }
double ms(clk::duration d){ return std::chrono::duration<double, std::milli>(d).count(); }
void report(const char* name, std::vector<double> v){
	if(v.empty()){ std::cout << name << ": none\n"; return; }
	std::sort(v.begin(), v.end());
	auto q = [&](double f){ return v[std::min(v.size() - 1, size_t(f * v.size()))]; };
	std::cout << name << ": n=" << v.size() << " p50=" << q(.5) << " p95=" << q(.95) << " p99=" << q(.99) << " max=" << v.back() << " ms\n";
}
double ping(Pair& v){
	auto t = clk::now();
	if(!v.ca->send("x")){ throw std::runtime_error("victim send refused"); }
	wait([&]{ return v.cb->queued_messages() > 0; }, "victim delivery");
	auto d = clk::now() - t;
	char c; v.cb->copy_front(&c, 1, true);
	return ms(d);
}
int main(int argc, char** argv){
	size_t n = argc > 1 ? std::stoul(argv[1]) : 100;
	if(getenv("RTC_LOG")){ ::rtc::InitLogger(::rtc::LogLevel::Info, [](::rtc::LogLevel l, std::string m){ std::cerr << "LOG[" << int(l) << "] " << m << std::endl; }); }
	try{
		std::vector<Pair> pairs(n + 1);
		auto t0 = clk::now();
		{
			std::atomic<size_t> next{0};
			std::vector<std::thread> ts;
			for(int i = 0; i < 8; ++i){ ts.emplace_back([&]{ for(size_t k; (k = next++) < pairs.size();){ try{ pairs[k] = connect(); }catch(const std::exception& e){ std::cerr << "connect fail " << k << ": " << e.what() << std::endl; _exit(3); } } }); }
			for(auto& t : ts){ t.join(); }
		}
		std::cout << "connected " << pairs.size() << " pairs in " << ms(clk::now() - t0) << " ms\n";
		std::cout << "live " << 2 * pairs.size() << " peers: fds=" << fdcount() << " threads=" << field("Threads") << " rssMB=" << field("VmRSS") / 1024 << " managedKB=" << managed_buffer_bytes() / 1024 << std::endl;
		auto& victim = pairs[0];
		std::vector<double> quiet, busy, closes;
		for(int i = 0; i < 300; ++i){ quiet.push_back(ping(victim)); }
		report("victim quiet", quiet);
		std::atomic<bool> stop{false};
		std::thread closer([&]{
			for(size_t k = 1; k < pairs.size(); ++k){
				try{ pairs[k].ca->send("last"); }catch(...){}
				auto t = clk::now();
				pairs[k].a->close();
				closes.push_back(ms(clk::now() - t));
				t = clk::now();
				pairs[k].b->close();
				closes.push_back(ms(clk::now() - t));
			}
			stop = true;
		});
		while(!stop){ busy.push_back(ping(victim)); }
		closer.join();
		report("victim during closes", busy);
		report("peer close()", closes);
		wait([]{ return managed_buffer_bytes() > 0 || true; }, "x");
		std::cout << "active_peers=" << active_peers() << "\n";
	}catch(const std::exception& e){ std::cerr << "FAIL " << e.what() << "\n"; return 1; }
	std::cout.flush(); _exit(0);
}
