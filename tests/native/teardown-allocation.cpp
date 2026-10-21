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


#include <cstdlib>
#include <new>
thread_local int failAt = 0;
thread_local bool allocationFailed = false;
void* operator new(size_t size){
	if(failAt > 0 && --failAt == 0){ allocationFailed = true; throw std::bad_alloc(); }
	if(auto* pointer = std::malloc(size ? size : 1)){ return pointer; }
	throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
#define main teardown_native_main
#include "teardown.cpp"
#undef main

int main(){
	try{
		for(int index = 1; index <= 2; ++index){
			{
				Pair pair;
				std::lock_guard operation(pair.a->operations);
				const auto budget = budget_used(), owned = managed_buffer_bytes();
				bool caught = false;
				failAt = index; allocationFailed = false;
				try{ pair.local[0]->send("rollback"); } catch(const std::bad_alloc&){ caught = true; }
				failAt = 0;
				check(caught && allocationFailed && pair.a->queues->sendMessages == 0, "admission allocation failure not atomic");
				check(budget_used() == budget && managed_buffer_bytes() == owned, "admission allocation leaked credit");
			}
			released([]{ return budget_used() == 0; });
		}
		for(int index = 1; index <= 3; ++index){
			{
				Pair pair;
				std::lock_guard operation(pair.a->operations);
				check(pair.local[0]->send("handoff"), "handoff admission rejected");
				failAt = index; allocationFailed = false;
				pair.a->service();
				failAt = 0;
				check(allocationFailed && pair.a->closed && pair.a->failure == ::rtc::PeerConnection::State::Failed, "backend allocation failure hidden");
				check(pair.a->queues->sendMessages == 0, "backend allocation failure kept entry");
			}
			released([]{ return budget_used() == 0 && managed_buffer_bytes() == 0; });
		}
		stop_dispatcher();
		std::cout << "PASS five real allocation boundaries, atomic rollback and exact escrow cleanup\n";
	}catch(const std::exception& error){ failAt = 0; std::cerr << error.what() << '\n'; return 1; }
	return 0;
}
