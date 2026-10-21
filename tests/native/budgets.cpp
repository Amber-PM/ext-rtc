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

#include "../../src/channel/channel.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
namespace { thread_local bool failAllocation=false; }
void* operator new(size_t size){
	if(failAllocation){failAllocation=false;throw std::bad_alloc();}
	if(auto* p=std::malloc(size?size:1)){return p;}throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
using namespace amber::rtc;
void check(bool v,const char* why){if(!v){throw std::runtime_error(why);}}
int main(){
	try{
		auto baseline=managed_buffer_bytes();
		failAllocation=true;
		try{Buffer::copy("allocation rollback");throw std::runtime_error("allocation did not fail");}catch(const std::bad_alloc&){}
		check(managed_buffer_bytes()==baseline,"allocation budget leaked");
		std::array<Buffer*,1024> entries{};size_t count=0;
		std::string payload(262144,'x');bool exhausted=false;
		try{while(count<entries.size()){entries[count++]=Buffer::copy(payload);}}catch(const Error& e){exhausted=e.code==10;}
		check(exhausted,"global managed budget was not enforced");
		check(managed_buffer_bytes()<=67108864 && managed_buffer_bytes()>66846720,"global exact boundary");
		for(auto* entry:entries){Buffer::release(entry);}
		check(managed_buffer_bytes()==baseline,"global buffer credits not released");
		auto* empty=Buffer::copy("");check(managed_buffer_bytes()>baseline,"empty allocation uncharged");Buffer::release(empty);
		std::atomic<size_t> value{static_cast<size_t>((std::numeric_limits<intptr_t>::max)())-1};
		saturating_increment(value);saturating_increment(value);
		check(value==static_cast<size_t>((std::numeric_limits<intptr_t>::max)()),"counter wrapped");
		std::cout<<"PASS global budget exhaustion, allocation rollback, empty charge, release and counter saturation\n";
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
