// measures dispatcher latency of a victim peer while other peers close, and close durations
#include "../../../src/channel/channel.h"
#include "../../../src/core/dispatcher.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <sstream>
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
Pair connect_once(){
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
Pair connect(){
	for(int attempt = 0; attempt < 4; ++attempt){
		try{ return connect_once(); }catch(const std::exception&){ if(attempt == 3){ throw; } }
	}
	throw std::runtime_error("unreachable");
}
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

#include <dirent.h>
struct T{ std::string id; double cpu; };
std::vector<T> threadcpu(){ std::vector<T> v; if(auto* d = opendir("/proc/self/task")){ while(auto* e = readdir(d)){ if(e->d_name[0] == '.') continue; std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/stat"); std::string l; std::getline(f, l); auto r = l.rfind(')'); std::istringstream is(l.substr(r + 2)); std::string tok; double ut = 0, st = 0; for(int i = 1; i <= 13; ++i){ is >> tok; if(i == 12) ut = std::stod(tok); if(i == 13) st = std::stod(tok); } v.push_back({std::string(e->d_name), (ut + st) / sysconf(_SC_CLK_TCK)}); } closedir(d); } return v; }
int main(int argc, char** argv){
	size_t n = argc > 1 ? std::stoul(argv[1]) : 100; size_t perPair = argc > 2 ? std::stoul(argv[2]) : 5000;
	try{
		std::vector<Pair> pairs(n + 1);
		{ std::atomic<size_t> next{0}; std::vector<std::thread> ts; for(int i = 0; i < 8; ++i){ ts.emplace_back([&]{ for(size_t k; (k = next++) < pairs.size();){ pairs[k] = connect(); } }); } for(auto& t : ts){ t.join(); } }
		auto& victim = pairs[0];
		std::vector<double> quiet, loaded;
		for(int i = 0; i < 200; ++i){ quiet.push_back(ping(victim)); }
		report("victim quiet", quiet);
		auto cpu0 = threadcpu();
		std::atomic<size_t> sent{0}, got{0}, refused{0}; std::vector<std::atomic<size_t>> pend(pairs.size()); std::atomic<bool> done{false};
		auto t0 = clk::now();
		std::vector<std::thread> senders, drainers;
		const size_t S = 4;
		for(size_t w = 0; w < S; ++w){ senders.emplace_back([&, w]{ std::string payload(200, 'p'); for(size_t k = 1 + w; k < pairs.size(); k += S){ } size_t left = (pairs.size() - 1) * perPair / S; size_t k = 1 + w; size_t mine = 0; while(mine < left){ auto& p = pairs[k]; if(pend[k] < 100 && p.ca->send(payload)){ ++pend[k]; ++mine; ++sent; } else{ ++refused; std::this_thread::yield(); } k += S; if(k >= pairs.size()){ k = 1 + w; } } }); }
		for(size_t w = 0; w < 4; ++w){ drainers.emplace_back([&, w]{ char buf[512]; while(!done){ bool any = false; for(size_t k = 1 + w; k < pairs.size(); k += 4){ while(auto sz = pairs[k].cb->read_size()){ pairs[k].cb->copy_front(buf, *sz, true); ++got; --pend[k]; any = true; } } if(!any) std::this_thread::yield(); } }); }
		std::thread pinger([&]{ while(!done){ loaded.push_back(ping(victim)); std::this_thread::sleep_for(2ms); } });
		for(auto& t : senders){ t.join(); }
		const size_t total = sent.load();
		auto deadline = clk::now() + 60s; while(got.load() + 0 < total && clk::now() < deadline){ std::this_thread::sleep_for(5ms); }
		auto secs = std::chrono::duration<double>(clk::now() - t0).count();
		done = true; for(auto& t : drainers){ t.join(); } pinger.join();
		auto cpu1 = threadcpu();
		std::cout << "pairs=" << n << " sent=" << total << " delivered=" << got << " refused=" << refused << " in " << secs << " s => " << got / secs << " msg/s" << std::endl;
		report("victim under load", loaded);
		std::vector<std::pair<double, std::string>> diff; for(auto& a : cpu1){ double b = 0; for(auto& c : cpu0){ if(c.id == a.id) b = c.cpu; } diff.push_back({a.cpu - b, a.id}); }
		std::sort(diff.rbegin(), diff.rend()); std::cout << "top thread cpu seconds:"; for(size_t i = 0; i < 4 && i < diff.size(); ++i){ std::cout << " [" << diff[i].second << "] " << diff[i].first; } std::cout << std::endl;
		std::cout.flush(); _exit(0);
	}catch(const std::exception& e){ std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
