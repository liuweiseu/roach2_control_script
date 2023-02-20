#!/usr/bin/env python

import time, struct, sys, logging, socket
import katcp_wrapper, log_handlers
import katadc
import argparse
import sys
import pyqtgraph as pg
import numpy as np
from pyqtgraph.Qt import QtCore, QtGui

roach2_default = '192.168.100.128'
bitstream_default = 'italy_seti1_v1.172.bof'
port_default = 7147

beam_id = 1
src_ip_gbe0 = '192.168.16.223'
src_ip_gbe1 = '192.168.16.224'
src_port = 33333
multicast_ip_pol0 = '239.1.0.3'
multicast_ip_pol1 = '239.1.0.4'
dst_port = 12345

mac_base = (2<<40) + (2<<32)
spec_scope_names = ('AA', 'BB', 'CR', 'CI')

class attrdict(dict):
	def __init__(self, **kwargs):
		dict.__init__(self, **kwargs)
		self.__dict__ = self

opts = attrdict(
			nbins = 4 * 1024,
			fftshift = 0xFFFF,
			gain = 0x0100<<16 | 0x0100,
			acclen = 100,
			bitsel = 0b01010101,
			)

def init_10gbe(dev, ip, port, dest_ip, dest_port):
	ip_addr, = struct.unpack('!L',socket.inet_aton(ip))
	mac_addr = mac_base + ip_addr
	devname = dev + '_core'
	print('Initializing %s fabric mac: %s, ip: %s, port: %i ...' %
			(dev, ':'.join(("%012X" % mac_addr)[i:i+2] for i in range(0, 12, 2)), ip, port)),
	fpga.tap_start(dev, devname, mac_addr, ip_addr, port)
	print('done')
	dest_ip_addr, = struct.unpack('!L',socket.inet_aton(dest_ip))
	print('Configuring %s destination IP and port %s:%i ... ' %
			(dev, socket.inet_ntoa(struct.pack('!L', dest_ip_addr)), dest_port)),
	fpga.write_int(dev + '_dest_ip', dest_ip_addr)
	fpga.write_int(dev + '_dest_port', dest_port)
	print('done')
	# Workaround for tgtap:
	#   write destination ip address entry in arp table to all 0 mac address
	#   instead of broadcast address filled by tgtap
	fpga.write(devname, '\0'*8, 0x3000 + 8 * (dest_ip_addr & 0xFF))

#START OF MAIN:

if __name__ == '__main__':

	
	parser = argparse.ArgumentParser(description="Usage for ROACH2(MRO SETI) configuration")
	parser.add_argument("--ip",type=str, dest="roach",default=roach2_default,help="ip address of ROACH2 board")
	parser.add_argument("--port",type=int, dest="port",default=port_default,help="communication port of ROACH2 board")
	parser.add_argument("--bof",type=str, dest="bof",default=bitstream_default,help="bof file you want to upload to the SNAP board")
	parser.add_argument("--plot", dest="plot", action="store_true", default=False,help="Plot the adc data from snapshot.")
	args = parser.parse_args()
	
	lh = log_handlers.DebugLogHandler()
	logger = logging.getLogger(args.roach)
	logger.addHandler(lh)
	logger.setLevel(10)

	print('Connecting to server %s on port %i... ' % (args.roach, args.port))
	fpga = katcp_wrapper.FpgaClient(args.roach, args.port, timeout=10, logger=logger)
	time.sleep(0.1)

	if fpga.is_connected():
		print('ok')
	else:
		print('ERROR connecting to server %s on port %i.\r\n' % (args.roach,args.port))
		try:
			fpga.stop()
			sys.exit(1)
		except SystemExit:
			exit()

	print('-' * 20)

	print('Programming FPGA with  %s ... ' % args.bof),
	fpga.progdev(args.bof)
	print('done')


	print('Configuring spectrometer "%s" fft_shift, fft_shift=0x%X ... ' % ('u0', opts.fftshift)),
	fpga.write_int('u0' + '_fft_shift', int(opts.fftshift))
	print('done')

	print('Configuring spectrometer "%s" scale coefficients, gain=0x%X ... ' % ('u0', opts.gain)),
	fpga.write_int('u0' + '_gain', opts.gain) # in 16_8-16_8 format
	print('done')

	print('Configuring spectrometer "%s" bit selection, bit_select=0x%X ... ' % ('u0', opts.bitsel)),
	fpga.write_int('u0' + '_bit_select', opts.bitsel)
	print('done')

	print('Configuring spectrometer "%s" accumulation length, acc_len=%d ... ' % ('u0', opts.acclen)),
	fpga.write_int('u0' + '_acc_len',opts.acclen)
	print('done')
		
	# write beam id
	fpga.write_int('u0_beam_id', beam_id)

	fpga.write_int('use_tvg', 0b00)

	# We don't use xgbe0 and xgbe1 in the current design, 
	# so it doesn't matter what the src/dst ip addresses are set for them
	init_10gbe('xgbe0', '192.168.16.221', 33333, '239.2.0.1', 12345)
	init_10gbe('xgbe1', '192.168.16.222', 33333, '239.2.0.2', 12345)
	init_10gbe('xgbe2', '192.168.16.223', src_port, multicast_ip_pol1, dst_port)
	init_10gbe('xgbe3', '192.168.16.224', src_port, multicast_ip_pol0, dst_port)

	print('Issue reset signal...')
	fpga.write_int('reset', 0b00)
	fpga.write_int('reset', 0b11)
	print('done')
	fpga.stop()