import sys
import time, struct, sys, logging, socket
import katcp_wrapper, log_handlers
import katadc
import argparse
import pyqtgraph as pg
import numpy as np
from pyqtgraph.Qt import QtCore, QtGui


if __name__ == '__main__':
    try:
        print('test...')
        sys.exit(1)
    except SystemExit:
        print('error')