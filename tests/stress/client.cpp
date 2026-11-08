// rtc stress client: hosts many libdatachannel offerers against the php server driver
// signaling is a line protocol over one tcp connection to server.php
#include <rtc/rtc.hpp>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static uint64_t now_ns(){ return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

// log buckets with 2 percent precision, rtt in microseconds
struct Hist{
	static constexpr int N = 900;
	uint64_t c[N] = {};
	uint64_t n = 0, maxUs = 0;
	void add(uint64_t us){
		int i = us <= 1 ? 0 : (std::min)(N - 1, (int)(std::log((double)us) / std::log(1.02)));
		++c[i]; ++n; if(us > maxUs){ maxUs = us; }
	}
	double pct(double p) const {
		if(!n){ return 0; }
		uint64_t target = (uint64_t)std::ceil(p * n), acc = 0;
		for(int i = 0; i < N; ++i){ acc += c[i]; if(acc >= target){ return std::pow(1.02, i + 1); } }
		return (double)maxUs;
	}
};

struct Opt{
	std::string server = "127.0.0.1:9000";
	int count = 10, parallel = 32, duration = 10, rounds = 1, hold = 2, size = 64, slowPeers = 0, readDelayMs = 0;
	int connectTimeout = 30, floodPeers = 0, gapMs = 200, churnInterval = 10;
	double echoRate = 10, streamRate = 0, churnFrac = 0;
	uint64_t floodCount = 0; int floodSize = 1000;
	std::string out, name = "client";
};
static Opt opt;

static std::mutex statMutex;
static Hist relHist, unrelHist;
static std::map<int, std::pair<Hist, Hist>> timeline;
static uint64_t startNs;
static std::atomic<uint64_t> relSent{0}, unrelSent{0}, relRecv{0}, unrelRecv{0}, sendExc{0}, sendBuffered{0};
static std::atomic<uint64_t> dRelRecv{0}, dRelBad{0}, dUnrelRecv{0}, dUnrelLost{0}, dUnrelReorder{0};
static std::atomic<uint64_t> fRecv{0}, fBad{0}, bytesIn{0}, bytesOut{0}, dropped{0}, connAttempt{0}, connOk{0}, connFail{0};
static std::vector<double> connectMs;
static std::vector<std::string> roundsJson;

