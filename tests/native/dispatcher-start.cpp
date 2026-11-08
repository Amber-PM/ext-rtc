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

// the dispatcher starts its threads when the first peer is created, a thread that cannot be started must surface as an error
// and never as a terminated process or as a dispatcher without workers that accepts sends and drops them
// pthread_create fails like it does under a process or cgroup limit, calls are counted from the dispatcher start

#include "../../src/core/dispatcher.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <pthread.h>
#include <stdexcept>

namespace {
std::atomic<int> failFrom{0}, failTo{0}, calls{0};
}

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attributes, void* (*entry)(void*), void* argument){
	using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
	static auto real = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
	const int n = ++calls;
	if(failFrom.load() && n >= failFrom.load() && n <= failTo.load()){ return EAGAIN; }
	return real(thread, attributes, entry, argument);
}

namespace {
void check(bool value, const char* why){ if(!value){ throw std::runtime_error(why); } }

bool starts(){
	try{
		amber::rtc::start_dispatcher();
		return true;
	} catch(const std::exception&){
		return false;
	}
}
}

int main(int argc, char** argv){
	const std::string mode = argc > 1 ? argv[1] : "supervisor";
	try{
		calls = 0;
		if(mode == "supervisor"){
			// workers are calls 1 and 2, the supervisor is call 3
			failFrom = failTo = 3;
			check(!starts(), "the dispatcher started without its supervisor");
		} else if(mode == "workers"){
			failFrom = 1; failTo = 2;
			check(!starts(), "the dispatcher started without any worker");
		} else if(mode == "one-worker"){
			failFrom = failTo = 1;
			check(starts(), "one failed worker must not stop the dispatcher");
			check(amber::rtc::dispatcher_workers() == 1, "expected the single started worker");
			std::cout << "PASS dispatcher-start one-worker" << std::endl;
			amber::rtc::stop_dispatcher();
			return 0;
		} else{
			throw std::runtime_error("unknown mode");
		}
		// the failure is not remembered, the next attempt starts normally
		failFrom = failTo = 0;
		check(starts(), "the dispatcher did not start after the threads became available");
		check(amber::rtc::dispatcher_workers() >= 1, "no worker after the retry");
		amber::rtc::stop_dispatcher();
		std::cout << "PASS dispatcher-start " << mode << std::endl;
		return 0;
	} catch(const std::exception& error){
		std::cerr << "FAIL " << error.what() << std::endl;
		return 1;
	}
}
