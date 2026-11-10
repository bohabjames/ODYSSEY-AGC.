# ODYSSEY-AGC test ROM core: BUS-CORE
#
# Purpose:  Milestone 6 bus test program.  One program, parameterised by the
#           including file, that
#             1. TRANSMITS NTXW messages from table TXTAB (4 words per message:
#                TYPE, SEQ, DATA0, DATA1) using the ODYSSEY bus channels
#                0310 TYPE, 0311 SEQ, 0312 DATA0, 0313 DATA1, then 0314 COMMIT
#                (the COMMIT value is the message's SEQ);
#             2. RECEIVES NRXW messages: polls channel 0320 (bit 1 = VALID) with
#                real READ instructions, reads TYPE/SEQ/DATA0/DATA1 from channels
#                0321-0324, compares each word with the expected table RXTAB,
#                reports   TYPE SEQ DATA0 DATA1 ERR   on the audio-sample
#                channel 0300 (ERR = 0 if all four match, else 1; the 0300 use is
#                test-only so the host can capture the results), then writes the
#                SEQ to 0325 to acknowledge;
#             3. spins.
#
#           The including file supplies: TXTAB, RXTAB (via EQUALS to a table in
#           BUS-DATA.agc), and the constants NTXW and NRXW.
#
# NOTE:     Channels 0300-0325 are an ODYSSEY EXTENSION, not Apollo channels.
#
# License:  GNU GPL v2 or later (same as VirtualAGC).

Q		EQUALS	2			# Q register is address 2 (A=0, L=1, Q=2); RETURN = TC Q
IDX		EQUALS	0100			# erasable: table offset, 4 x message number
CNT		EQUALS	0101			# messages left
ERR		EQUALS	0102			# 0 = message matched, 1 = mismatch
OBT		EQUALS	0103			# observed TYPE
OBS		EQUALS	0104			# observed SEQ
OBD0		EQUALS	0105			# observed DATA0
OBD1		EQUALS	0106			# observed DATA1

$KEEPALIVE.agc

MAIN		RELINT				# interrupts on: T3RUPT keeps the watchdogs fed
		CAF	ZEROW
		TS	IDX
		CAF	NTXW
		TS	CNT

# ------------------------------------------------------------ transmit phase
TXTEST		CCS	CNT			# CNT > 0: A = CNT-1, fall to TXSEND
		TCF	TXSEND
		TCF	RXSTART			# CNT = +0: nothing (more) to send
		TCF	RXSTART
		TCF	RXSTART

TXSEND		TS	CNT			# CNT = CNT - 1
		INDEX	IDX
		CA	TXTAB			# TYPE
		EXTEND
		WRITE	0310
		INDEX	IDX
		CA	TXTAB +1		# SEQ
		EXTEND
		WRITE	0311
		INDEX	IDX
		CA	TXTAB +2		# DATA0
		EXTEND
		WRITE	0312
		INDEX	IDX
		CA	TXTAB +3		# DATA1
		EXTEND
		WRITE	0313
		INDEX	IDX
		CA	TXTAB +1		# COMMIT value = SEQ
		EXTEND
		WRITE	0314
		CA	IDX
		AD	FOURW
		TS	IDX
		TCF	TXTEST

# ------------------------------------------------------------- receive phase
RXSTART		CAF	ZEROW
		TS	IDX
		CAF	NRXW
		TS	CNT

RXTEST		CCS	CNT
		TCF	RXWAIT
		TCF	DONE
		TCF	DONE
		TCF	DONE

RXWAIT		TS	CNT			# CNT = CNT - 1
RXPOLL		EXTEND
		READ	0320			# receive status
		MASK	ONEW			# bit 1 = VALID
		CCS	A
		TCF	GOTMSG			# A > 0: a message is waiting
		TCF	RXPOLL			# A = +0: none yet
		TCF	RXPOLL
		TCF	RXPOLL

GOTMSG		CAF	ZEROW
		TS	ERR
		EXTEND
		READ	0321			# TYPE
		TS	OBT
		EXTEND
		READ	0322			# SEQ
		TS	OBS
		EXTEND
		READ	0323			# DATA0
		TS	OBD0
		EXTEND
		READ	0324			# DATA1
		TS	OBD1

		CS	OBT			# A = expected - observed; zero if equal
		INDEX	IDX
		AD	RXTAB
		TC	CHKZ
		CS	OBS
		INDEX	IDX
		AD	RXTAB +1
		TC	CHKZ
		CS	OBD0
		INDEX	IDX
		AD	RXTAB +2
		TC	CHKZ
		CS	OBD1
		INDEX	IDX
		AD	RXTAB +3
		TC	CHKZ

		CA	OBT			# report to the host (test-only use of 0300)
		EXTEND
		WRITE	0300
		CA	OBS
		EXTEND
		WRITE	0300
		CA	OBD0
		EXTEND
		WRITE	0300
		CA	OBD1
		EXTEND
		WRITE	0300
		CA	ERR
		EXTEND
		WRITE	0300

		CA	OBS			# acknowledge: write the SEQ back
		EXTEND
		WRITE	0325

		CA	IDX
		AD	FOURW
		TS	IDX
		TCF	RXTEST

DONE		CAF	ONEW			# non-TC instruction + TCF every pass
		TCF	DONE

# CHKZ: A is +0 or -0 (both mean equal) -> return; anything else -> ERR = 1.
CHKZ		CCS	A
		TCF	CHKBAD			# A > 0
		RETURN				# +0
		TCF	CHKBAD			# A < 0
		RETURN				# -0
CHKBAD		CAF	ONEW
		TS	ERR
		RETURN

ZEROW		OCT	00000
ONEW		OCT	00001
FOURW		OCT	00004