static int sockFd = -1;
static std::mutex sockMutex;
static void sendLine(const std::string& line){
	std::lock_guard l(sockMutex);
	size_t off = 0;
	while(off < line.size()){
		auto r = ::send(sockFd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
		if(r <= 0){ return; }
		off += r;
	}
}

static std::string hex(const std::string& s){
	static const char d[] = "0123456789abcdef"; std::string r; r.reserve(s.size() * 2);
	for(unsigned char c : s){ r += d[c >> 4]; r += d[c & 15]; }
	return r;
}
static std::string unhex(const std::string& s){
	auto nb = [](char c){ return c <= '9' ? c - '0' : c - 'a' + 10; };
	std::string r;
	for(size_t i = 0; i + 1 < s.size(); i += 2){ r += (char)(nb(s[i]) * 16 + nb(s[i + 1])); }
	return r;
}

struct P{
	int id = 0, index = 0;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::DataChannel> rel, unrel;
	std::atomic<bool> relOpen{false}, unrelOpen{false}, failed{false}, connectedOnce{false}, closing{false}, sentControl{false};
	// failure diagnosis: 1 state change, 2 remote description exception, 3 server reject, 4 timeout
	std::atomic<int> why{0}, lastState{-1};
	std::atomic<bool> offerSent{false}, answerSeen{false};
	std::mutex histMutex;
	std::string hist, answerSdp, offerSdp;
	uint64_t t0 = 0;
	uint64_t expRel = 0, expUnrel = 0, expFlood = 0;
	uint64_t seqEcho = 0, seqStreamRel = 0, seqStreamUnrel = 0;
	Clock::time_point nextEcho, nextStream;
	bool ready() const { return relOpen && unrelOpen && !failed && !closing; }
};
static std::mutex peersMutex;
static std::map<int, std::shared_ptr<P>> peers;
static std::atomic<int> nextId{1};
static std::atomic<bool> stopLoad{false};

static void put64(std::string& s, uint64_t v){ for(int i = 0; i < 8; ++i){ s += (char)((v >> (8 * i)) & 255); } }
static uint64_t get64(const std::byte* b){ uint64_t v = 0; for(int i = 0; i < 8; ++i){ v |= (uint64_t)(uint8_t)b[i] << (8 * i); } return v; }

static void onMessage(const std::shared_ptr<P>& p, bool reliable, rtc::message_variant m){
	auto* bin = std::get_if<rtc::binary>(&m);
	if(!bin || bin->empty()){ return; }
	const auto& b = *bin;
	bytesIn += b.size();
	char t = (char)b[0];
	if(t == 'r' && b.size() >= 17){
		uint64_t ts = get64(b.data() + 9), now = now_ns(), us = (now - ts) / 1000;
		(reliable ? relRecv : unrelRecv)++;
		int sec = (int)((now - startNs) / 1000000000ull);
		std::lock_guard l(statMutex);
		(reliable ? relHist : unrelHist).add(us);
		auto& tl = timeline[sec];
		(reliable ? tl.first : tl.second).add(us);
	} else if(t == 'D' && b.size() >= 9){
		uint64_t seq = get64(b.data() + 1);
		if(reliable){ ++dRelRecv; if(seq != p->expRel){ ++dRelBad; } p->expRel = seq + 1; }
		else{
			++dUnrelRecv;
			if(seq > p->expUnrel){ dUnrelLost += seq - p->expUnrel; p->expUnrel = seq + 1; }
			else if(seq < p->expUnrel){ ++dUnrelReorder; }
			else{ p->expUnrel = seq + 1; }
		}
	} else if(t == 'F' && b.size() >= 9){
		uint64_t seq = get64(b.data() + 1);
		++fRecv; if(seq != p->expFlood){ ++fBad; } p->expFlood = seq + 1;
		if(opt.readDelayMs > 0 && p->index < opt.slowPeers){ std::this_thread::sleep_for(std::chrono::milliseconds(opt.readDelayMs)); }
	}
}

static std::shared_ptr<P> createPeer(int index){
	auto p = std::make_shared<P>();
	p->id = nextId++; p->index = index; p->t0 = now_ns();
	rtc::Configuration cfg; cfg.disableAutoNegotiation = true;
	p->pc = std::make_shared<rtc::PeerConnection>(cfg);
	std::weak_ptr<P> w = p;
	p->pc->onStateChange([w](rtc::PeerConnection::State s){
		auto q = w.lock(); if(!q){ return; }
		q->lastState = (int)s;
		{ std::lock_guard l(q->histMutex); q->hist += std::to_string((int)s) + "@" + std::to_string((now_ns() - q->t0) / 1000000) + "ms "; }
		if(s == rtc::PeerConnection::State::Connected){ q->connectedOnce = true; }
		if((s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed || s == rtc::PeerConnection::State::Disconnected) && !q->failed.exchange(true)){
			if(!q->why){ q->why = 1; }
			if(q->connectedOnce && !q->closing){ ++dropped; }
		}
	});
	p->pc->onGatheringStateChange([w](rtc::PeerConnection::GatheringState s){
		auto q = w.lock(); if(!q || s != rtc::PeerConnection::GatheringState::Complete){ return; }
		auto d = q->pc->localDescription(); if(!d){ return; }
		q->offerSent = true;
		{ std::lock_guard l(q->histMutex); q->offerSdp = std::string(*d); q->hist += "offer@" + std::to_string((now_ns() - q->t0) / 1000000) + "ms "; }
		sendLine("OFFER " + std::to_string(q->id) + " " + hex(std::string(*d)) + "\n");
	});
	p->rel = p->pc->createDataChannel("ReliableDataChannel");
	rtc::DataChannelInit init; init.reliability.unordered = true; init.reliability.maxRetransmits = 0;
	p->unrel = p->pc->createDataChannel("UnreliableDataChannel", init);
	p->rel->onOpen([w]{ if(auto q = w.lock()){ q->relOpen = true; } });
	p->unrel->onOpen([w]{ if(auto q = w.lock()){ q->unrelOpen = true; } });
	p->rel->onMessage([w](rtc::message_variant m){ if(auto q = w.lock()){ onMessage(q, true, std::move(m)); } });
	p->unrel->onMessage([w](rtc::message_variant m){ if(auto q = w.lock()){ onMessage(q, false, std::move(m)); } });
	{ std::lock_guard l(peersMutex); peers[p->id] = p; }
	++connAttempt;
	p->pc->setLocalDescription(rtc::Description::Type::Offer);
	return p;
}

static void closePeer(const std::shared_ptr<P>& p){
	p->closing = true;
	try{ p->pc->resetCallbacks(); p->pc->close(); }catch(...){}
	std::lock_guard l(peersMutex); peers.erase(p->id);
}

static void readerThread(){
	std::string buf; char tmp[65536];
	for(;;){
		auto r = ::recv(sockFd, tmp, sizeof tmp, 0);
		if(r <= 0){ return; }
		buf.append(tmp, r);
		size_t nl;
		while((nl = buf.find('\n')) != std::string::npos){
			std::string line = buf.substr(0, nl); buf.erase(0, nl + 1);
			std::istringstream in(line); std::string cmd, rest; int id = 0;
			in >> cmd >> id >> rest;
			std::shared_ptr<P> p;
			{ std::lock_guard l(peersMutex); auto it = peers.find(id); if(it != peers.end()){ p = it->second; } }
			if(!p){ continue; }
			if(cmd == "ANSWER"){
				p->answerSeen = true;
				{ std::lock_guard l(p->histMutex); p->answerSdp = unhex(rest); p->hist += "answer@" + std::to_string((now_ns() - p->t0) / 1000000) + "ms "; }
				try{ p->pc->setRemoteDescription(rtc::Description(unhex(rest), rtc::Description::Type::Answer)); }
				catch(...){ p->why = 2; p->failed = true; }
			} else if(cmd == "REJECT"){ p->why = 3; p->failed = true; }
		}
	}
}

struct BatchResult{ int ok = 0, fail = 0; std::vector<double> ms; };

// start n peers with a bounded number of unfinished handshakes
static BatchResult connectBatch(int n, std::vector<std::shared_ptr<P>>& out, int firstIndex){
	BatchResult r; std::vector<std::shared_ptr<P>> pending; int started = 0;
	auto settle = [&](){
		for(auto it = pending.begin(); it != pending.end();){
			auto p = *it; double ms = (now_ns() - p->t0) / 1e6; bool done = false;
			if(p->ready()){ ++r.ok; ++connOk; r.ms.push_back(ms); out.push_back(p); done = true; }
			else if(p->failed || ms > opt.connectTimeout * 1000.0){
				if(!p->failed){ p->why = 4; }
				std::cerr << "[" << opt.name << "] FAIL peer=" << p->id << " why=" << p->why << " lastState=" << p->lastState << " offerSent=" << p->offerSent
						  << " answerSeen=" << p->answerSeen << " rel=" << p->relOpen << " unrel=" << p->unrelOpen << " at_ms=" << (int)ms << std::endl;
				{
					std::lock_guard l(p->histMutex);
					std::cerr << "[" << opt.name << "]   history: " << p->hist << " ice=" << (int)p->pc->iceState() << " gather=" << (int)p->pc->gatheringState() << std::endl;
					if(!opt.out.empty()){
						FILE* f = fopen((opt.out + ".fail-" + std::to_string(p->id) + ".sdp").c_str(), "w");
						if(f){ fprintf(f, "--- offer\n%s\n--- answer\n%s\n", p->offerSdp.c_str(), p->answerSdp.c_str()); fclose(f); }
					}
				}
				++r.fail; ++connFail; closePeer(p); done = true;
			}
			it = done ? pending.erase(it) : it + 1;
		}
	};
	while(started < n || !pending.empty()){
		settle();
		while(started < n && (int)pending.size() < opt.parallel){
			try{ pending.push_back(createPeer(firstIndex + started)); }
			catch(const std::exception&){ ++r.fail; ++connFail; }
			++started;
		}
		std::this_thread::sleep_for(2ms);
	}
	return r;
}

static std::string payload(char type, uint64_t seq, uint64_t ts, size_t size){
	std::string s; s += type; put64(s, seq); if(type == 'R'){ put64(s, ts); }
	if(s.size() < size){ s.resize(size, 'x'); }
	return s;
}
static void trySend(const std::shared_ptr<rtc::DataChannel>& ch, const std::string& s, bool reliable, bool echo = false){
	try{
		if(!ch->send(reinterpret_cast<const std::byte*>(s.data()), s.size())){ ++sendBuffered; }
		bytesOut += s.size();
		if(echo){ (reliable ? relSent : unrelSent)++; }
	}catch(...){ ++sendExc; }
}

static void loadLoop(){
	while(!stopLoad){
		std::vector<std::shared_ptr<P>> snap;
		{ std::lock_guard l(peersMutex); for(auto& kv : peers){ snap.push_back(kv.second); } }
		auto now = Clock::now();
		for(auto& p : snap){
			if(!p->ready()){ continue; }
			if(!p->sentControl.exchange(true)){
				if(opt.streamRate > 0){
					trySend(p->rel, "CSTREAM " + std::to_string((int)opt.streamRate) + " " + std::to_string(opt.size), true);
				}
				if(opt.floodCount > 0 && (opt.floodPeers == 0 || p->index < opt.floodPeers)){
					trySend(p->rel, "CFLOOD " + std::to_string(opt.floodSize) + " " + std::to_string(opt.floodCount), true);
				}
				p->nextEcho = now; p->nextStream = now;
			}
			if(opt.echoRate > 0 && now >= p->nextEcho){
				auto step = std::chrono::nanoseconds((int64_t)(1e9 / opt.echoRate));
				p->nextEcho += step; if(p->nextEcho < now - 1s){ p->nextEcho = now; }
				trySend(p->rel, payload('R', p->seqEcho, now_ns(), opt.size), true, true);
				trySend(p->unrel, payload('R', p->seqEcho, now_ns(), opt.size), false, true);
				++p->seqEcho;
			}
			if(opt.streamRate > 0 && now >= p->nextStream){
				auto step = std::chrono::nanoseconds((int64_t)(1e9 / opt.streamRate));
				p->nextStream += step; if(p->nextStream < now - 1s){ p->nextStream = now; }
				trySend(p->rel, payload('D', p->seqStreamRel++, 0, opt.size), true);
				trySend(p->unrel, payload('D', p->seqStreamUnrel++, 0, opt.size), false);
			}
		}
		std::this_thread::sleep_for(1ms);
	}
}

static std::string q(double v){ char b[64]; snprintf(b, sizeof b, "%.2f", v); return b; }
static std::string pj(const Hist& h){
	return "{\"n\":" + std::to_string(h.n) + ",\"p50_us\":" + q(h.pct(.5)) + ",\"p95_us\":" + q(h.pct(.95)) + ",\"p99_us\":" + q(h.pct(.99)) + ",\"max_us\":" + std::to_string(h.maxUs) + "}";
}
static std::string pctVec(std::vector<double> v){
	if(v.empty()){ return "{\"n\":0}"; }
	std::sort(v.begin(), v.end());
	auto at = [&](double p){ size_t i = (size_t)std::ceil(p * v.size()); return v[(std::min)(v.size() - 1, i ? i - 1 : 0)]; };
	return "{\"n\":" + std::to_string(v.size()) + ",\"p50\":" + q(at(.5)) + ",\"p95\":" + q(at(.95)) + ",\"p99\":" + q(at(.99)) + ",\"max\":" + q(v.back()) + "}";
}

int main(int argc, char** argv){
	for(int i = 1; i < argc; ++i){
		std::string a = argv[i]; auto val = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if(a == "--server") opt.server = val(); else if(a == "--count") opt.count = std::stoi(val());
		else if(a == "--parallel") opt.parallel = std::stoi(val()); else if(a == "--duration") opt.duration = std::stoi(val());
		else if(a == "--rounds") opt.rounds = std::stoi(val()); else if(a == "--hold") opt.hold = std::stoi(val());
		else if(a == "--size") opt.size = std::stoi(val()); else if(a == "--echo-rate") opt.echoRate = std::stod(val());
		else if(a == "--stream-rate") opt.streamRate = std::stod(val()); else if(a == "--flood-count") opt.floodCount = std::stoull(val());
		else if(a == "--flood-size") opt.floodSize = std::stoi(val()); else if(a == "--flood-peers") opt.floodPeers = std::stoi(val());
		else if(a == "--slow-peers") opt.slowPeers = std::stoi(val()); else if(a == "--read-delay-ms") opt.readDelayMs = std::stoi(val());
		else if(a == "--churn-frac") opt.churnFrac = std::stod(val()); else if(a == "--churn-interval") opt.churnInterval = std::stoi(val());
		else if(a == "--connect-timeout") opt.connectTimeout = std::stoi(val()); else if(a == "--gap-ms") opt.gapMs = std::stoi(val());
		else if(a == "--out") opt.out = val(); else if(a == "--name") opt.name = val();
		else{ std::cerr << "unknown arg " << a << "\n"; return 2; }
	}
	auto colon = opt.server.rfind(':');
	addrinfo hints{}, *res; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_INET;
	if(getaddrinfo(opt.server.substr(0, colon).c_str(), opt.server.substr(colon + 1).c_str(), &hints, &res)){ std::cerr << "resolve failed\n"; return 2; }
	sockFd = socket(AF_INET, SOCK_STREAM, 0);
	if(connect(sockFd, res->ai_addr, res->ai_addrlen)){ std::cerr << "signaling connect failed\n"; return 2; }
	freeaddrinfo(res);
	sendLine("HELLO client\n");
	std::thread reader(readerThread);
	startNs = now_ns();
	auto startUnix = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::thread load(loadLoop);
	std::vector<std::shared_ptr<P>> active;

	for(int round = 0; round < opt.rounds; ++round){
		uint64_t r0 = now_ns();
		active.clear();
		auto b = connectBatch(opt.count, active, 0);
		double connectWall = (now_ns() - r0) / 1e6;
		for(auto m : b.ms){ connectMs.push_back(m); }
		roundsJson.push_back("{\"round\":" + std::to_string(round) + ",\"ok\":" + std::to_string(b.ok) + ",\"fail\":" + std::to_string(b.fail) + ",\"wall_ms\":" + q(connectWall) + ",\"connect_ms\":" + pctVec(b.ms) + "}");
		std::cerr << "[" << opt.name << "] round " << round << " ok=" << b.ok << " fail=" << b.fail << " wall_ms=" << q(connectWall) << std::endl;
		int dur = opt.rounds > 1 ? opt.hold : opt.duration;
		if(opt.churnFrac > 0 && opt.rounds == 1){
			std::mt19937 rng(12345);
			auto end = Clock::now() + std::chrono::seconds(dur);
			while(Clock::now() < end){
				auto left = std::chrono::duration_cast<std::chrono::seconds>(end - Clock::now()).count();
				std::this_thread::sleep_for(std::chrono::seconds((std::min<long>)(opt.churnInterval, left + 1)));
				if(Clock::now() >= end){ break; }
				int k = (int)(active.size() * opt.churnFrac); std::vector<std::shared_ptr<P>> fresh;
				std::shuffle(active.begin(), active.end(), rng);
				for(int i = 0; i < k; ++i){ closePeer(active.back()); active.pop_back(); }
				auto nb = connectBatch(k, fresh, 10000);
				for(auto m : nb.ms){ connectMs.push_back(m); }
				for(auto& f : fresh){ active.push_back(f); }
			}
		} else{ std::this_thread::sleep_for(std::chrono::seconds(dur)); }
		if(round + 1 < opt.rounds){
			for(auto& p : active){ closePeer(p); }
			active.clear();
			std::this_thread::sleep_for(std::chrono::milliseconds(opt.gapMs));
		}
	}
	int aliveEnd = 0;
	for(auto& p : active){ if(p->ready()){ ++aliveEnd; } }
	stopLoad = true; load.join();
	for(auto& p : active){ closePeer(p); }
	active.clear();
	::shutdown(sockFd, SHUT_RDWR);
	reader.join(); ::close(sockFd);
	auto cleanup = rtc::Cleanup();
	bool cleanOk = cleanup.wait_for(15s) == std::future_status::ready;

	std::ostringstream o;
	o << "{\"name\":\"" << opt.name << "\",\"start_unix_ms\":" << startUnix << ",\"params\":{\"count\":" << opt.count << ",\"duration\":" << opt.duration
	  << ",\"rounds\":" << opt.rounds << ",\"echo_rate\":" << opt.echoRate << ",\"stream_rate\":" << opt.streamRate << ",\"size\":" << opt.size << "}"
	  << ",\"connect\":{\"attempted\":" << connAttempt << ",\"ok\":" << connOk << ",\"fail\":" << connFail << ",\"ms\":" << pctVec(connectMs) << "},\"rounds\":[";
	for(size_t i = 0; i < roundsJson.size(); ++i){ o << (i ? "," : "") << roundsJson[i]; }
	o << "],\"alive_at_end\":" << aliveEnd << ",\"dropped_peers\":" << dropped << ",\"rtt_reliable\":" << pj(relHist) << ",\"rtt_unreliable\":" << pj(unrelHist)
	  << ",\"echo\":{\"rel_sent\":" << relSent << ",\"rel_recv\":" << relRecv << ",\"unrel_sent\":" << unrelSent << ",\"unrel_recv\":" << unrelRecv
	  << ",\"unrel_loss_pct\":" << q(unrelSent ? 100.0 * (1.0 - (double)unrelRecv / unrelSent) : 0) << "}"
	  << ",\"stream\":{\"rel_recv\":" << dRelRecv << ",\"rel_order_violations\":" << dRelBad << ",\"unrel_recv\":" << dUnrelRecv << ",\"unrel_lost\":" << dUnrelLost << ",\"unrel_reordered\":" << dUnrelReorder << "}"
	  << ",\"flood\":{\"recv\":" << fRecv << ",\"order_violations\":" << fBad << "},\"send_exceptions\":" << sendExc << ",\"send_buffered\":" << sendBuffered
	  << ",\"bytes_in\":" << bytesIn << ",\"bytes_out\":" << bytesOut << ",\"cleanup_ok\":" << (cleanOk ? "true" : "false") << ",\"timeline\":[";
	bool first = true;
	for(auto& kv : timeline){
		auto& h = kv.second;
		o << (first ? "" : ",") << "{\"t\":" << kv.first << ",\"rel_n\":" << h.first.n << ",\"rel_p50\":" << q(h.first.pct(.5)) << ",\"rel_p99\":" << q(h.first.pct(.99)) << ",\"rel_max\":" << h.first.maxUs
		  << ",\"unrel_n\":" << h.second.n << ",\"unrel_p99\":" << q(h.second.pct(.99)) << "}";
		first = false;
	}
	o << "]}";
	if(!opt.out.empty()){ FILE* f = fopen(opt.out.c_str(), "w"); if(f){ fputs(o.str().c_str(), f); fclose(f); } }
	else{ std::cout << o.str() << std::endl; }
	if(!cleanOk){ std::_Exit(3); }
	return 0;
}
