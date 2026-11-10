# ODYSSEY-AGC test ROM: BUS-RECV
# Receives 12 messages, checks them against set A, acknowledges each, reports on channel 0300.
# See BUS-CORE.agc and BUS-DATA.agc.  License: GNU GPL v2 or later.

$BUS-DATA.agc

TXTAB		EQUALS	ATAB
RXTAB		EQUALS	ATAB

$BUS-CORE.agc

NTXW		DEC	0
NRXW		DEC	12
