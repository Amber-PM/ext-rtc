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


#define main teardown_native_main
#include "teardown.cpp"
#undef main
#include <fstream>

extern "C" {
#include "php.h"
extern zend_module_entry rtc_module_entry;
}


std::unique_ptr<Pair> requestPair;
std::weak_ptr<Escrow> requestFirst, requestSecond;
size_t requestDebt = 0, requestEscrow = 0;
bool requestEnded = false;

PHP_FUNCTION(teardown_probe_start){
	ZEND_PARSE_PARAMETERS_NONE();
	try{
		requestPair = std::make_unique<Pair>();
		const auto before = budget_used();
		requestPair->congest();
		{
			std::lock_guard lock(requestPair->a->queues->mutex);
			auto& q = *requestPair->a->queues;
			requestFirst = q.sends[q.first].credit;
			requestSecond = q.sends[(q.first + 1) % q.sends.size()].credit;
			requestEscrow = q.sends[q.first].escrow + q.sends[(q.first + 1) % q.sends.size()].escrow;
		}
		requestDebt = budget_used() - before;
		gateClose = true;
		std::cout << "armed real blocked SCTP escrow=" << requestEscrow << '\n';
	}catch(const std::exception& error){ std::cerr << error.what() << '\n'; std::abort(); }
}

PHP_RSHUTDOWN_FUNCTION(teardown_probe){
	try{
		check(requestPair && requestPair->a->closed && requestPair->b->closed, "rtc RSHUTDOWN did not precede probe");
		requestPair.reset();
		spin([]{ return closeEntered.load() && budget_used() == requestDebt; });
		check(!requestFirst.expired() && !requestSecond.expired(), "RSHUTDOWN released blocked escrow");
		requestEnded = true;
		std::cout << "RSHUTDOWN retained escrow=" << requestEscrow << " charged total=" << requestDebt << '\n';
		std::ofstream log(std::getenv("RTC_TEARDOWN_LOG"));
		log << "RSHUTDOWN retained escrow=" << requestEscrow << " charged=" << requestDebt << '\n';
		check(log.good(), "unable to record request evidence");
	}catch(const std::exception& error){ std::cerr << error.what() << '\n'; std::abort(); }
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(teardown_probe){
	try{
		check(requestEnded, "request did not finish");
		permitClose = true; permitClose.notify_all();
		spin([]{ return budget_used() == 0 && managed_buffer_bytes() == 0; });
		check(requestFirst.expired() && requestSecond.expired(), "native escrow owners survived completion");
		std::ofstream log(std::getenv("RTC_TEARDOWN_LOG"), std::ios::app);
		log << "MSHUTDOWN budget=0 owners=0 native release after RSHUTDOWN\n";
		check(log.good(), "unable to record completion evidence");
		std::cout << "PASS native release after RSHUTDOWN, budget=0, no Zend on native thread\n";
	}catch(const std::exception& error){ std::cerr << error.what() << '\n'; std::abort(); }
	return SUCCESS;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_teardown_start, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()
static const zend_function_entry teardown_functions[] = {
	PHP_FE(teardown_probe_start, arginfo_teardown_start)
	PHP_FE_END
};

extern "C" {
extern zend_module_entry teardown_probe_module_entry;
}
PHP_MINIT_FUNCTION(teardown_probe){
	try{
		retain_library(&rtc_module_entry);
		retain_library(&teardown_probe_module_entry);
	}catch(const std::exception& error){ std::cerr << error.what() << '\n'; return FAILURE; }
	return SUCCESS;
}
extern "C" {
zend_module_entry teardown_probe_module_entry = {
	STANDARD_MODULE_HEADER, "teardown_probe", teardown_functions,
	PHP_MINIT(teardown_probe), PHP_MSHUTDOWN(teardown_probe), nullptr,
	PHP_RSHUTDOWN(teardown_probe), nullptr, "private", STANDARD_MODULE_PROPERTIES
};
ZEND_GET_MODULE(teardown_probe)
}
