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

#include <rtc/rtc.hpp>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {
std::string hex(const std::string& input){
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.reserve(input.size() * 2);
	for(unsigned char c : input){
		result += digits[c >> 4];
		result += digits[c & 15];
	}
	return result;
}

std::string unhex(const std::string& input){
	if(input.size() % 2 || input.size() > 131072){
		throw std::runtime_error("invalid answer encoding");
	}
	auto nibble = [](char c){
		if(c >= '0' && c <= '9'){
			return c - '0';
		}
		if(c >= 'a' && c <= 'f'){
			return c - 'a' + 10;
		}
		throw std::runtime_error("invalid hex");
	};
	std::string result;
	result.reserve(input.size() / 2);
	for(size_t i = 0; i < input.size(); i += 2){
		result += static_cast<char>(nibble(input[i]) * 16 + nibble(input[i + 1]));
	}
	return result;
}
}

int main(){
	int result = 0;
	try{
		{
			rtc::Configuration config;
			config.disableAutoNegotiation = true;
			auto peer = std::make_shared<rtc::PeerConnection>(config);
			auto channel = peer->createDataChannel("transport-test");
			peer->setLocalDescription(rtc::Description::Type::Offer);
			auto deadline = std::chrono::steady_clock::now() + 10s;
			while(peer->gatheringState() != rtc::PeerConnection::GatheringState::Complete){
				if(std::chrono::steady_clock::now() > deadline){
					throw std::runtime_error("gather timeout");
				}
				std::this_thread::sleep_for(10ms);
			}
			std::cout << "OFFER " << hex(std::string(*peer->localDescription())) << std::endl;
			std::string answer;
			if(std::getline(std::cin, answer) && answer != "CLOSE"){
				peer->setRemoteDescription(rtc::Description(unhex(answer), rtc::Description::Type::Answer));
				deadline = std::chrono::steady_clock::now() + 15s;
				while(peer->state() != rtc::PeerConnection::State::Connected &&
					  peer->state() != rtc::PeerConnection::State::Failed &&
					  peer->state() != rtc::PeerConnection::State::Closed &&
					  peer->state() != rtc::PeerConnection::State::Disconnected){
					if(std::chrono::steady_clock::now() > deadline){
						throw std::runtime_error("connect timeout");
					}
					std::this_thread::sleep_for(10ms);
				}
				std::cout << "STATE " << peer->state() << std::endl;
				std::getline(std::cin, answer);
			}
			peer->resetCallbacks();
			peer->close();
			channel.reset();
			peer.reset();
		}
	} catch(const std::exception&){
		std::cerr << "offerer failed" << std::endl;
		result = 1;
	}
	auto done = rtc::Cleanup();
	if(done.wait_for(10s) != std::future_status::ready){
		std::cerr << "cleanup timeout" << std::endl;
		std::_Exit(2);
	}
	try{
		done.get();
	} catch(...){
		return 3;
	}
	return result;
}
