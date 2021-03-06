"""Helper module for controlling the daemon (via protobuf).

Example use as a module:

from daemon_control import *

commands = [reg_read(MOD_CENTRAL, CENTRAL_STATE),
            reg_read(2, 14),
            reg_write(MOD_UDP, UDP_ENABLE, 0)]

responses = do_control_cmds(commands)

for i, rsp in enumerate(responses):
    print 'Response %d:' % i
    print '  * module:', rsp.reg_io.module
    print '  * value in hex: 0x%x' % rsp.reg_io.val
    print '  * Stringification:'
    print str(rsp),
    print
"""

from __future__ import print_function

import contextlib
from os.path import abspath, basename, dirname, join
import socket
import struct
import sys
import errno

import google.protobuf.text_format

# Tack the directory containing the protobuf serializers/deserializes
# onto sys.path.
script_file = abspath(__file__)
pyproto_dir = join(dirname(script_file), '..', 'build', 'pyproto', 'proto')
sys.path.append(pyproto_dir)

# Pull everything in from the generated protobuf module, for convenience
from control_pb2 import *

def reg_read(module, register):
    """Create a protocol message for reading a register."""
    reg_io = RegisterIO()
    reg_io.module = module
    if module == MOD_ERR:
        reg_io.err = register
    elif module == MOD_CENTRAL:
        reg_io.central = register
    elif module == MOD_SATA:
        reg_io.sata = register
    elif module == MOD_DAQ:
        reg_io.daq = register
    elif module == MOD_UDP:
        reg_io.udp = register
    elif module == MOD_GPIO:
        reg_io.gpio = register
    return ControlCommand(type=ControlCommand.REG_IO, reg_io=reg_io)

def reg_write(module, register, value):
    """Create a protocol message for reading a register."""
    reg_io = RegisterIO()
    reg_io.module = module
    reg_io.val=value
    if module == MOD_ERR:
        reg_io.err = register
    elif module == MOD_CENTRAL:
        reg_io.central = register
    elif module == MOD_SATA:
        reg_io.sata = register
    elif module == MOD_DAQ:
        reg_io.daq = register
    elif module == MOD_UDP:
        reg_io.udp = register
    elif module == MOD_GPIO:
        reg_io.gpio = register
    return ControlCommand(type=ControlCommand.REG_IO, reg_io=reg_io)

def read_err_regs():
    """Create and return protocol messages for reading error registers."""
    return [reg_read(mod, 0) for mod in
            (MOD_ERR, MOD_CENTRAL, MOD_SATA, MOD_DAQ, MOD_UDP, MOD_GPIO)]

def get_daemon_control_sock(addr=('127.0.0.1', 1371), retry=False,
                            max_retries=100):
    tries = max_retries if retry else 1
    while 1:
        try:
            sckt = socket.create_connection(('127.0.0.1', 1371))
        except socket.error:
            tries -= 1
            if tries:
                continue
            else:
                raise
        # Configure sckt to transmit a TCP segment as soon as data
        # is available, rather than wait for more more data
        sckt.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sckt

def recv(sckt, bufsize):
    while True:
        try:
            return sckt.recv(bufsize)
        except socket.error as se:
            # retry interrupted system calls
            if se.errno == errno.EINTR:
                continue
            else:
                raise

def send(sckt, string):
    while True:
        try:
            return sckt.send(string)
        except socket.error as se:
            # retry interrupted system calls
            if se.errno == errno.EINTR:
                continue
            else:
                raise

def do_control_cmds(commands, retry=False, max_retries=100,
                    control_socket=None):
    if control_socket is not None:
        sckt = control_socket
    else:
        try:
            sckt = get_daemon_control_sock(retry=retry,
                                           max_retries=max_retries)
        except socket.error as se:
            print("Can't open connection to daemon:", se,
                  file=sys.stderr)
            return None

    try:
        # Send each command, then wait for and get the response.
        responses = []
        for i, cmd in enumerate(commands):
            # Pack cmd into a protocol buffer.
            ser = cmd.SerializeToString()
            # Convert cmd's packed length to a network byte-order uint32, and
            # send that first.
            send(sckt, struct.pack('>l', len(ser)))
            # Then send packed cmd.
            send(sckt, ser)

            # Try to get the response's length as network byte-order uint32.
            resplen_net = recv(sckt, 4)
            if len(resplen_net) == 4:
                # Response length received. Convert it from network to host
                # byte ordering.
                resplen = struct.unpack('>l', resplen_net)[0]
                # Receive the response protocol buffer.
                pbuf_resp = recv(sckt, resplen)
            else:
                # No response length was received. Maybe the socket was closed?
                pbuf_resp = None

            # If we got a response, append it to the list.
            if pbuf_resp:
                rsp = ControlResponse()
                rsp.ParseFromString(pbuf_resp)
                responses.append(rsp)
            else:
                print("Didn't get response for command", i, file=sys.stderr)
                return None
        return responses
    finally:
        if control_socket is None:
            sckt.close()

def do_control_cmd(cmd, **kwargs):
    resps = do_control_cmds([cmd], **kwargs)
    return resps[0] if resps is not None else None

def get_err_regs(**kwargs):
    """Read error registers; return reg_io of results with nonzero values."""
    results = do_control_cmds(read_err_regs(), **kwargs)
    if results is None:
        return results
    else:
        return [r.reg_io for r in results if r.reg_io.val != 0]
