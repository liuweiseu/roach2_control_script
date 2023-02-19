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

beam_id = 0

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


def exit_clean():
	try:
		fpga.stop()
	except: pass
	exit()


def exit_fail(e):
	print('FAILURE DETECTED.')
	print('Exception:')
	print(e)
	print('Log entries:')
	lh.printMessages()
	exit_clean()


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


def split_snapshot(snap):
	len = snap['length']
	all = struct.unpack('%db'%len, snap['data'])
	segments = np.array(all).reshape(-1, 4)
	p0 = segments[0::2, :].flatten()
	p1 = segments[1::2, :].flatten()
	return p0, p1


def plot_anim():
	global fpga, plts, lines
	#for u in range(0, 2):
        for u in (0,0):
		unit = 'u%d' % u

		# ADC histogram
		print('zdok%d_scope' % u)
		snap = fpga.snapshot_get('zdok%d_scope' % u, man_trig=True, man_valid=True)
		adc0, adc1 = split_snapshot(snap)
		y, x = np.histogram(adc0, 100)
		lines[u*2 + 0].setData(x, y)
		y, x = np.histogram(adc1, 100)
		lines[u * 2 + 1].setData(x, y)

		# Spectrometer scope
		for i in range(2, 4):
			scopename = unit + '_x4_vacc_scope_' + spec_scope_names[i-2]
			print(scopename)
			snap = fpga.snapshot_get(scopename, man_valid=True)
			speclin = np.array(struct.unpack('>%dI' % (snap['length']/4), snap['data']))
			speclog = np.log2(speclin+1)
			lines[4*u + 2+i].setData(speclog)
			idx = np.argmax(speclog)

		for i in range(4, 6):
			scopename = unit + '_x4_vacc_scope_' + spec_scope_names[i-2]
			print(scopename)
			snap = fpga.snapshot_get(scopename, man_valid=True)
			speclin = np.array(struct.unpack('>%di' % (snap['length']/4), snap['data']))
			speclog = np.log2(np.fabs(speclin)+1)
			lines[4*u + 2+i].setData(speclog)
			idx = np.argmax(speclog)


#START OF MAIN:

if __name__ == '__main__':

	try:
		parser = argparse.ArgumentParser(description="Usage for ROACH2(MRO SETI) configuration")
		parser.add_argument('--skip', action='store_true', default=False, help='Skip programming FPGA')
		parser.add_argument("--ip",type=str, dest="roach",default=roach2_default,help="ip address of ROACH2 board")
		parser.add_argument("--port",type=int, dest="port",default=port_default,help="communication port of ROACH2 board")
		parser.add_argument("--bof",type=str, dest="bof",default=bitstream_default,help="bof file you want to upload to the SNAP board")
		parser.add_argument("--plot", dest="plot", action="store_true", default=False,help="Plot the adc data from snapshot.")
		args = parser.parse_args()

		
		lh = log_handlers.DebugLogHandler()
		logger = logging.getLogger(args.roach)
		logger.addHandler(lh)
		logger.setLevel(10)

		print('Connecting to server %s on port %i... \r\n' % (args.roach, args.port))
		fpga = katcp_wrapper.FpgaClient(args.roach, args.port, timeout=10, logger=logger)
		time.sleep(0.1)

		if fpga.is_connected():
			print('ok')
		else:
			print('ERROR connecting to server %s on port %i.\r\n' % (args.roach,args.port))
			sys.exit(1)

		print('-' * 20)

		if not args.skip:
			print('Programming FPGA with  %s ... ' % args.bof),
			fpga.progdev(args.bof)
			print('done')

		for unit in ('u0','u0'):
			print('Configuring spectrometer "%s" fft_shift, fft_shift=0x%X ... ' % (unit, opts.fftshift)),
			fpga.write_int(unit + '_fft_shift', int(opts.fftshift))
			print('done')

			print('Configuring spectrometer "%s" scale coefficients, gain=0x%X ... ' % (unit, opts.gain)),
			fpga.write_int(unit + '_gain', opts.gain) # in 16_8-16_8 format
			print('done')

			print('Configuring spectrometer "%s" bit selection, bit_select=0x%X ... ' % (unit, opts.bitsel)),
			fpga.write_int(unit + '_bit_select', opts.bitsel)
			print('done')

			print('Configuring spectrometer "%s" accumulation length, acc_len=%d ... ' % (unit, opts.acclen)),
			fpga.write_int(unit + '_acc_len',opts.acclen)
			print('done')
		
		# write beam id
		fpga.write_int('u0_beam_id', beam_id)

		fpga.write_int('use_tvg', 0b00)

		init_10gbe('xgbe0', '192.168.16.221', 33333, '239.2.3.1', 12345)
		init_10gbe('xgbe1', '192.168.16.222', 33333, '239.2.3.2', 12345)
		init_10gbe('xgbe2', '192.168.16.223', 33333, '239.1.2.3', 12345)
		init_10gbe('xgbe3', '192.168.16.224', 33333, '239.1.2.4', 12345)

		print('Issue reset signal...'),
		fpga.write_int('reset', 0b00)
		fpga.write_int('reset', 0b11)
		print('done')

		# set up the figure with a subplot to be plotted
		if(args.plot):
			win = pg.GraphicsWindow(title='MRO-beam')
			win.resize(1280, 800)
			plts = []
			lines = []
			for u in range(0, 2):
				unit = 'u%d' % u
				for i in range(0, 2):
					scopenum = i
					plt = win.addPlot(title=unit + ' ADC Hist %d' % scopenum)
					plts.append(plt)
					x=range(11)
					y=range(10)
					lines.append(plt.plot(x=x,y=y,stepMode=True, fillLevel=0, brush=(0,255,0,150)))
			win.nextRow()
			for u in range(0, 2):
				unit = 'u%d' % u
				for i in range(2, 4):
					scopenum = i - 2
					plt = win.addPlot(title=unit + ' SPEC Scope ' + spec_scope_names[scopenum])
					plts.append(plt)
					plt.showGrid(y=True)
					plt.setYRange(0, 32)
					plt.getAxis('left').setTicks([[(0, '0'), (8, '2^8'), (16, '2^16'), (24, '2^24'), (32, '2^32')]])
					lines.append(plt.plot())
			win.nextRow()
			for u in range(0, 2):
				unit = 'u%d' % u
				for i in range(4, 6):
					scopenum = i - 2
					plt = win.addPlot(title=unit + ' SPEC Scope ' + spec_scope_names[scopenum])
					plts.append(plt)
					plt.showGrid(y=True)
					plt.setYRange(0, 32)
					plt.getAxis('left').setTicks([[(0, '0'), (8, '2^8'), (16, '2^16'), (24, '2^24'), (32, '2^32')]])
					lines.append(plt.plot())

			print('Plot started.')
			plot_anim()
			QtGui.QApplication.instance().exec_()
	
	except SystemExit:
		fpga.stop()
	#except Exception as e:
	#	exit_fail(e)
	#finally:
	#	exit_clean()