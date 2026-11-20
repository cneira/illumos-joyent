#!/usr/bin/env python3
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License (CDDL), version 1.0.
# A full copy of the CDDL should have accompanied this source.
#
# Copyright 2026 Edgecast Cloud LLC.
"""Run inside an LX zone with lx_uinput to check independent gamepads."""

import errno
import fcntl
import os
import select
import struct


UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_SET_ABSBIT = 0x40045567
UI_DEV_CREATE = 0x5501
UI_DEV_SETUP = 0x405C5503
UI_GET_SYSNAME = 0x8008552C
EV_KEY = 1
EV_ABS = 3
BTN_SOUTH = 304
ABS_X = 0
EVENT = struct.Struct("@llHHi")
SETUP = struct.Struct("@HHHH80sI")


def create_gamepad(name):
    fd = os.open("/dev/uinput", os.O_RDWR)
    try:
        for request, value in (
            (UI_SET_EVBIT, EV_KEY),
            (UI_SET_EVBIT, EV_ABS),
            (UI_SET_KEYBIT, BTN_SOUTH),
            (UI_SET_ABSBIT, ABS_X),
        ):
            fcntl.ioctl(fd, request, value)
        fcntl.ioctl(fd, UI_DEV_SETUP, SETUP.pack(3, 1, 2, 1, name.encode(), 0))
        fcntl.ioctl(fd, UI_DEV_CREATE)
        return fd
    except Exception:
        os.close(fd)
        raise


def sysname(fd):
    name = bytearray(8)
    fcntl.ioctl(fd, UI_GET_SYSNAME, name)
    return name.split(b"\0", 1)[0]


def read_key(fd):
    ready, _, _ = select.select([fd], [], [], 2)
    assert ready, "controller event did not arrive"
    data = os.read(fd, EVENT.size)
    assert len(data) == EVENT.size
    return EVENT.unpack(data)[2:]


def main():
    assert EVENT.size == 24 and SETUP.size == 92
    first = second = first_event = second_event = replacement = None
    extra = []
    extra_events = []
    try:
        first = create_gamepad("LX controller A")
        second = create_gamepad("LX controller B")
        assert sysname(first) == b"input0"
        assert sysname(second) == b"input1"
        first_event = os.open("/dev/input/event0", os.O_RDONLY | os.O_NONBLOCK)
        second_event = os.open("/dev/input/event1", os.O_RDONLY | os.O_NONBLOCK)

        extra = [create_gamepad("LX controller C"),
                 create_gamepad("LX controller D")]
        assert [sysname(fd) for fd in extra] == [b"input2", b"input3"]
        extra_events = [os.open(f"/dev/input/event{index}",
                                os.O_RDONLY | os.O_NONBLOCK)
                        for index in (2, 3)]
        try:
            create_gamepad("LX controller E")
        except OSError as error:
            assert error.errno == errno.ENOSPC
        else:
            raise AssertionError("fifth controller unexpectedly succeeded")

        os.write(first, EVENT.pack(0, 0, EV_KEY, BTN_SOUTH, 1))
        assert read_key(first_event) == (EV_KEY, BTN_SOUTH, 1)
        assert not select.select([second_event], [], [], 0)[0]

        os.write(second, EVENT.pack(0, 0, EV_KEY, BTN_SOUTH, 1))
        assert read_key(second_event) == (EV_KEY, BTN_SOUTH, 1)
        assert not select.select([first_event], [], [], 0)[0]

        for index, (producer, event) in enumerate(zip(extra, extra_events)):
            os.write(producer, EVENT.pack(0, 0, EV_KEY, BTN_SOUTH, 1))
            assert read_key(event) == (EV_KEY, BTN_SOUTH, 1)
            others = [first_event, second_event, extra_events[1 - index]]
            assert not select.select(others, [], [], 0)[0]

        os.close(first_event)
        first_event = None
        os.close(first)
        first = None
        replacement = create_gamepad("LX controller replacement")
        assert sysname(replacement) == b"input0"
        assert sysname(second) == b"input1"
        print("multiple LX controllers: pass")
    finally:
        for fd in (first_event, second_event, *extra_events,
                   first, second, replacement, *extra):
            if fd is not None:
                os.close(fd)


if __name__ == "__main__":
    main()
