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
#include <sstream>
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
	if(input.size() % 2 || input.size() > 524288){
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

int main(int argc, char** argv){
	int result = 0;
	try{
		{
			rtc::Configuration config;
			config.disableAutoNegotiation = true;
			auto peer = std::make_shared<rtc::PeerConnection>(config);
			auto channel = peer->createDataChannel("transport-test");
			std::vector<std::shared_ptr<rtc::DataChannel>> channels{channel};
			const bool data = argc == 2 && std::string(argv[1]) == "--data";
			if(data){
				rtc::DataChannelInit init;
				init.reliability.unordered = true;
				init.reliability.maxRetransmits = 0;
				init.protocol = "binary";
				channels.push_back(peer->createDataChannel("unreliable-test", init));
				init.reliability.maxRetransmits.reset();
				init.reliability.maxPacketLifeTime = 25ms;
				channels.push_back(peer->createDataChannel("timed-test", init));
			}
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
				while(std::getline(std::cin, answer) && answer != "CLOSE"){
					std::istringstream command(answer);
					std::string operation, payload;
					size_t index;
					if(!(command >> operation >> index) || index >= channels.size()){
						throw std::runtime_error("invalid command");
					}
					auto selected = channels[index];
					if(operation == "SEND"){
						command >> payload;
						auto bytes = unhex(payload);
						auto end = std::chrono::steady_clock::now() + 10s;
						while(!selected->isOpen()){
							if(std::chrono::steady_clock::now() > end){ throw std::runtime_error("open deadline"); }
							std::this_thread::sleep_for(2ms);
						}
						selected->send(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
						std::cout << "SENT" << std::endl;
					} else if(operation == "RECEIVE"){
						auto message = selected->receive();
						if(message){
							std::visit([](const auto& bytes){
								std::cout << "DATA " << hex(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())) << std::endl;
							}, *message);
						} else{ std::cout << "EMPTY" << std::endl; }
					} else if(operation == "CLOSECHANNEL"){
						selected->close();
						std::cout << "CLOSED" << std::endl;
					} else{ throw std::runtime_error("unknown command"); }
				}
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
