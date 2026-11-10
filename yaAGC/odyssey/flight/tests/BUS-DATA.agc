# ODYSSEY-AGC test data: BUS-DATA
#
# Two fixed message sets used by the milestone-6 bus test ROMs.  Each set is 12
# messages of four words: TYPE, SEQ, DATA0, DATA1.  The words are opaque 15-bit
# test patterns (no musical meaning yet).
#
#   ATAB: sequence numbers start at 77773, so the set crosses the 77777 -> 00000
#         wraparound.  Payloads include +0, -0, +max, -max and mixed bit patterns.
#   BTAB: sequence numbers start at 00100; types run 8 down to 5.
#
# Placed at 06000 (fixed-fixed memory) so it never collides with the program.
#
# NOTE:     ODYSSEY EXTENSION, not Apollo software.
# License:  GNU GPL v2 or later (same as VirtualAGC).

		SETLOC	6000

ATAB		OCT	00001			# msg 1
		OCT	77773
		OCT	00001
		OCT	77777
		OCT	00002			# msg 2
		OCT	77774
		OCT	12345
		OCT	00000
		OCT	00003			# msg 3
		OCT	77775
		OCT	37777
		OCT	40000
		OCT	00004			# msg 4
		OCT	77776
		OCT	65432
		OCT	00777
		OCT	00005			# msg 5
		OCT	77777
		OCT	01234
		OCT	54321
		OCT	00006			# msg 6  (SEQ wrapped to 0)
		OCT	00000
		OCT	77777
		OCT	00001
		OCT	00007			# msg 7
		OCT	00001
		OCT	20000
		OCT	17777
		OCT	00010			# msg 8
		OCT	00002
		OCT	00010
		OCT	00020
		OCT	00001			# msg 9
		OCT	00003
		OCT	31415
		OCT	27172
		OCT	00002			# msg 10
		OCT	00004
		OCT	77776
		OCT	77775
		OCT	00003			# msg 11
		OCT	00005
		OCT	00100
		OCT	07070
		OCT	00004			# msg 12
		OCT	00006
		OCT	40001
		OCT	36000

BTAB		OCT	00010			# msg 1
		OCT	00100
		OCT	11111
		OCT	22222
		OCT	00007			# msg 2
		OCT	00101
		OCT	33333
		OCT	44444
		OCT	00006			# msg 3
		OCT	00102
		OCT	55555
		OCT	66666
		OCT	00005			# msg 4
		OCT	00103
		OCT	77777
		OCT	00000
		OCT	00004			# msg 5
		OCT	00104
		OCT	01010
		OCT	10101
		OCT	00003			# msg 6
		OCT	00105
		OCT	02020
		OCT	20202
		OCT	00002			# msg 7
		OCT	00106
		OCT	03030
		OCT	30303
		OCT	00001			# msg 8
		OCT	00107
		OCT	04040
		OCT	40404
		OCT	00010			# msg 9
		OCT	00110
		OCT	05050
		OCT	50505
		OCT	00007			# msg 10
		OCT	00111
		OCT	06060
		OCT	60606
		OCT	00006			# msg 11
		OCT	00112
		OCT	07070
		OCT	70707
		OCT	00005			# msg 12
		OCT	00113
		OCT	00001
		OCT	00002
