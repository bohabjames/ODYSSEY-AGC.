# ODYSSEY-AGC test ROM: BUS-SEND
# Sends message set A (12 messages, SEQ 77773..00006 crossing the wrap); receives nothing.
# See BUS-CORE.agc and BUS-DATA.agc.  License: GNU GPL v2 or later.

$BUS-DATA.agc

TXTAB		EQUALS	ATAB
RXTAB		EQUALS	ATAB

$BUS-CORE.agc

NTXW		DEC	12
NRXW		DEC	0
