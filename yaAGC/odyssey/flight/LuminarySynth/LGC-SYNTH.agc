# ODYSSEY-AGC experimental program: LGC-SYNTH  (milestone 7)
#
# NOT Apollo flight software and NOT derived from Luminary 099 source; a
# separate experimental program for the LGC seat.  ODYSSEY EXTENSION channels
# as in CMC-SYNTH.agc.  See docs/SYNTH-PROTOCOL.md.
#
# ROLE: stochastic / modulation voice.  EVERY audio value is computed here by
# AGC instructions; the host only transports it.
#
#   * Noise source: 14-bit linear congruential generator
#         NZ = (1229 * NZ + 1013) mod 2^14            (full period, seed 1)
#     computed with MP (low word of the product) and TS overflow correction.
#   * Held noise: a new noise value is drawn every HSET samples; HSET is set by
#     the CMC's PITCH message from the note step:  HSET = HOLDTAB[step & 7].
#     (Lower notes hold the noise longer = a pitch-following sample-and-hold.)
#   * Envelope: a TRIGGER message sets ENV = velocity; ENV falls by 100 per
#     sample to 0.  Output sample = (ENV * HELD) >> 14   (MP, high word).
#   * SYNC message: the step number is recorded (no audible effect in M7).
#   * Every MODPER samples the LGC SENDS a MODULATION message (type 3) to the
#     CMC:  DATA0 = ((HELD * 400) >> 14) - 200   (signed, -200..+199),
#           DATA1 = NZ & 1                       (waveform select).
#   * Every received message is acknowledged; a status word is written to 0301
#     afterwards: TYPE if applied, TYPE | 040 if ignored.
#
# Per-sample order in one pass: [poll/apply message, write 0301] -> [noise] ->
# [output, WRITE 0300] -> [envelope decay] -> [modulation send] -> [delay].
#
# License:  GNU GPL v2 or later (same as VirtualAGC).

L		EQUALS	1
Q		EQUALS	2
NZ		EQUALS	0100			# LCG state
HELD		EQUALS	0101			# held noise value
HCNT		EQUALS	0102			# samples until the next noise draw
HSET		EQUALS	0103			# hold length in samples (>= 1)
ENV		EQUALS	0104			# envelope level 0..037777
SCNT		EQUALS	0105			# samples since the last MODULATION send
TSEQ		EQUALS	0106			# next transmit sequence number
RTYPE		EQUALS	0107
RSEQ		EQUALS	0110
RD0		EQUALS	0111
RD1		EQUALS	0112
SYNCN		EQUALS	0113			# last SYNC step number (recorded only)
MT		EQUALS	0114
MD0		EQUALS	0115
MD1		EQUALS	0116
TMP		EQUALS	0117

$KEEPALIVE.agc

MAIN		RELINT
		CAF	ZEROW
		TS	HELD
		TS	HCNT
		TS	ENV
		TS	SCNT
		TS	TSEQ
		TS	SYNCN
		CAF	ONEW
		TS	NZ			# seed
		TS	HSET			# hold 1 sample until the first PITCH

# ----------------------------------------------------------- poll the mailbox
LOOP		EXTEND
		READ	0320
		MASK	ONEW
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
		WRITE	0325			# acknowledge
		CS	RTYPE
		AD	TWOW			# 2 - TYPE
		CCS	A
		TCF	NOTPITCH
		TCF	ISPITCH
		TCF	NOTPITCH
		TCF	ISPITCH
NOTPITCH	CS	RTYPE
		AD	FOURW			# 4 - TYPE
		CCS	A
		TCF	NOTTRIG
		TCF	ISTRIG
		TCF	NOTTRIG
		TCF	ISTRIG
NOTTRIG		CS	RTYPE
		AD	ONEW			# 1 - TYPE
		CCS	A
		TCF	IGNORE
		TCF	ISSYNC
		TCF	IGNORE
		TCF	ISSYNC

ISPITCH		CA	RD1
		MASK	SEVENW			# note step 0..7
		TS	TMP
		INDEX	TMP
		CA	HOLDTAB
		TS	HSET
		TCF	APPLIED
ISTRIG		CA	RD0
		TS	ENV
		TCF	APPLIED
ISSYNC		CA	RD0
		TS	SYNCN
APPLIED		CA	RTYPE
		EXTEND
		WRITE	0301			# status: TYPE applied
		TCF	NOMSG
IGNORE		CA	RTYPE
		AD	IGNBITW
		EXTEND
		WRITE	0301			# status: TYPE | 040 = ignored

# ------------------------------------------------------------------- noise
NOMSG		CCS	HCNT
		TCF	HOLD			# > 0: A = HCNT-1, keep the held value
		TCF	NEWNZ
		TCF	NEWNZ
		TCF	NEWNZ
HOLD		TS	HCNT
		TCF	OUT
NEWNZ		CA	NZ
		EXTEND
		MP	LCGAW			# product: A = high word, L = low word
		CA	L			# low 14 bits of NZ * 1229
		AD	LCGCW			# + 1013, wraps mod 2^14 via TS
		TS	NZ
		TCF	+1
		CA	NZ			# (overflow path lands here too)
		TS	HELD
		CA	HSET
		AD	NEGONEW			# HCNT = HSET - 1
		TS	HCNT

# ------------------------------------------------------------------- output
OUT		CA	ENV
		EXTEND
		MP	HELD			# A = (ENV * HELD) >> 14
		EXTEND
		WRITE	0300			# the AGC-computed audio sample

# -------------------------------------------------------------- envelope decay
		CCS	ENV
		TCF	DECAY			# ENV > 0
		TCF	MODCHK
		TCF	MODCHK
		TCF	MODCHK
DECAY		CA	ENV
		AD	NDECW			# ENV - DEC
		TS	ENV
		CCS	ENV
		TCF	MODCHK			# still > 0
		TCF	ENVZERO
		TCF	ENVZERO
		TCF	ENVZERO
ENVZERO		CAF	ZEROW
		TS	ENV

# --------------------------------------------------- periodic MODULATION send
MODCHK		CA	SCNT
		AD	ONEW
		TS	SCNT
		CS	SCNT
		AD	MODPERW			# MODPER - SCNT
		CCS	A
		TCF	PACE
		TCF	DOMOD
		TCF	DOMOD
		TCF	DOMOD
DOMOD		CAF	ZEROW
		TS	SCNT
		CAF	THREEW
		TS	MT
		CA	HELD
		EXTEND
		MP	BSCALEW			# (HELD * 400) >> 14  = 0..399
		AD	NBOFFW			# - 200  = -200..199
		TS	MD0
		CA	NZ
		MASK	ONEW
		TS	MD1
		TC	SEND

PACE		CAF	DLYW
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
		WRITE	0314
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
SEVENW		OCT	00007
IGNBITW		OCT	00040
NEGONEW		OCT	77776			# -1
LCGAW		DEC	1229			# multiplier (= 1 mod 4)
LCGCW		DEC	1013			# increment (odd)
NDECW		DEC	-100			# envelope decay per sample
MODPERW		DEC	400			# samples between MODULATION sends
BSCALEW		DEC	400			# bend scale
NBOFFW		DEC	-200			# bend offset
DLYW		DEC	10			# pacing delay (tuned by measurement)

HOLDTAB		DEC	8			# hold length (samples) per note step 0..7
		DEC	7
		DEC	6
		DEC	4
		DEC	3
		DEC	2
		DEC	4
		DEC	6
