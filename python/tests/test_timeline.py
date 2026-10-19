from simavr_py.timeline import (
    PinTrace, bus_lane, describe_bus, format_time, measure, timer_output, write_vcd,
)


def test_trace_records_edges_only():
    t = PinTrace(10, False)
    t.record(20, False)             # repeated level
    t.record(30, True)
    t.record(25, False)             # out of order
    t.record(50, False)
    assert len(t) == 3
    assert t.level_at(5) is None and t.level_at(10) is False
    assert t.level_at(29) is False and t.level_at(30) is True and t.level_at(50) is False


def test_segments_clip_and_extend():
    t = PinTrace(10, False)
    t.record(30, True)
    t.record(50, False)
    assert t.segments(0, 100) == [(10, 30, False), (30, 50, True), (50, 100, False)]
    assert t.segments(20, 40) == [(20, 30, False), (30, 40, True)]
    assert t.segments(60, 70) == [(60, 70, False)]
    assert t.segments(0, 10) == []          # before the first sample
    assert t.segments(40, 40) == []


def test_trace_trims_old_history():
    t = PinTrace(0, False, limit=100)
    for i in range(1, 300):
        t.record(i, bool(i % 2))
    assert len(t) <= 100 and t.level_at(299) is True


def test_format_time():
    assert format_time(3_333_333, 3_333_333) == "1 s"
    assert format_time(1000, 1_000_000) == "1 ms"
    assert format_time(10, 1_000_000) == "10 µs"
    assert format_time(5, 0) == "5 cyc"


def test_measure():
    assert "cursor A" in measure(None, None, 1000)
    assert measure(100, None, 1000).endswith("B -")
    text = measure(1000, 3000, 1_000_000)
    assert "Δ 2 ms" in text and "500 Hz" in text
    assert "Hz" not in measure(5, 5, 1000)              # zero distance: no frequency


def test_write_vcd(tmp_path):
    a = PinTrace(0, False)
    a.record(1000, True)
    b = PinTrace(0, True)
    out = tmp_path / "t.vcd"
    write_vcd(str(out), {"PA0": a, "PA1": b}, 1_000_000, 3000, uart=[(2000, 0x68)])
    lines = out.read_text(encoding="utf-8").splitlines()
    assert "$var wire 1 ! PA0 $end" in lines and "$var wire 1 \" PA1 $end" in lines
    assert "$var wire 8 # uart0_tx $end" in lines
    body = lines[lines.index("$enddefinitions $end") + 1:]
    assert body == ["#0", "0!", "1\"", "#1000000", "1!", "#2000000", "b01101000 #", "#3000000"]


def test_timer_output_names():
    assert timer_output("TCA0.WO1") == ("tca", "0", 1)
    assert timer_output("TCB3.WO") == ("tcb", "3", 2)
    assert timer_output("TCD0.WOB") == ("tcd", "0", 1)
    for bad in ("PA0", "TCA0.WO3", "TCA0", "TCB0.WO1", "TCD0.WOC", "xTCA0.WO0"):
        assert timer_output(bad) is None


def test_vcd_interrupt_vector(tmp_path):
    out = tmp_path / "v.vcd"
    write_vcd(str(out), {}, 1_000_000, 10, vectors=[(5, 3)])
    text = out.read_text(encoding="utf-8")
    assert "$var wire 8 \" irq_vector $end" in text and "b00000011 \"" in text


def test_bus_lane_names():
    assert bus_lane("SPI0") == ("spi", "0", (("out", 1), ("in", 0)))
    assert bus_lane("TWI1")[:2] == ("twi", "1")
    assert bus_lane("USART2") == ("uar", "2", (("tx", 1), ("rx", 0)))
    assert bus_lane("USART0") == ("uar", "0", (("rx", 0),))      # TX is the built-in lane
    for bad in ("SPI", "SPI10", "PA0", "UART0", "TWI0.X"):
        assert bus_lane(bad) is None


def test_describe_bus():
    assert describe_bus("spi", 0x1A5) == "A5"
    assert describe_bus("uar", 0x68) == "h" and describe_bus("uar", 0x0A) == "0A"
    msg = lambda cond, addr, data: (cond << 8) | (addr << 16) | (data << 24)
    assert describe_bus("twi", msg(0x01, 0xA0, 0)) == "S A0"
    assert describe_bus("twi", msg(0x10, 0xA0, 0x3C)) == "W 3C"
    assert describe_bus("twi", msg(0x20, 0xA0, 0)) == "R"
    assert describe_bus("twi", msg(0x02, 0xA0, 1)) == "P"
    assert describe_bus("twi", msg(0x08, 0xA0, 1)) == "ACK 1"
    assert describe_bus("twi", msg(0x00, 0, 0)) == "?00"


def test_vcd_extra_signals(tmp_path):
    out = tmp_path / "e.vcd"
    write_vcd(str(out), {}, 1_000_000, 10, extra={"spi0_out": (8, [(2, 0xA5)]),
                                                  "twi0_in": (32, [(3, 1 << 8)])})
    text = out.read_text(encoding="utf-8")
    assert "$var wire 8 # spi0_out $end" in text and "$var wire 32 $ twi0_in $end" in text
    assert "b10100101 #" in text and "b" + format(1 << 8, "032b") + " $" in text
