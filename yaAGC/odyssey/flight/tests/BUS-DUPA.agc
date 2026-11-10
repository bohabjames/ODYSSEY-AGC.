# ODYSSEY-AGC test ROM: BUS-DUPA
# Full duplex, seat A: sends set A, then receives and checks set B.
# See BUS-CORE.agc and BUS-DATA.agc.  License: GNU GPL v2 or later.

$BUS-DATA.agc

TXTAB		EQUALS	ATAB
RXTAB		EQUALS	BTAB

$BUS-CORE.agc

NTXW		DEC	12
NRXW		DEC	12
