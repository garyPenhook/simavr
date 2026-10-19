from simavr_py.timeline import PinTrace, format_time, measure, write_vcd


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
