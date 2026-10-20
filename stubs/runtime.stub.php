<?php
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

declare(strict_types=1);

namespace amber\rtc;

final class RtcRuntime{
	private function __construct(){}
	private function __clone(){}
	public static function getVersion() : string{ throw new \LogicException('Contract stub only'); }
	public static function getApiVersion() : int{ throw new \LogicException('Contract stub only'); }
	/** @return array{nativeBootstrap: bool, peerConnection: bool, dataChannels: bool, trickleIce: bool, observedRemoteFingerprint: bool, media: bool} */
	public static function getCapabilities() : array{ throw new \LogicException('Contract stub only'); }
	/** @return array{activePeers: int, activeChannels: int, managedBufferBytes: int, receiveOverflowCount: int, sendRejectedCount: int} */
	public static function getStats() : array{ throw new \LogicException('Contract stub only'); }
}
