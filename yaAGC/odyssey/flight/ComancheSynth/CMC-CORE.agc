# ODYSSEY-AGC experimental program core: CMC-CORE  (milestone 7)
#
# Shared by CMC-SYNTH.agc (PNOTEOFS = 0) and CMC-SYNTH-ALT.agc (PNOTEOFS = 3).
# The including file supplies the constant word PNOTEOFS after this file.
# (The ALT build exists only so a test can show that changing what the CMC tells
# the LGC changes the LGC's modulation and therefore the CMC's own pitch.)
#
# NOT Apollo flight software and NOT derived from Comanche 055 source; it is a
# separate experimental program for the CMC seat.  ODYSSEY EXTENSION channels:
# 0300 audio sample, 0301 status word, 0310-0314 bus transmit, 0320-0325 bus
# receive/ack.  See docs/SYNTH-PROTOCOL.md for the message semantics.
#
# ROLE: primary tonal/rhythmic voice.  EVERY audio value is computed here by
# AGC instructions; the host only transports it.
#
#   * 14-bit phase accumulator: PHASE += (NOTEINC[STEP] + BEND) each sample,
#     wrapping mod 2^14 (TS overflow correction).  Output is the phase itself
#     (sawtooth) when WAVE = 0, or a square (0 / 037777) when WAVE = 1.
#   * Sequencer: STEP advances every STEPLEN samples through an 8-step table.
#     At each step the CMC SENDS three bus messages to the LGC:
#         SYNC     (type 1) DATA0 = step number, DATA1 = 0
#         PITCH    (type 2) DATA0 = phase increment of the note,
#                           DATA1 = (step + PNOTEOFS) & 7   (note index for the LGC)
#         TRIGGER  (type 4) DATA0 = velocity (0..037777),        DATA1 = step
#   * Each sample it polls the bus mailbox.  A MODULATION message (type 3) from
#     the LGC sets   BEND  = DATA0 (signed pitch offset, in phase-increment
#     units)   and   WAVE = DATA1 & 1.   Every message is acknowledged.  After
#     handling a message the CMC writes a STATUS word to channel 0301: the
#     message TYPE if it was applied, or TYPE | 040 if it was ignored.  The host
#     only records this word (tests use it to align cause and effect).
#
# Per-sample order inside one pass (a contract the tests rely on):
#     [poll/apply message, write 0301]  ->  [step logic / sends]  ->
#     [oscillator]  ->  [WRITE 0300 sample]  ->  [pacing delay]
# so a status word with an earlier cycle than a sample was applied before that
# sample was computed.
#
# License:  GNU GPL v2 or later (same as VirtualAGC).

Q		EQUALS	2
PHASE		EQUALS	0100			# oscillator phase, 0..037777
SCNT		EQUALS	0101			# samples since the current step began
STEP		EQUALS	0102			# sequencer step 0..7
BEND		EQUALS	0103			# signed pitch offset from MODULATION
WAVE		EQUALS	0104			# 0 = saw, 1 = square
INCEFF		EQUALS	0105			# phase increment actually used
RTYPE		EQUALS	0106			# received TYPE
RSEQ		EQUALS	0107			# received SEQ
RD0		EQUALS	0110			# received DATA0
RD1		EQUALS	0111			# received DATA1
TSEQ		EQUALS	0112			# next transmit sequence number
MT		EQUALS	0113			# message to send: TYPE
MD0		EQUALS	0114			#                  DATA0
MD1		EQUALS	0115			#                  DATA1

$KEEPALIVE.agc

MAIN		RELINT
		CAF	ZEROW
		TS	PHASE
		TS	BEND
		TS	WAVE
		TS	TSEQ
		CAF	NSTEPM1			# STEP = 7 so the first step becomes 0
		TS	STEP
		CAF	STEPLEN1		# SCNT = STEPLEN-1: first pass starts a step
		TS	SCNT

# ----------------------------------------------------------- poll the mailbox
LOOP		EXTEND
		READ	0320			# receive status
		MASK	ONEW			# bit 1 = VALID
		CCS	A
		TCF	GOTMSG
		TCF	NOMSG
		TCF	NOMSG
		TCF	NOMSG

GOTMSG		EXTEND
		READ	0321
		TS	RTYPE
		EXTEND
		READ	0322
		TS	RSEQ
		EXTEND
		READ	0323
		TS	RD0
		EXTEND
		READ	0324
		TS	RD1
		CA	RSEQ
		EXTEND
		WRITE	0325			# acknowledge (fields are already copied)
		CS	RTYPE
		AD	THREEW			# 3 - TYPE
		CCS	A
		TCF	IGNORE			# > 0
		TCF	ISMOD			# +0
		TCF	IGNORE			# < 0
		TCF	ISMOD			# -0
