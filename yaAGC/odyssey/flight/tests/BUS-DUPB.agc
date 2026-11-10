# ODYSSEY-AGC test ROM: BUS-DUPB
# Full duplex, seat B: sends set B, then receives and checks set A.
# See BUS-CORE.agc and BUS-DATA.agc.  License: GNU GPL v2 or later.

$BUS-DATA.agc

TXTAB		EQUALS	BTAB
RXTAB		EQUALS	ATAB

$BUS-CORE.agc

NTXW		DEC	12
NRXW		DEC	12
