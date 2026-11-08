// an external offerer is SIGKILLed while the server queue is full; other peers must stay responsive and resources return
#include "../../../src/channel/channel.h"
#include "../../../src/core/dispatcher.h"
#include <algorithm>
#include <csignal>
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
using namespace amber::rtc;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;
extern char** environ;
template<class F> void wait(F f, const char* why, std::chrono::seconds s = 20s){ auto end = clk::now() + s; while(!f()){ if(clk::now() > end){ throw std::runtime_error(why); } std::this_thread::yield(); } }
std::string hex(const std::string& in){ static const char d[] = "0123456789abcdef"; std::string r; for(unsigned char c : in){ r += d[c >> 4]; r += d[c & 15]; } return r; }
std::string unhex(const std::string& in){ std::string r; for(size_t i = 0; i + 1 < in.size(); i += 2){ r += char(std::stoi(in.substr(i, 2), nullptr, 16)); } return r; }
struct Child{ pid_t pid = 0; FILE* out = nullptr; FILE* in = nullptr; };
Child spawn_offerer(const char* path){
	int toChild[2], fromChild[2];
	if(pipe(toChild) || pipe(fromChild)){ throw std::runtime_error("pipe"); }
	posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, toChild[0], 0); posix_spawn_file_actions_adddup2(&fa, fromChild[1], 1);
	posix_spawn_file_actions_addclose(&fa, toChild[1]); posix_spawn_file_actions_addclose(&fa, fromChild[0]);
	Child c; char* argv[] = {const_cast<char*>(path), nullptr};
	if(posix_spawn(&c.pid, path, &fa, nullptr, argv, environ)){ throw std::runtime_error("spawn"); }
	close(toChild[0]); close(fromChild[1]);
	c.in = fdopen(toChild[1], "w"); c.out = fdopen(fromChild[0], "r");
	return c;
}
double ms(clk::duration d){ return std::chrono::duration<double, std::milli>(d).count(); }
int main(int argc, char** argv){
	try{
		Options o; o.pending = 2; o.sendBytes = 1 << 20;
		// victim pair in-process
		auto va = Peer::create(o), vb = Peer::create(o);
		auto vca = va->create_channel("v", {});
		wait([&]{ va->snapshot(); std::lock_guard l(va->mutex); return va->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "g1");
		va->snapshot(); vb->remote(*va->localDescription, true);
		wait([&]{ vb->snapshot(); std::lock_guard l(vb->mutex); return vb->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "g2");
		vb->snapshot(); va->remote(*vb->localDescription, false);
		std::shared_ptr<Channel> vcb;
		wait([&]{ for(auto c : vb->poll_channels()){ vcb = c; } return vcb && vca->is_open() && vcb->is_open(); }, "victim open");
		auto ping = [&]{ auto t = clk::now(); vca->send("x"); wait([&]{ return vcb->queued_messages() > 0; }, "victim delivery"); auto d = clk::now() - t; char c; vcb->copy_front(&c, 1, true); return ms(d); };
		std::vector<double> quiet; for(int i = 0; i < 200; ++i){ quiet.push_back(ping()); }
		std::sort(quiet.begin(), quiet.end());
		std::cout << "victim quiet p50=" << quiet[100] << " p99=" << quiet[197] << " max=" << quiet.back() << " ms" << std::endl;
		// external client
		auto child = spawn_offerer(argv[1]);
		char line[1 << 20];
		if(!fgets(line, sizeof line, child.out)){ throw std::runtime_error("no offer"); }
		std::string offer = unhex(std::string(line + 6, strcspn(line + 6, "\r\n")));
		auto srv = Peer::create(o);
		srv->remote(offer, true);
		wait([&]{ srv->snapshot(); std::lock_guard l(srv->mutex); return srv->gathering == ::rtc::PeerConnection::GatheringState::Complete; }, "g3");
		srv->snapshot();
		fprintf(child.in, "%s\n", hex(*srv->localDescription).c_str()); fflush(child.in);
		std::shared_ptr<Channel> sc;
		wait([&]{ for(auto c : srv->poll_channels()){ sc = c; } return sc && sc->is_open(); }, "external open");
		// fill the send queue
		std::string payload(1000, 'z'); size_t queued = 0;
		while(sc->send(payload)){ ++queued; }
		std::cout << "server queued " << queued << " messages before refusal" << std::endl;
		std::vector<double> during; std::atomic<bool> stop{false};
		if(argc > 2){
			std::atomic<bool> go{false}; std::vector<double> lat;
			std::thread closer([&]{ go = true; auto t = clk::now(); srv->close(); std::cout << "close() of LIVE peer with full queue took " << ms(clk::now() - t) << " ms" << std::endl; go = false; });
			while(!go){ std::this_thread::yield(); }
			while(go){ lat.push_back(ping()); }
			closer.join();
			std::sort(lat.begin(), lat.end());
			std::cout << "victim during live close n=" << lat.size() << " max=" << lat.back() << " ms" << std::endl;
			kill(child.pid, SIGKILL); waitpid(child.pid, nullptr, 0);
			sc.reset(); srv.reset(); va->close(); vb->close(); vca.reset(); vcb.reset(); va.reset(); vb.reset();
			wait([]{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }, "resources not released");
			std::cout << "PASS live close" << std::endl; std::cout.flush(); _exit(0);
		}
		auto killedAt = clk::now();
		kill(child.pid, SIGKILL); waitpid(child.pid, nullptr, 0);
		std::optional<double> detect;
		while(clk::now() - killedAt < 40s){
			during.push_back(ping());
			{ std::lock_guard l(srv->mutex); if(!detect && srv->failure){ detect = ms(clk::now() - killedAt); break; } }
			std::this_thread::sleep_for(1ms);
		}
		std::sort(during.begin(), during.end());
		std::cout << "victim while peer dead n=" << during.size() << " p50=" << during[during.size() / 2] << " p99=" << during[during.size() * 99 / 100] << " max=" << during.back() << " ms" << std::endl;
		if(detect){ std::cout << "failure detected after " << *detect << " ms" << std::endl; } else{ std::cout << "failure NOT detected within 40s (peer state kept connected)" << std::endl; }
		auto t = clk::now(); srv->close(); std::cout << "close() of dead peer took " << ms(clk::now() - t) << " ms" << std::endl;
		sc.reset(); srv.reset();
		wait([&]{ std::lock_guard l(va->queues->mutex); return true; }, "x");
		va->close(); vb->close(); vca.reset(); vcb.reset(); va.reset(); vb.reset();
		wait([]{ return active_peers() == 0 && active_channels() == 0 && managed_buffer_bytes() == 0; }, "resources not released");
		std::cout << "PASS abrupt disconnect: resources released" << std::endl;
		std::cout.flush(); _exit(0);
	}catch(const std::exception& e){ std::cerr << "FAIL " << e.what() << std::endl; return 1; }
}