ISMOD		CA	RD0
		TS	BEND
		CA	RD1
		MASK	ONEW
		TS	WAVE
		CA	RTYPE
		EXTEND
		WRITE	0301			# status: MODULATION applied
		TCF	NOMSG
IGNORE		CA	RTYPE
		AD	IGNBITW
		EXTEND
		WRITE	0301			# status: TYPE | 040 = ignored

# ------------------------------------------------------------- sequencer step
NOMSG		CA	SCNT
		AD	ONEW
		TS	SCNT
		CS	SCNT
		AD	STEPLENW		# STEPLEN - SCNT
		CCS	A
		TCF	NOSTEP			# not yet
		TCF	DOSTEP
		TCF	DOSTEP
		TCF	DOSTEP

DOSTEP		CAF	ZEROW
		TS	SCNT
		CA	STEP
		AD	ONEW
		TS	STEP
		CS	STEP
		AD	NSTEPW			# NSTEPS - STEP
		CCS	A
		TCF	SENDALL			# still inside the table
		TCF	WRAPSTP
		TCF	WRAPSTP
		TCF	WRAPSTP
WRAPSTP		CAF	ZEROW
		TS	STEP

SENDALL		CAF	ONEW			# SYNC
		TS	MT
		CA	STEP
		TS	MD0
		CAF	ZEROW
		TS	MD1
		TC	SEND
		CAF	TWOW			# PITCH
		TS	MT
		INDEX	STEP
		CA	NOTEINC
		TS	MD0
		CA	STEP
		AD	PNOTEOFS		# note index the LGC sees = (STEP + PNOTEOFS) & 7
		MASK	SEVENW
		TS	MD1
		TC	SEND
		CAF	FOURW			# TRIGGER
		TS	MT
		INDEX	STEP
		CA	VELTAB
		TS	MD0
		CA	STEP
		TS	MD1
		TC	SEND

# ------------------------------------------------------------------ oscillator
NOSTEP		INDEX	STEP
		CA	NOTEINC
		AD	BEND
		TS	INCEFF
		CA	PHASE
		AD	INCEFF			# phase += increment (mod 2^14)
		TS	PHASE
		TCF	+1
		CA	PHASE			# (overflow path lands here too)
		CA	WAVE
		CCS	A
		TCF	SQUARE			# WAVE = 1
		TCF	OUTSAW
		TCF	OUTSAW
		TCF	OUTSAW
OUTSAW		CA	PHASE
		TCF	EMIT
SQUARE		CA	PHASE
		AD	NHALFW			# phase - 8192
		CCS	A
		TCF	HIGH
		TCF	HIGH
		TCF	LOW
		TCF	HIGH
HIGH		CAF	MAXW
		TCF	EMIT
LOW		CAF	ZEROW
EMIT		EXTEND
		WRITE	0300			# the AGC-computed audio sample
		CAF	DLYW
DELAY		CCS	A
		TCF	DELAY
		TCF	LOOP

# ------------------------------------------------- SEND: transmit MT, MD0, MD1
SEND		CA	MT
		EXTEND
		WRITE	0310
		CA	TSEQ
		EXTEND
		WRITE	0311
		CA	MD0
		EXTEND
		WRITE	0312
		CA	MD1
		EXTEND
		WRITE	0313
		CA	TSEQ
		EXTEND
		WRITE	0314			# commit (value = SEQ)
		CA	TSEQ
		AD	ONEW
		TS	TSEQ
		RETURN

# ---------------------------------------------------------------------- data
ZEROW		OCT	00000
ONEW		OCT	00001
TWOW		OCT	00002
THREEW		OCT	00003
FOURW		OCT	00004
IGNBITW		OCT	00040
SEVENW		OCT	00007
NHALFW		OCT	57777			# -8192 (one's complement of 020000)
MAXW		OCT	37777
STEPLENW	DEC	200			# samples per sequencer step
STEPLEN1	DEC	199
NSTEPW		DEC	8
NSTEPM1		DEC	7
DLYW		DEC	10			# pacing delay (tuned by measurement)

NOTEINC		DEC	1800			# phase increments per sample, 8 steps
		DEC	2025
		DEC	2250
		DEC	2700
		DEC	3000
		DEC	3600
		DEC	2700
		DEC	2250
VELTAB		DEC	16000			# TRIGGER velocities, 8 steps
		DEC	12000
		DEC	14000
		DEC	10000
		DEC	15000
		DEC	16383
		DEC	11000
		DEC	13000
